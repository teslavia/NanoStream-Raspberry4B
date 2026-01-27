#include "ncnn_detector.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

NCNNDetector::NCNNDetector() {
    net.opt.num_threads = 4;
    net.opt.use_packing_layout = true;
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
        ncnn::Mat in = ncnn::Mat::from_pixels(local_frame.data(), ncnn::Mat::PIXEL_RGB, w, h);
        const float mean_vals[3] = {123.675f, 116.28f, 103.53f};
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
        float max_score_all = 0.0f;
        for (const auto& h : heads) {
            ncnn::Mat out_cls, out_reg;
            if (ex.extract(h.cls.c_str(), out_cls) != 0 || out_cls.empty()) continue;
            if (ex.extract(h.reg.c_str(), out_reg) != 0 || out_reg.empty()) continue;

            if (out_cls.w <= 0 || out_cls.h <= 0 || out_cls.c <= 0) continue;
            if (out_reg.c < 4) continue;

            for (int i = 0; i < out_cls.w * out_cls.h; i++) {
                float score = 0;
                for (int c = 0; c < out_cls.c; c++) score = std::max(score, out_cls.channel(c)[i]);
                if (score > max_score_all) max_score_all = score;

                if (score > 0.20f) {
                    int gx = i % out_cls.w;
                    int gy = i / out_cls.w;
                    
                    // 真实回归解码 (取 Reg 分支前 4 个通道作为 l,t,r,b 偏移)
                    float l = out_reg.channel(0)[i] * h.stride;
                    float t = out_reg.channel(1)[i] * h.stride;
                    float r = out_reg.channel(2)[i] * h.stride;
                    float b = out_reg.channel(3)[i] * h.stride;

                    Detection d;
                    // 映射到 640x480 (in 是 320x320)
                    float scale_x = 640.0f / 320.0f;
                    float scale_y = 480.0f / 320.0f;
                    
                    float cx = gx * h.stride;
                    float cy = gy * h.stride;
                    
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
                if (std::abs(f.x - d.x) < 30 && std::abs(f.y - d.y) < 30) { skip = true; break; }
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
                std::cout << "\r[NanoStream] MaxScore: " << max_score_all << " | Lat: " << lat << "ms    " << std::flush;
            }
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections.clear();
        }
    }
}
