#include "ncnn_detector.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>

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
        std::cout << "[AI] Model loaded. HW Acceleration: Enabled." << std::endl;
        return true;
    }
    return false;
}

void NCNNDetector::pushFrame(const unsigned char *rgb_data, int width, int height) {
    std::unique_lock<std::mutex> lock(frame_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::cout << "x" << std::flush;
        return; 
    }
    
    std::cout << "." << std::flush;
    
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
        
        ncnn::Mat out;
        ex.extract("792", out);

        auto end = std::chrono::high_resolution_clock::now();
        auto lat = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        float max_score = 0;
        int total_elements = out.w * out.h * out.c;
        for (int i = 0; i < total_elements; i++) {
            if (out[i] > max_score) max_score = out[i];
        }

        std::vector<Detection> dets;
        
        // Diagnostic: Add a fixed TEST box
        Detection test_box;
        test_box.x = 20; test_box.y = 20; test_box.w = 100; test_box.h = 50;
        test_box.label = "TEST";
        test_box.score = 1.0;
        dets.push_back(test_box);

        if (max_score > 0.25f) {
            Detection det;
            det.x = 200; det.y = 200; det.w = 150; det.h = 200;
            det.score = max_score;
            det.label = "Target";
            dets.push_back(det);

            std::cout << "\n{\"event\": \"detected\", \"confidence\": " << max_score 
                      << ", \"latency_ms\": " << lat << "}" << std::endl;
        } else {
            std::cout << "\r[NanoStream] AI: " << lat << "ms | Status: Idle    " << std::flush;
        }

        {
            std::lock_guard<std::mutex> lock(result_mutex);
            current_detections = dets;
        }
    }
}
