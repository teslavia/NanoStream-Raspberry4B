#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <net.h>
#include "runtime_config.hpp"

struct Detection {
    int x, y, w, h;
    std::string label;
    float score;
    int class_id = -1;
};

class NCNNDetector {
public:
    struct DetectorConfig {
        struct Head {
            std::string cls;
            std::string reg;
            int stride = 0;
        };

        int frameWidth = 640;
        int frameHeight = 480;
        int inputWidth = 320;
        int inputHeight = 320;
        float baseScore = 0.30f;
        float minScoreSmallArea = 0.55f;
        float minScoreMediumArea = 0.45f;
        float minScoreSmallAreaThreshold = 0.02f;
        float minScoreMediumAreaThreshold = 0.05f;
        float capSmallAreaThreshold = 0.02f;
        float capMediumAreaThreshold = 0.08f;
        int minBoxArea = 400;
        int maxDetections = 6;
        int topK = 200;
        float iouThreshold = 0.3f;
        float emaAlpha = 0.6f;

        std::vector<Head> heads = {
            {"792", "795", 8},
            {"814", "817", 16},
            {"836", "839", 32}
        };
    };

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
    bool waitForFrame(std::vector<unsigned char>& frame, int& w, int& h);
    bool prepareInput(const std::vector<unsigned char>& frame, int w, int h, ncnn::Mat& in);
    void clearResults();
    void applyRuntimeOverrides(const RuntimeConfig& runtime);
    float calculateIoU(const Detection& a, const Detection& b) const;
    void applyPostFilter(const RuntimeConfig& runtime,
                         const std::vector<Detection>& raw_dets,
                         std::vector<Detection>& final_dets,
                         float frame_area) const;
    void smoothDetections(std::vector<Detection>& final_dets);
    float distExpect(const float* p, int bins) const;
    bool extractHeadOutputs(ncnn::Extractor& ex,
                            const std::string& cls,
                            const std::string& reg,
                            uint64_t frame_id,
                            bool debug,
                            ncnn::Mat& out_cls,
                            ncnn::Mat& out_reg) const;
    std::string formatDetectorConfig() const;

    ncnn::Net net;
    DetectorConfig config;
    std::thread worker_thread;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::atomic<bool> running{true};
    
    std::vector<unsigned char> pending_frame;
    int img_w = 0, img_h = 0;
    bool has_new_frame = false;

    std::atomic<int> throttle_ms{0};
    std::atomic<bool> paused{false};
    bool config_logged = false;

    // Detection results
    std::mutex result_mutex;
    std::vector<Detection> current_detections;
    std::vector<Detection> prev_detections;
};
