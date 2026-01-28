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
    bool buildPipelineInternal(bool use_dmabuf, bool use_direct);
    void rebuildSoftwarePipeline();
    void rebuildDmabufDirectPipeline();

    GstElement *pipeline = nullptr;
    GstElement *app_sink = nullptr;
    GstBus *bus = nullptr;
    NCNNDetector detector;

    bool use_dmabuf_config = false;
    bool dmabuf_active = false;
    bool dmabuf_disabled = false;
    bool dmabuf_direct_tried = false;

    // Static callback wrapper for GStreamer C API
    static GstFlowReturn on_new_sample_wrapper(GstElement *sink, gpointer user_data);

    // Bus message handler
    static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data);
    
    // Actual member function to handle the sample
    GstFlowReturn on_new_sample(GstElement *sink);
};
