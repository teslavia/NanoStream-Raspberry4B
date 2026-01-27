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
        std::cout << "[AI] Precision Tracking Engine Initialized." << std::endl;
        return true;
    }
    return false;
}

void NCNNDetector::pushFrame(const unsigned char *rgb_data, int width, int height) {
    std::unique_lock<std::mutex> lock(frame_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    
    size_t size = width * height * 3;
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
    // 用于 EMA 平滑的历史记录 (Persistent Box State)
    struct State { int x, y, w, h; bool active; };
    std::vector<State> history;
    const float alpha = 0.4f; // 平滑系数：越小越丝滑，越大越跟手

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

        auto start = std::chrono::high_resolution_clock::now();
        ncnn::Mat in = ncnn::Mat::from_pixels(local_frame.data(), ncnn::Mat::PIXEL_RGB, w, h);
        const float mean_vals[3] = {103.53f, 116.28f, 123.675f};
        const float norm_vals[3] = {0.017429f, 0.017507f, 0.017125f};
        in.substract_mean_normalize(mean_vals, norm_vals);

        ncnn::Extractor ex = net.create_extractor();
        ex.set_light_mode(true);
        ex.input("input.1", in);
        
        // 同时提取分类 (Cls) 和回归 (Reg) 分支
        // Stride 16 (Medium) 和 Stride 32 (Large)
        std::vector<int> cls_nodes = {814, 836};
        std::vector<int> reg_nodes = {817, 839};
        std::vector<int> strides = {16, 32};

        std::vector<Detection> raw_dets;
        for (size_t k = 0; k < cls_nodes.size(); k++) {
            ncnn::Mat out_cls, out_reg;
            ex.extract(std::to_string(cls_nodes[k]).c_str(), out_cls);
            ex.extract(std::to_string(reg_nodes[k]).c_str(), out_reg);

            int stride = strides[k];
            for (int i = 0; i < out_cls.w * out_cls.h; i++) {
                float score = 0;
                for (int c = 0; c < out_cls.c; c++) {
                    score = std::max(score, out_cls.channel(c)[i]);
                }

                if (score > 0.42f) { // 提高阈值，消除背景噪音
                    int gx = i % out_cls.w;
                    int gy = i / out_cls.w;
                    
                    // 解码回归：NanoDet 简化版解码 (取 reg 通道的近似均值)
                    float reg_val = out_reg.channel(0)[i] * stride;
                    
                    Detection d;
                    d.x = (gx * stride * 640) / 320;
                    d.y = (gy * stride * 480) / 320;
                    d.w = reg_val * 6.0f; // 动态宽度
                    d.h = reg_val * 8.0f; // 动态高度
                    d.score = score;
                    d.label = "Target";
                    
                    // 极简 IoU 去重
                    bool is_duplicate = false;
                    for (auto& r : raw_dets) {
                        if (std::abs(r.x - d.x) < 40 && std::abs(r.y - d.y) < 40) {
                            is_duplicate = true; break;
                        }
                    }
                    if (!is_duplicate) raw_dets.push_back(d);
                }
            }
        }

        // --- 时间平滑算法 (EMA) ---
        if (!raw_dets.empty()) {
            std::sort(raw_dets.begin(), raw_dets.end(), [](const Detection& a, const Detection& b){
                return a.score > b.score;
            });
            
            // 仅对前 3 个主要目标进行平滑处理
            size_t num = std::min(raw_dets.size(), (size_t)3);
            std::vector<Detection> smoothed;
            for (size_t i = 0; i < num; i++) {
                Detection d = raw_dets[i];
                if (history.size() <= i) {
                    history.push_back({d.x, d.y, d.w, d.h, true});
                } else {
                    history[i].x = (int)(alpha * d.x + (1.0f - alpha) * history[i].x);
                    history[i].y = (int)(alpha * d.y + (1.0f - alpha) * history[i].y);
                    history[i].w = (int)(alpha * d.w + (1.0f - alpha) * history[i].w);
                    history[i].h = (int)(alpha * d.h + (1.0f - alpha) * history[i].h);
                }
                d.x = history[i].x; d.y = history[i].y;
                d.w = history[i].w; d.h = history[i].h;
                smoothed.push_back(d);
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            std::cout << "\r[NanoStream] Tracking: " << smoothed.size() << " | Latency: " << lat << "ms    " << std::flush;

            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = smoothed;
        } else {
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections.clear();
            history.clear();
            std::cout << "\r[NanoStream] Scanning...                " << std::flush;
        }
    }
}
