#include "pipeline_manager.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <fstream>
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

gboolean PipelineManager::on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    auto* self = static_cast<PipelineManager*>(user_data);
    auto write_disable_flag = []() {
        const char* home = std::getenv("HOME");
        std::string disable_flag = home ? std::string(home) + "/.nanostream_dmabuf_disabled" : std::string(".nanostream_dmabuf_disabled");
        std::ofstream flag(disable_flag);
    };
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR && self->use_dmabuf_config && self->dmabuf_active) {
        const char* debug_env = std::getenv("NANOSTREAM_DEBUG");
        bool debug = (debug_env && std::string(debug_env) == "1");
        GError *err = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(message, &err, &dbg);
        std::string msg = err ? err->message : "";
        const char* src_name = GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : "unknown";
        if (err) g_error_free(err);
        if (dbg) g_free(dbg);

        if (debug) {
            std::cout << "[NanoStream] DMABUF error from " << src_name << ": " << msg << std::endl;
        }
        if (!self->dmabuf_direct_tried) {
            std::cout << "[NanoStream] DMABUF runtime failure detected, switching to direct DMABUF pipeline." << std::endl;
            self->dmabuf_direct_tried = true;
            self->rebuildDmabufDirectPipeline();
        } else {
            std::cout << "[NanoStream] DMABUF runtime failure detected, switching to software pipeline." << std::endl;
            self->dmabuf_active = false;
            self->dmabuf_disabled = true;
            std::cout << "[NanoStream] DMABUF disabled on this platform. Use NANOSTREAM_DMABUF=0." << std::endl;
            write_disable_flag();
            self->rebuildSoftwarePipeline();
        }
    }
    return TRUE;
}

void PipelineManager::setAIThrottle(int sleep_ms, bool paused) {
    detector.setThrottle(sleep_ms, paused);
}

static void on_draw_wrapper(GstElement *overlay, cairo_t *cr, guint64 timestamp, guint64 duration, gpointer user_data) {
    static_cast<PipelineManager*>(user_data)->draw_overlay(cr);
}

bool PipelineManager::buildPipeline() {
    const char* dmabuf_env = std::getenv("NANOSTREAM_DMABUF");
    bool use_dmabuf = (dmabuf_env && std::string(dmabuf_env) == "1");
    use_dmabuf_config = use_dmabuf;
    dmabuf_direct_tried = false;
    const char* home = std::getenv("HOME");
    std::string disable_flag = home ? std::string(home) + "/.nanostream_dmabuf_disabled" : std::string(".nanostream_dmabuf_disabled");
    if (use_dmabuf) {
        std::ifstream flag(disable_flag);
        if (flag.good()) {
            dmabuf_disabled = true;
        }
    }
    if (use_dmabuf && dmabuf_disabled) {
        std::cout << "[NanoStream] DMABUF disabled on this platform, using software pipeline." << std::endl;
        use_dmabuf = false;
    }
    return buildPipelineInternal(use_dmabuf, false);
}

bool PipelineManager::buildPipelineInternal(bool use_dmabuf, bool use_direct) {
    std::stringstream ss;

    // STABLE ENGINE: x264enc + OSD + 640x480
    // Force BGRx for Cairo OSD, then convert to I420 for encoder
    const std::string dmabuf_pipeline =
        "libcamerasrc ! video/x-raw,width=640,height=480,framerate=15/1,format=NV12 ! tee name=t "
        "t. ! queue max-size-buffers=10 leaky=downstream ! "
        "v4l2convert output-io-mode=dmabuf-import ! video/x-raw,format=NV12 ! "
        "v4l2h264enc output-io-mode=dmabuf-import ! h264parse config-interval=1 ! "
        "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=5004 sync=false async=false "
        "t. ! queue leaky=downstream max-size-buffers=2 ! videoscale ! videoconvert ! "
        "video/x-raw,format=RGB,width=320,height=320 ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    const std::string dmabuf_direct_pipeline =
        "libcamerasrc ! video/x-raw,width=640,height=480,framerate=15/1,format=NV12 ! tee name=t "
        "t. ! queue max-size-buffers=10 leaky=downstream ! "
        "v4l2h264enc output-io-mode=dmabuf-import ! h264parse config-interval=1 ! "
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

    if (use_dmabuf && use_direct) {
        ss << dmabuf_direct_pipeline;
    } else if (use_dmabuf) {
        ss << dmabuf_pipeline;
    } else {
        ss << software_pipeline;
    }

    if (use_dmabuf && use_direct) {
        std::cout << "[NanoStream] Launching DMABUF Direct Pipeline..." << std::endl;
    } else if (use_dmabuf) {
        std::cout << "[NanoStream] Launching DMABUF Zero-Copy Pipeline..." << std::endl;
    } else {
        std::cout << "[NanoStream] Launching Stabilized OSD Engine (Software)..." << std::endl;
    }

    GError *error = nullptr;
    bool dmabuf_fallback = false;
    pipeline = gst_parse_launch(ss.str().c_str(), &error);
    if (error) {
        std::cerr << "[Error] " << error->message << std::endl;
        g_error_free(error);
        if (use_dmabuf && !use_direct) {
            std::cout << "[NanoStream] DMABUF pipeline failed, trying direct DMABUF pipeline." << std::endl;
            return buildPipelineInternal(true, true);
        }
        if (use_dmabuf && use_direct) {
            std::cout << "[NanoStream] DMABUF pipeline failed, falling back to software pipeline." << std::endl;
            dmabuf_fallback = true;
            dmabuf_disabled = true;
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
        }
    }

    dmabuf_active = use_dmabuf && !dmabuf_fallback;
    if (use_dmabuf) {
        std::cout << "[NanoStream] DMABUF status: "
                  << (dmabuf_fallback ? "fallback" : (use_direct ? "active-direct" : "active"))
                  << " (set NANOSTREAM_DMABUF=0 to force software)" << std::endl;
    }

    if (use_dmabuf && use_direct) {
        auto write_disable_flag = []() {
            const char* home = std::getenv("HOME");
            std::string disable_flag = home ? std::string(home) + "/.nanostream_dmabuf_disabled" : std::string(".nanostream_dmabuf_disabled");
            std::ofstream flag(disable_flag);
        };
        last_sample_us.store(0);
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!use_dmabuf_config || !dmabuf_active) return;
            long long last = last_sample_us.load();
            if (last == 0) {
                std::cout << "[NanoStream] DMABUF direct pipeline produced no samples, falling back to software." << std::endl;
                dmabuf_active = false;
                dmabuf_disabled = true;
                const char* home = std::getenv("HOME");
                std::string disable_flag = home ? std::string(home) + "/.nanostream_dmabuf_disabled" : std::string(".nanostream_dmabuf_disabled");
                std::ofstream flag(disable_flag);
                rebuildSoftwarePipeline();
            }
        }).detach();
    }

    GstElement *osd = gst_bin_get_by_name(GST_BIN(pipeline), "osd");
    if (osd) {
        g_signal_connect(osd, "draw", G_CALLBACK(on_draw_wrapper), this);
        gst_object_unref(osd);
    }

    app_sink = gst_bin_get_by_name(GST_BIN(pipeline), "ncnn_sink");
    g_signal_connect(app_sink, "new-sample", G_CALLBACK(on_new_sample_wrapper), this);

    if (!bus) {
        bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(bus, on_bus_message, this);
    }

    detector.loadModel("models/nanodet_m.param", "models/nanodet_m.bin");
    return true;
}

void PipelineManager::rebuildSoftwarePipeline() {
    dmabuf_active = false;
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }

    buildPipelineInternal(false, false);
    start();
}

void PipelineManager::rebuildDmabufDirectPipeline() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }

    buildPipelineInternal(true, true);
    start();
}

void PipelineManager::start() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void PipelineManager::stop() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }
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
            last_sample_us.store(g_get_monotonic_time());
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
