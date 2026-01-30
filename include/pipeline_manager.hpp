#pragma once

#include <string>
#include <gst/gst.h>
#include <cairo.h>
#include "ncnn_detector.hpp"

class PipelineManager {
public:
    struct PipelineConfig {
        int width = 640;
        int height = 480;
        int framerate_num = 15;
        int framerate_den = 1;
        int ai_width = 320;
        int ai_height = 320;
        int stream_port = 5004;
        int stream_queue_max = 10;
        int ai_queue_max = 2;
        bool useDmabuf = false;
        bool useDirect = false;
    };

    PipelineManager();
    ~PipelineManager();

    // Initialize and build the GStreamer pipeline
    bool buildPipeline();

    // Start the pipeline state to PLAYING
    void start();

    // Stop the pipeline and release resources
    void stop();

    // OSD Drawing logic
    void draw_overlay(cairo_t *cr);

    // Thermal throttling hook
    void setAIThrottle(int sleep_ms, bool paused);

private:
    bool buildPipelineInternal(bool use_dmabuf, bool use_direct);
    void rebuildSoftwarePipeline();
    void rebuildDmabufDirectPipeline();
    void resetPipeline();
    bool applyPipeline(const std::string& pipeline_desc);

    GstElement *pipeline = nullptr;
    GstElement *app_sink = nullptr;
    GstBus *bus = nullptr;
    NCNNDetector detector;
    PipelineConfig config;

    bool use_dmabuf_config = false;
    bool dmabuf_active = false;
    bool dmabuf_disabled = false;
    bool dmabuf_direct_tried = false;
    bool caps_logged = false;
    int overlay_width = 640;
    int overlay_height = 480;
    std::atomic<long long> last_sample_us{0};

    // Static callback wrapper for GStreamer C API
    static GstFlowReturn on_new_sample_wrapper(GstElement *sink, gpointer user_data);

    // Bus message handler
    static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data);
    
    // Actual member function to handle the sample
    GstFlowReturn on_new_sample(GstElement *sink);
};
