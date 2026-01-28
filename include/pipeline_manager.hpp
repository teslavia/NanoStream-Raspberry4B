#pragma once

#include <gst/gst.h>
#include <cairo.h>
#include <string>
#include "ncnn_detector.hpp"

class PipelineManager {
public:
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
    GstElement *pipeline = nullptr;
    GstElement *app_sink = nullptr;
    NCNNDetector detector;

    // Static callback wrapper for GStreamer C API
    static GstFlowReturn on_new_sample_wrapper(GstElement *sink, gpointer user_data);
    
    // Actual member function to handle the sample
    GstFlowReturn on_new_sample(GstElement *sink);
};
