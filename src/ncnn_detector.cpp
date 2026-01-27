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
        std::cout << "[AI] Model loaded. High-sensitivity mode active." << std::endl;
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
        
        // 抓取全尺度分类输出头 (对应 Strides: 8, 16, 32)
        std::vector<std::pair<std::string, int>> heads = {
            {"792", 8},  // Small Objects (e.g. cup, phone)
            {"814", 16}, // Medium Objects
            {"836", 32}  // Large Objects (e.g. person)
        };

        std::vector<Detection> dets;
        for (auto& head : heads) {
            ncnn::Mat out;
            if (ex.extract(head.first.c_str(), out) != 0) continue;

            int stride = head.second;
            int num_grid = out.w * out.h;
            int num_class = out.c;

            for (int i = 0; i < num_grid; i++) {
                // 在所有类别中寻找最高分
                float max_cls_score = 0;
                for (int c = 0; c < num_class; c++) {
                    float score = out.channel(c)[i];
                    if (score > max_cls_score) max_cls_score = score;
                }

                if (max_cls_score > 0.25f) { // 灵敏度阈值下调
                    int grid_x = i % out.w;
                    int grid_y = i / out.w;
                    
                    Detection d;
                    d.x = (grid_x * 640) / out.w;
                    d.y = (grid_y * 480) / out.h;
                    // 动态调整框大小：根据步长(Stride)粗略估计目标大小
                    d.w = stride * 6; 
                    d.h = stride * 10;
                    d.score = max_cls_score;
                    d.label = "Target";

                    // 空间抑制 (NMS 简化版)
                    bool duplicate = false;
                    for (const auto& existing : dets) {
                        if (std::abs(existing.x - d.x) < (stride * 2) && 
                            std::abs(existing.y - d.y) < (stride * 2)) {
                            duplicate = true; break;
                        }
                    }
                    if (!duplicate) dets.push_back(d);
                }
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // 仅在发现目标时静默打印数量，避免刷屏
        if (!dets.empty()) {
            static int frame_cnt = 0;
            if (frame_cnt++ % 15 == 0) {
                std::cout << "\r[NanoStream] Detected: " << dets.size() << " | Latency: " << lat << "ms    " << std::flush;
            }
        } else {
            std::cout << "\r[NanoStream] Scanning... Latency: " << lat << "ms    " << std::flush;
        }

        {
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = dets;
        }
    }
}
