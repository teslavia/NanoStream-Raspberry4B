#include "ncnn_detector.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

NCNNDetector::NCNNDetector() {
    net.opt.num_threads = 4;
    net.opt.use_packing_layout = false; // safer for these heads
    worker_thread = std::thread(&NCNNDetector::workerLoop, this);
}

NCNNDetector::~NCNNDetector() {
    running = false;
    frame_cv.notify_all();
    if (worker_thread.joinable()) worker_thread.join();
    net.clear();
}

bool NCNNDetector::loadModel(const std::string &paramPath, const std::string &binPath) {
    if (net.load_param(paramPath.c_str()) == 0 && net.load_model(binPath.c_str()) == 0) {
        std::cout << "[AI] Precision Engine Ready." << std::endl;
        return true;
    }
    return false;
}

void NCNNDetector::pushFrame(const unsigned char *rgb_data, int width, int height) {
    std::unique_lock<std::mutex> lock(frame_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    if (!rgb_data || width <= 0 || height <= 0) return;

    size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    if (size == 0) return;
    if (pending_frame.size() != size) pending_frame.resize(size);
    memcpy(pending_frame.data(), rgb_data, size);
    
    img_w = width;
    img_h = height;
    has_new_frame = true;
    frame_cv.notify_one();
}

std::vector<Detection> NCNNDetector::getDetections() {
    std::lock_guard<std::mutex> lock(result_mutex);
    return current_detections;
}

void NCNNDetector::workerLoop() {
    struct State { int x, y, w, h; };
    std::vector<State> history(5, {0,0,0,0});
    const float alpha = 0.3f;
    uint64_t frame_id = 0;

    while (running) {
        std::vector<unsigned char> local_frame;
        int w, h;
        {
            std::unique_lock<std::mutex> lock(frame_mutex);
            frame_cv.wait(lock, [this]{ return has_new_frame || !running; });
            if (!running) break;
            local_frame = std::move(pending_frame);
            w = img_w; h = img_h;
            has_new_frame = false;
        }

        if (w <= 0 || h <= 0) continue;
        const size_t expected_size = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
        if (local_frame.size() < expected_size) continue;

        auto start = std::chrono::high_resolution_clock::now();
        ncnn::Mat in = ncnn::Mat::from_pixels(local_frame.data(), ncnn::Mat::PIXEL_BGR, w, h);
        const float mean_vals[3] = {103.53f, 116.28f, 123.675f};
        const float norm_vals[3] = {0.017429f, 0.017507f, 0.017125f};
        in.substract_mean_normalize(mean_vals, norm_vals);

        ncnn::Extractor ex = net.create_extractor();
        ex.set_light_mode(true);
        ex.input("input.1", in);
        
        // 定义三尺度节点 ID (Cls, Reg, Stride)
        struct Head { std::string cls; std::string reg; int stride; };
        std::vector<Head> heads = {
            {"792", "795", 8},  // Small
            {"814", "817", 16}, // Medium
            {"836", "839", 32}  // Large
        };

        std::vector<Detection> raw_dets;
        float max_score_all = -1e9f;
        bool any_head_ok = false;
        for (const auto& h : heads) {
            ncnn::Mat out_cls, out_reg;
            int cls_ret = ex.extract(h.cls.c_str(), out_cls);
            int reg_ret = ex.extract(h.reg.c_str(), out_reg);
            bool extracted = (cls_ret == 0 && reg_ret == 0 && !out_cls.empty() && !out_reg.empty());
            if (frame_id < 5) {
                std::cout << "\n[Diag] Head " << h.cls << "/" << h.reg
                          << " cls_ret=" << cls_ret << " reg_ret=" << reg_ret
                          << " cls_shape=" << out_cls.w << "x" << out_cls.h << "x" << out_cls.c
                          << " reg_shape=" << out_reg.w << "x" << out_reg.h << "x" << out_reg.c
                          << " cls_empty=" << out_cls.empty() << " reg_empty=" << out_reg.empty()
                          << std::endl;
            }
            if (!extracted) continue;

            // Layout handling: NanoDet-m (ncnn-assets) uses distribution regression (reg_max=7, 4*8 bins), and cls folded into w.
            if (out_cls.c == 1 && out_reg.c == 1 && out_reg.w % 4 == 0 && out_reg.h == out_cls.h) {
                int num_cls = out_cls.w;            // 80
                int locations = out_cls.h;          // 1600/400/100
                int bins = out_reg.w / 4;           // expect 8
                int feat_w = (int)(std::sqrt((float)locations) + 0.5f);
                if (feat_w <= 0) feat_w = locations;
                int feat_h = locations / feat_w;
                if (feat_w * feat_h != locations) { feat_w = locations; feat_h = 1; }

                auto dist_expect = [&](const float* p) {
                    float maxv = p[0];
                    for (int i = 1; i < bins; ++i) maxv = std::max(maxv, p[i]);
                    float sum = 0.f; float expv;
                    float expbuf[16];
                    for (int i = 0; i < bins; ++i) { expv = std::exp(p[i] - maxv); expbuf[i] = expv; sum += expv; }
                    float v = 0.f;
                    for (int i = 0; i < bins; ++i) v += (expbuf[i] / sum) * i;
                    return v;
                };

                any_head_ok = true;
                const int topk = 200;
                int kept = 0;
                for (int loc = 0; loc < locations; ++loc) {
                    const float* cls_ptr = out_cls.row(loc);
                    const float* reg_ptr = out_reg.row(loc);

                    float max_score = 0.f;
                    for (int c = 0; c < num_cls; ++c) max_score = std::max(max_score, cls_ptr[c]);
                    if (max_score > max_score_all) max_score_all = max_score;
                    if (max_score <= 0.35f) continue;
                    if (kept++ > topk) continue;

                    int gx = loc % feat_w;
                    int gy = loc / feat_w;
                    float l = dist_expect(reg_ptr + 0 * bins) * h.stride;
                    float t = dist_expect(reg_ptr + 1 * bins) * h.stride;
                    float r = dist_expect(reg_ptr + 2 * bins) * h.stride;
                    float b = dist_expect(reg_ptr + 3 * bins) * h.stride;

                    float scale_x = 640.0f / 320.0f;
                    float scale_y = 480.0f / 320.0f;
                    float cx = gx * h.stride;
                    float cy = gy * h.stride;

                    Detection d;
                    d.x = (int)((cx - l) * scale_x);
                    d.y = (int)((cy - t) * scale_y);
                    d.w = (int)((l + r) * scale_x);
                    d.h = (int)((t + b) * scale_y);
                    d.score = max_score;
                    d.label = "Target";
                    raw_dets.push_back(d);
                }
                continue;
            }

            // Fallback: original layout
            if (out_cls.w <= 0 || out_cls.h <= 0 || out_cls.c <= 0) continue;
            if (out_reg.c < 4) continue;
            any_head_ok = true;

            for (int i = 0; i < out_cls.w * out_cls.h; i++) {
                float max_logit = -1e9f;
                for (int c = 0; c < out_cls.c; c++) max_logit = std::max(max_logit, out_cls.channel(c)[i]);
                float score = 1.0f / (1.0f + std::exp(-max_logit));
                if (score > max_score_all) max_score_all = score;

                if (score > 0.35f) {
                    int gx = i % out_cls.w;
                    int gy = i / out_cls.w;
                    
                    float l = out_reg.channel(0)[i] * h.stride;
                    float t = out_reg.channel(1)[i] * h.stride;
                    float r = out_reg.channel(2)[i] * h.stride;
                    float b = out_reg.channel(3)[i] * h.stride;

                    float scale_x = 640.0f / 320.0f;
                    float scale_y = 480.0f / 320.0f;
                    float cx = gx * h.stride;
                    float cy = gy * h.stride;

                    Detection d;
                    d.x = (int)((cx - l) * scale_x);
                    d.y = (int)((cy - t) * scale_y);
                    d.w = (int)((l + r) * scale_x);
                    d.h = (int)((t + b) * scale_y);
                    d.score = score;
                    d.label = "Target";
                    raw_dets.push_back(d);
                }
            }
        }

        // NMS & Smoothing
        std::sort(raw_dets.begin(), raw_dets.end(), [](const Detection& a, const Detection& b){ return a.score > b.score; });
        std::vector<Detection> final_dets;
        for (const auto& d : raw_dets) {
            if (final_dets.size() >= 5) break;
            bool skip = false;
            for (const auto& f : final_dets) {
                if (std::abs(f.x - d.x) < 20 && std::abs(f.y - d.y) < 20) { skip = true; break; }
            }
            if (!skip) final_dets.push_back(d);
        }

        auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();
        frame_id++;
        if (!final_dets.empty()) {
            std::cout << "\r[NanoStream] Detected: " << final_dets.size() << " | Lat: " << lat << "ms    " << std::flush;
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = final_dets;
        } else {
            if (frame_id % 60 == 0) {
                std::cout << "\r[NanoStream] MaxScore: " << max_score_all
                          << " | HeadOK: " << (any_head_ok ? "Y" : "N")
                          << " | Lat: " << lat << "ms    " << std::flush;
            }
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections.clear();
        }
    }
}
