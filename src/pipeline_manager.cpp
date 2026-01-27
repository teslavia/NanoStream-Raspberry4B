#include "pipeline_manager.hpp"
#include <iostream>
#include <sstream>
#include <gst/app/gstappsink.h>
#include <cairo.h>
#include <cairo-gobject.h>

PipelineManager::PipelineManager() {}

PipelineManager::~PipelineManager() {
    stop();
}

void PipelineManager::draw_overlay(cairo_t *cr) {
    std::vector<Detection> dets = detector.getDetections();
    
    // 设置字体和基础绘图属性
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 20.0);
    cairo_set_line_width(cr, 3.0);

    for (const auto& det : dets) {
        // 画绿色边框
        cairo_set_source_rgb(cr, 0.0, 1.0, 0.0); 
        cairo_rectangle(cr, det.x, det.y, det.w, det.h);
        cairo_stroke(cr);
        
        // 写标签背景
        cairo_rectangle(cr, det.x, det.y - 25, 100, 25);
        cairo_fill(cr);

        // 写白色文字
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_move_to(cr, det.x + 5, det.y - 5);
        cairo_show_text(cr, det.label.c_str());
    }
}

static void on_draw_wrapper(GstElement *overlay, cairo_t *cr, guint64 timestamp, guint64 duration, gpointer user_data) {
    static_cast<PipelineManager*>(user_data)->draw_overlay(cr);
}

bool PipelineManager::buildPipeline() {
    std::stringstream ss;

    // STABLE ENGINE: x264enc + OSD + 640x480
    // I420 is chosen as the common ground for Cairo and x264enc
    ss << "libcamerasrc ! video/x-raw,width=640,height=480,framerate=15/1 ! tee name=t "
       << "t. ! queue max-size-buffers=10 leaky=downstream ! "
       << "videoconvert ! video/x-raw,format=I420 ! cairooverlay name=osd ! "
       << "x264enc speed-preset=ultrafast tune=zerolatency bitrate=1000 threads=4 ! h264parse config-interval=1 ! "
       << "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=5004 sync=false async=false "
       << "t. ! queue leaky=downstream max-size-buffers=2 ! videoscale ! videoconvert ! "
       << "video/x-raw,format=RGB,width=320,height=320 ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    std::cout << "[NanoStream] Launching Stabilized OSD Engine (Software)..." << std::endl;

    GError *error = nullptr;
    pipeline = gst_parse_launch(ss.str().c_str(), &error);
    if (error) {
        std::cerr << "[Error] " << error->message << std::endl;
        g_error_free(error);
        return false;
    }

    GstElement *osd = gst_bin_get_by_name(GST_BIN(pipeline), "osd");
    if (osd) {
        g_signal_connect(osd, "draw", G_CALLBACK(on_draw_wrapper), this);
        gst_object_unref(osd);
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
