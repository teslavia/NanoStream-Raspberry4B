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

void NCNNDetector::setThrottle(int sleep_ms, bool is_paused) {
    throttle_ms.store(sleep_ms);
    paused.store(is_paused);
}

void NCNNDetector::workerLoop() {
    struct State { float x, y, w, h; };
    State ema{0,0,0,0};
    bool ema_init = false;
    const float alpha = 0.65f;
    uint64_t frame_id = 0;

    static const char* kCoco80[] = {
        "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
        "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
        "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
        "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle",
        "wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange",
        "broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed",
        "dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven",
        "toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"
    };

    while (running) {
        if (paused.load()) {
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                current_detections.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

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

        int sleep_ms = throttle_ms.load();
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
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
        const char* debug_env = std::getenv("NANOSTREAM_DEBUG");
        bool debug = (debug_env && std::string(debug_env) == "1");

        auto iou = [](const Detection& a, const Detection& b) {
            int x1 = std::max(a.x, b.x);
            int y1 = std::max(a.y, b.y);
            int x2 = std::min(a.x + a.w, b.x + b.w);
            int y2 = std::min(a.y + a.h, b.y + b.h);
            int inter_w = std::max(0, x2 - x1);
            int inter_h = std::max(0, y2 - y1);
            int inter = inter_w * inter_h;
            int area_a = a.w * a.h;
            int area_b = b.w * b.h;
            int uni = area_a + area_b - inter;
            return uni > 0 ? (float)inter / (float)uni : 0.0f;
        };

        for (const auto& h : heads) {
            ncnn::Mat out_cls, out_reg;
            int cls_ret = ex.extract(h.cls.c_str(), out_cls);
            int reg_ret = ex.extract(h.reg.c_str(), out_reg);
            bool extracted = (cls_ret == 0 && reg_ret == 0 && !out_cls.empty() && !out_reg.empty());
            if (debug && frame_id < 5) {
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
                    int max_idx = 0;
                    for (int c = 0; c < num_cls; ++c) {
                        if (cls_ptr[c] > max_score) { max_score = cls_ptr[c]; max_idx = c; }
                    }
                    if (max_score > max_score_all) max_score_all = max_score;
                    if (max_score <= 0.30f) continue;
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
                    const char* label_env = std::getenv("NANOSTREAM_LABELS");
                    bool show_labels = !(label_env && std::string(label_env) == "0");
                    if (show_labels && max_idx >= 0 && max_idx < 80) {
                        d.label = kCoco80[max_idx];
                    } else {
                        d.label = "Target";
                    }
                    d.class_id = max_idx;
                    if (d.w * d.h < 400) continue;
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

                if (score > 0.30f) {
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
                    d.class_id = -1;
                    if (d.w * d.h < 400) continue;
                    raw_dets.push_back(d);
                }
            }
        }

        // NMS & Smoothing
        std::sort(raw_dets.begin(), raw_dets.end(), [](const Detection& a, const Detection& b){ return a.score > b.score; });
        std::vector<Detection> final_dets;
        int per_class_count[80] = {0};
        const float frame_area = 640.0f * 480.0f;
        const float small_thresh = 0.02f;  // <2% frame area
        const float medium_thresh = 0.08f; // <8% frame area
        int person_max = 2;
        if (const char* v = std::getenv("NANOSTREAM_PERSON_MAX")) {
            int parsed = std::atoi(v);
            if (parsed >= 0) person_max = parsed;
        }

        // Post-filter: suppress small false positives for person
        int max_person_area = 0;
        for (const auto& d : final_dets) {
            if (d.class_id == 0) {
                int area = d.w * d.h;
                if (area > max_person_area) max_person_area = area;
            }
        }

        if (max_person_area > 0) {
            float ratio = 0.6f;
            float min_score = 0.55f;
            float ar_min = 0.6f;
            float ar_max = 2.5f;
            if (const char* v = std::getenv("NANOSTREAM_PERSON_MIN_AREA_RATIO")) {
                float parsed = std::atof(v);
                if (parsed >= 0.0f && parsed <= 1.0f) ratio = parsed;
            }
            if (const char* v = std::getenv("NANOSTREAM_PERSON_MIN_SCORE")) {
                float parsed = std::atof(v);
                if (parsed >= 0.0f && parsed <= 1.0f) min_score = parsed;
            }
            if (const char* v = std::getenv("NANOSTREAM_PERSON_AR_MIN")) {
                float parsed = std::atof(v);
                if (parsed > 0.0f) ar_min = parsed;
            }
            if (const char* v = std::getenv("NANOSTREAM_PERSON_AR_MAX")) {
                float parsed = std::atof(v);
                if (parsed > 0.0f) ar_max = parsed;
            }
            std::vector<Detection> filtered;
            filtered.reserve(final_dets.size());
            for (const auto& d : final_dets) {
                if (d.class_id == 0) {
                    float ar = d.h > 0 ? (float)d.h / (float)d.w : 0.0f;
                    if (d.w * d.h < (int)(max_person_area * ratio)) continue;
                    if (d.score < min_score) continue;
                    if (ar < ar_min || ar > ar_max) continue;
                }
                filtered.push_back(d);
            }
            final_dets.swap(filtered);
        }
        for (const auto& d : raw_dets) {
            if (final_dets.size() >= 6) break;
            if (d.class_id >= 0 && d.class_id < 80) {
                float area_norm = (d.w * d.h) / frame_area;
                int cap = (area_norm < small_thresh) ? 1 : (area_norm < medium_thresh ? 2 : 3);
                if (d.class_id == 0 && person_max < cap) cap = person_max;
                if (per_class_count[d.class_id] >= cap) continue;
            }
            bool skip = false;
            for (const auto& f : final_dets) {
                if (d.class_id >= 0 && f.class_id >= 0 && d.class_id != f.class_id) continue;
                if (iou(d, f) > 0.3f) { skip = true; break; }
            }
            if (!skip) {
                if (d.class_id >= 0 && d.class_id < 80) per_class_count[d.class_id] += 1;
                final_dets.push_back(d);
            }
        }

        auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();
        frame_id++;
        if (!final_dets.empty()) {
            // EMA smoothing on top-1
            if (!ema_init) {
                ema = { (float)final_dets[0].x, (float)final_dets[0].y, (float)final_dets[0].w, (float)final_dets[0].h };
                ema_init = true;
            } else {
                ema.x = alpha * final_dets[0].x + (1.0f - alpha) * ema.x;
                ema.y = alpha * final_dets[0].y + (1.0f - alpha) * ema.y;
                ema.w = alpha * final_dets[0].w + (1.0f - alpha) * ema.w;
                ema.h = alpha * final_dets[0].h + (1.0f - alpha) * ema.h;
            }
            final_dets[0].x = (int)ema.x;
            final_dets[0].y = (int)ema.y;
            final_dets[0].w = (int)ema.w;
            final_dets[0].h = (int)ema.h;
            std::cout << "\r[NanoStream] Detected: " << final_dets.size() << " | Lat: " << lat << "ms    " << std::flush;
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = final_dets;
        } else {
            if (debug && frame_id % 60 == 0) {
                std::cout << "\r[NanoStream] MaxScore: " << max_score_all
                          << " | HeadOK: " << (any_head_ok ? "Y" : "N")
                          << " | Lat: " << lat << "ms    " << std::flush;
            }
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections.clear();
        }
    }
}
