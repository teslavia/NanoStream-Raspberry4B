#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <net.h>

struct Detection {
    int x, y, w, h;
    std::string label;
    float score;
};

class NCNNDetector {
public:
    NCNNDetector();
    ~NCNNDetector();

    bool loadModel(const std::string &paramPath, const std::string &binPath);
    
    // Non-blocking: just drops the frame into the processing slot
    void pushFrame(const unsigned char *rgb_data, int width, int height);

    // Thread-safe access to latest results for OSD
    std::vector<Detection> getDetections();

    // Thermal throttling controls
    void setThrottle(int sleep_ms, bool paused);

private:
    void workerLoop();

    ncnn::Net net;
    std::thread worker_thread;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::atomic<bool> running{true};
    
    std::vector<unsigned char> pending_frame;
    int img_w = 0, img_h = 0;
    bool has_new_frame = false;

    std::atomic<int> throttle_ms{0};
    std::atomic<bool> paused{false};

    // Detection results
    std::mutex result_mutex;
    std::vector<Detection> current_detections;
};
