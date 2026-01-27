#include "ncnn_detector.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

NCNNDetector::NCNNDetector() {
    net.opt.num_threads = 3;
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
        std::cout << "[AI] Model loaded. Multi-object detection enabled." << std::endl;
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
        
        // 抓取两个不同尺度的输出头
        ncnn::Mat out1, out2;
        ex.extract("792", out1); // 小尺度 (对应特征图大，检测小目标)
        ex.extract("814", out2); // 中尺度 (检测较大型目标如人)

        std::vector<Detection> dets;
        auto process_head = [&](const ncnn::Mat& m, int stride) {
            for (int i = 0; i < m.w * m.h; i++) {
                float score = m[i];
                if (score > 0.40f) {
                    int grid_x = i % m.w;
                    int grid_y = i / m.w;
                    Detection d;
                    d.x = (grid_x * 640) / m.w;
                    d.y = (grid_y * 480) / m.h;
                    d.w = 120; d.h = 180; // 假定大小
                    d.score = score;
                    d.label = "Target";

                    // 简单的空间去重 (Primitive NMS)
                    bool duplicate = false;
                    for (const auto& existing : dets) {
                        if (std::abs(existing.x - d.x) < 50 && std::abs(existing.y - d.y) < 50) {
                            duplicate = true; break;
                        }
                    }
                    if (!duplicate) dets.push_back(d);
                }
            }
        };

        process_head(out1, 8);
        process_head(out2, 16);

        auto end = std::chrono::high_resolution_clock::now();
        auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // 仅在有重要发现时打印
        if (!dets.empty()) {
            static int frame_cnt = 0;
            if (frame_cnt++ % 10 == 0) {
                std::cout << "\n[NanoStream] Detected " << dets.size() << " objects | Latency: " << lat << "ms" << std::endl;
            }
        } else {
            std::cout << "\r[NanoStream] Tracking...    " << std::flush;
        }

        {
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = dets;
        }
    }
}
