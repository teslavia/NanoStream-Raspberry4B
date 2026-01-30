#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sstream>

#include "ncnn_detector.hpp"
#include "runtime_config.hpp"

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

bool NCNNDetector::waitForFrame(std::vector<unsigned char>& frame, int& w, int& h) {
    std::unique_lock<std::mutex> lock(frame_mutex);
    frame_cv.wait(lock, [this]{ return has_new_frame || !running; });
    if (!running) return false;
    frame = std::move(pending_frame);
    w = img_w;
    h = img_h;
    has_new_frame = false;
    return true;
}

bool NCNNDetector::prepareInput(const std::vector<unsigned char>& frame, int w, int h, ncnn::Mat& in) {
    if (w <= 0 || h <= 0) return false;
    const size_t expected_size = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (frame.size() < expected_size) return false;

    in = ncnn::Mat::from_pixels(frame.data(), ncnn::Mat::PIXEL_BGR, w, h);
    const float mean_vals[3] = {103.53f, 116.28f, 123.675f};
    const float norm_vals[3] = {0.017429f, 0.017507f, 0.017125f};
    in.substract_mean_normalize(mean_vals, norm_vals);
    return true;
}

void NCNNDetector::clearResults() {
    std::lock_guard<std::mutex> lock(result_mutex);
    current_detections.clear();
    prev_detections.clear();
}

std::string NCNNDetector::formatDetectorConfig() const {
    std::ostringstream out;
    out << "det_frame_w=" << config.frameWidth
        << " det_frame_h=" << config.frameHeight
        << " det_input_w=" << config.inputWidth
        << " det_input_h=" << config.inputHeight
        << " det_base_score=" << config.baseScore
        << " det_min_score_small=" << config.minScoreSmallArea
        << " det_min_score_med=" << config.minScoreMediumArea
        << " det_area_small=" << config.minScoreSmallAreaThreshold
        << " det_area_med=" << config.minScoreMediumAreaThreshold
        << " det_cap_area_small=" << config.capSmallAreaThreshold
        << " det_cap_area_med=" << config.capMediumAreaThreshold
        << " det_min_box_area=" << config.minBoxArea
        << " det_max_det=" << config.maxDetections
        << " det_topk=" << config.topK
        << " det_iou=" << config.iouThreshold
        << " det_ema=" << config.emaAlpha;

    out << " det_heads=";
    for (size_t i = 0; i < config.heads.size(); ++i) {
        const auto& h = config.heads[i];
        if (i > 0) out << ",";
        out << h.cls << ":" << h.reg << ":" << h.stride;
    }
    return out.str();
}

void NCNNDetector::applyRuntimeOverrides(const RuntimeConfig& runtime) {
    if (runtime.detInputWidth > 0) config.inputWidth = runtime.detInputWidth;
    if (runtime.detInputHeight > 0) config.inputHeight = runtime.detInputHeight;
    if (runtime.detTopK > 0) config.topK = runtime.detTopK;
    if (runtime.detMaxDetections > 0) config.maxDetections = runtime.detMaxDetections;
    if (runtime.detMinBoxArea > 0) config.minBoxArea = runtime.detMinBoxArea;
    if (runtime.detBaseScore > 0.0f) config.baseScore = runtime.detBaseScore;
    if (runtime.detMinScoreSmallArea > 0.0f) config.minScoreSmallArea = runtime.detMinScoreSmallArea;
    if (runtime.detMinScoreMediumArea > 0.0f) config.minScoreMediumArea = runtime.detMinScoreMediumArea;
    if (runtime.detMinScoreSmallAreaThreshold > 0.0f) {
        config.minScoreSmallAreaThreshold = runtime.detMinScoreSmallAreaThreshold;
    }
    if (runtime.detMinScoreMediumAreaThreshold > 0.0f) {
        config.minScoreMediumAreaThreshold = runtime.detMinScoreMediumAreaThreshold;
    }
    if (runtime.detCapSmallAreaThreshold > 0.0f) {
        config.capSmallAreaThreshold = runtime.detCapSmallAreaThreshold;
    }
    if (runtime.detCapMediumAreaThreshold > 0.0f) {
        config.capMediumAreaThreshold = runtime.detCapMediumAreaThreshold;
    }
    if (runtime.detIouThreshold > 0.0f) config.iouThreshold = runtime.detIouThreshold;
    if (runtime.detEmaAlpha > 0.0f) config.emaAlpha = runtime.detEmaAlpha;

    if (!runtime.detHeads.empty()) {
        std::vector<DetectorConfig::Head> parsed;
        size_t start = 0;
        while (start < runtime.detHeads.size()) {
            size_t end = runtime.detHeads.find(',', start);
            std::string token = runtime.detHeads.substr(start, end - start);
            size_t p1 = token.find(':');
            size_t p2 = token.find(':', p1 == std::string::npos ? p1 : p1 + 1);
            if (p1 != std::string::npos && p2 != std::string::npos) {
                DetectorConfig::Head h;
                h.cls = token.substr(0, p1);
                h.reg = token.substr(p1 + 1, p2 - p1 - 1);
                h.stride = std::atoi(token.substr(p2 + 1).c_str());
                if (!h.cls.empty() && !h.reg.empty() && h.stride > 0) {
                    parsed.push_back(h);
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (!parsed.empty()) {
            config.heads = parsed;
        }
    }
}


void NCNNDetector::workerLoop() {
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
        const RuntimeConfig& runtime = getRuntimeConfig();
        applyRuntimeOverrides(runtime);
        if (runtime.debug && !config_logged) {
            std::cout << "[NanoStream] Detector config: " << formatDetectorConfig() << std::endl;
            config_logged = true;
        }
        if (paused.load()) {
            clearResults();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        std::vector<unsigned char> local_frame;
        int w = 0, h = 0;
        if (!waitForFrame(local_frame, w, h)) break;

        int sleep_ms = throttle_ms.load();
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }

        ncnn::Mat in;
        if (!prepareInput(local_frame, w, h, in)) continue;

        auto start = std::chrono::high_resolution_clock::now();

        ncnn::Extractor ex = net.create_extractor();
        ex.set_light_mode(true);
        ex.input("input.1", in);
        
        const float frame_area = static_cast<float>(config.frameWidth) * config.frameHeight;
        std::vector<Detection> raw_dets;
        float max_score_all = -1e9f;
        bool any_head_ok = false;
        bool debug = runtime.debug;

        for (const auto& h : config.heads) {
            ncnn::Mat out_cls, out_reg;
            if (!extractHeadOutputs(ex, h.cls, h.reg, frame_id, debug, out_cls, out_reg)) continue;

            // Layout handling: NanoDet-m (ncnn-assets) uses distribution regression (reg_max=7, 4*8 bins), and cls folded into w.
            if (out_cls.c == 1 && out_reg.c == 1 && out_reg.w % 4 == 0 && out_reg.h == out_cls.h) {
                int num_cls = out_cls.w;            // 80
                int locations = out_cls.h;          // 1600/400/100
                int bins = out_reg.w / 4;           // expect 8
                int feat_w = (int)(std::sqrt((float)locations) + 0.5f);
                if (feat_w <= 0) feat_w = locations;
                int feat_h = locations / feat_w;
                if (feat_w * feat_h != locations) { feat_w = locations; feat_h = 1; }

                any_head_ok = true;
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
                    if (max_score <= config.baseScore) continue;
                    if (kept++ > config.topK) continue;

                    int gx = loc % feat_w;
                    int gy = loc / feat_w;
                    float l = distExpect(reg_ptr + 0 * bins, bins) * h.stride;
                    float t = distExpect(reg_ptr + 1 * bins, bins) * h.stride;
                    float r = distExpect(reg_ptr + 2 * bins, bins) * h.stride;
                    float b = distExpect(reg_ptr + 3 * bins, bins) * h.stride;

                    float scale_x = static_cast<float>(config.frameWidth) / config.inputWidth;
                    float scale_y = static_cast<float>(config.frameHeight) / config.inputHeight;
                    float cx = gx * h.stride;
                    float cy = gy * h.stride;

                    Detection d;
                    d.x = (int)((cx - l) * scale_x);
                    d.y = (int)((cy - t) * scale_y);
                    d.w = (int)((l + r) * scale_x);
                    d.h = (int)((t + b) * scale_y);
                    d.score = max_score;
                    if (runtime.showLabels && max_idx >= 0 && max_idx < 80) {
                        d.label = kCoco80[max_idx];
                    } else {
                        d.label = "Target";
                    }
                    d.class_id = max_idx;
                    float area_norm = (d.w * d.h) / frame_area;
                    float min_score = config.baseScore;
                    if (area_norm < config.minScoreSmallAreaThreshold) min_score = config.minScoreSmallArea;
                    else if (area_norm < config.minScoreMediumAreaThreshold) min_score = config.minScoreMediumArea;
                    if (d.class_id == 0) {
                        if (min_score < runtime.personMinScore) min_score = runtime.personMinScore;
                    }
                    if (d.score < min_score) continue;
                    if (d.w * d.h < config.minBoxArea) continue;
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

                if (score > config.baseScore) {
                    int gx = i % out_cls.w;
                    int gy = i / out_cls.w;
                    
                    float l = out_reg.channel(0)[i] * h.stride;
                    float t = out_reg.channel(1)[i] * h.stride;
                    float r = out_reg.channel(2)[i] * h.stride;
                    float b = out_reg.channel(3)[i] * h.stride;

                    float scale_x = static_cast<float>(config.frameWidth) / config.inputWidth;
                    float scale_y = static_cast<float>(config.frameHeight) / config.inputHeight;
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
                    float area_norm = (d.w * d.h) / frame_area;
                    float min_score = config.baseScore;
                    if (area_norm < config.minScoreSmallAreaThreshold) min_score = config.minScoreSmallArea;
                    else if (area_norm < config.minScoreMediumAreaThreshold) min_score = config.minScoreMediumArea;
                    if (d.score < min_score) continue;
                    if (d.w * d.h < config.minBoxArea) continue;
                    raw_dets.push_back(d);
                }
            }
        }

        // NMS & Smoothing
        std::vector<Detection> final_dets;
        applyPostFilter(runtime, raw_dets, final_dets, frame_area);

        auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();
        frame_id++;
        if (!final_dets.empty()) {
            // Multi-target EMA smoothing with IOU association
            smoothDetections(final_dets);
            std::cout << "\r[NanoStream] Detected: " << final_dets.size() << " | Lat: " << lat << "ms    " << std::flush;
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = final_dets;
            prev_detections = final_dets;
        } else {
            if (debug && frame_id % 60 == 0) {
                std::cout << "\r[NanoStream] MaxScore: " << max_score_all
                          << " | HeadOK: " << (any_head_ok ? "Y" : "N")
                          << " | Lat: " << lat << "ms    " << std::flush;
            }
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections.clear();
            prev_detections.clear();
        }
    }
}
