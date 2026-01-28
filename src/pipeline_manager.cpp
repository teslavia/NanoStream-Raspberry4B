#include "pipeline_manager.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstdlib>
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
        int x = det.x;
        int y = det.y;
        int w = det.w;
        int h = det.h;
        if (w <= 0 || h <= 0) continue;

        x = std::max(0, std::min(x, 639));
        y = std::max(0, std::min(y, 479));
        w = std::min(w, 640 - x);
        h = std::min(h, 480 - y);
        if (w <= 0 || h <= 0) continue;

        // 画绿色边框
        cairo_set_source_rgb(cr, 0.0, 1.0, 0.0); 
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);
        
        // 写标签背景
        int label_x = x;
        int label_y = std::max(0, y - 25);
        int label_w = std::min(100, 640 - label_x);
        if (label_w <= 0) continue;
        cairo_rectangle(cr, label_x, label_y, label_w, 25);
        cairo_fill(cr);

        // 写白色文字
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_move_to(cr, label_x + 5, label_y + 20);
        cairo_show_text(cr, det.label.c_str());
    }
}

void PipelineManager::setAIThrottle(int sleep_ms, bool paused) {
    detector.setThrottle(sleep_ms, paused);
}

static void on_draw_wrapper(GstElement *overlay, cairo_t *cr, guint64 timestamp, guint64 duration, gpointer user_data) {
    static_cast<PipelineManager*>(user_data)->draw_overlay(cr);
}

bool PipelineManager::buildPipeline() {
    std::stringstream ss;

    const char* dmabuf_env = std::getenv("NANOSTREAM_DMABUF");
    bool use_dmabuf = (dmabuf_env && std::string(dmabuf_env) == "1");

    // STABLE ENGINE: x264enc + OSD + 640x480
    // Force BGRx for Cairo OSD, then convert to I420 for encoder
    const std::string dmabuf_pipeline =
        "libcamerasrc ! video/x-raw,width=640,height=480,framerate=15/1,format=NV12 ! tee name=t "
        "t. ! queue max-size-buffers=10 leaky=downstream ! "
        "v4l2convert output-io-mode=dmabuf-import ! video/x-raw,format=NV12 ! "
        "v4l2h264enc output-io-mode=dmabuf-import bitrate=1000 ! h264parse config-interval=1 ! "
        "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=5004 sync=false async=false "
        "t. ! queue leaky=downstream max-size-buffers=2 ! videoscale ! videoconvert ! "
        "video/x-raw,format=RGB,width=320,height=320 ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    const std::string software_pipeline =
        "libcamerasrc ! video/x-raw,width=640,height=480,framerate=15/1 ! tee name=t "
        "t. ! queue max-size-buffers=10 leaky=downstream ! "
        "videoconvert ! video/x-raw,format=BGRx ! cairooverlay name=osd ! videoconvert ! video/x-raw,format=I420 ! "
        "x264enc speed-preset=ultrafast tune=zerolatency bitrate=1000 threads=4 ! h264parse config-interval=1 ! "
        "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=5004 sync=false async=false "
        "t. ! queue leaky=downstream max-size-buffers=2 ! videoscale ! videoconvert ! "
        "video/x-raw,format=RGB,width=320,height=320 ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    ss << (use_dmabuf ? dmabuf_pipeline : software_pipeline);

    if (use_dmabuf) {
        std::cout << "[NanoStream] Launching DMABUF Zero-Copy Pipeline..." << std::endl;
    } else {
        std::cout << "[NanoStream] Launching Stabilized OSD Engine (Software)..." << std::endl;
    }

    GError *error = nullptr;
    pipeline = gst_parse_launch(ss.str().c_str(), &error);
    if (error) {
        std::cerr << "[Error] " << error->message << std::endl;
        g_error_free(error);
        if (use_dmabuf) {
            std::cout << "[NanoStream] DMABUF pipeline failed, falling back to software pipeline." << std::endl;
            ss.str("");
            ss.clear();
            ss << software_pipeline;
            error = nullptr;
            pipeline = gst_parse_launch(ss.str().c_str(), &error);
            if (error) {
                std::cerr << "[Error] " << error->message << std::endl;
                g_error_free(error);
                return false;
            }
        } else {
            return false;
        }
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
    static bool caps_logged = false;
    GstSample *sample;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample) {
        if (!caps_logged) {
            GstCaps *caps = gst_sample_get_caps(sample);
            if (caps) {
                gchar *caps_str = gst_caps_to_string(caps);
                std::cout << "[Debug] appsink caps: " << caps_str << std::endl;
                g_free(caps_str);
                caps_logged = true;
            }
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const size_t expected_size = static_cast<size_t>(320) * 320 * 3;
            if (map.size < expected_size) {
                std::cerr << "[Warning] appsink buffer too small: " << map.size
                          << " bytes, expected at least " << expected_size << " bytes" << std::endl;
                gst_buffer_unmap(buffer, &map);
                gst_sample_unref(sample);
                return GST_FLOW_OK;
            }

            detector.pushFrame(map.data, 320, 320);
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    return GST_FLOW_ERROR;
}
