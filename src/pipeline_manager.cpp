#include "pipeline_manager.hpp"
#include <iostream>
#include <sstream>
#include <gst/app/gstappsink.h>

PipelineManager::PipelineManager() {}

PipelineManager::~PipelineManager() {
    stop();
}

bool PipelineManager::buildPipeline() {
    std::stringstream ss;

    // VPU Hardware Pipeline: libcamerasrc -> OSD -> v4l2h264enc
    ss << "libcamerasrc ! video/x-raw,width=640,height=480,framerate=15/1 ! tee name=t "
       << "t. ! queue max-size-buffers=20 leaky=downstream ! "
       << "videoconvert ! cairooverlay name=osd ! video/x-raw,format=I420 ! "
       << "v4l2h264enc extra-controls=\"controls,video_bitrate=3000000\" ! h264parse config-interval=1 ! "
       << "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=5004 sync=false async=false "
       << "t. ! queue leaky=downstream max-size-buffers=2 ! videoscale ! videoconvert ! "
       << "video/x-raw,format=RGB,width=320,height=320 ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    std::cout << "[NanoStream] Launching VPU Hardware Pipeline..." << std::endl;

    GError *error = nullptr;
    pipeline = gst_parse_launch(ss.str().c_str(), &error);
    if (error) {
        std::cerr << "[Error] Build failed: " << error->message << std::endl;
        g_error_free(error);
        return false;
    }

    app_sink = gst_bin_get_by_name(GST_BIN(pipeline), "ncnn_sink");
    g_signal_connect(app_sink, "new-sample", G_CALLBACK(on_new_sample_wrapper), this);

    detector.loadModel("models/nanodet_m.param", "models/nanodet_m.bin");
    return true;
}

void PipelineManager::start() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void PipelineManager::stop() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }
}

GstFlowReturn PipelineManager::on_new_sample_wrapper(GstElement *sink, gpointer user_data) {
    return static_cast<PipelineManager*>(user_data)->on_new_sample(sink);
}

GstFlowReturn PipelineManager::on_new_sample(GstElement *sink) {
    GstSample *sample;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            detector.pushFrame(map.data, 320, 320);
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    return GST_FLOW_ERROR;
}
