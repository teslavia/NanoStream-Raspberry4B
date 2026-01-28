#include <iostream>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <cairo.h>
#include <cairo-gobject.h>

#include "pipeline_manager.hpp"
#include "runtime_config.hpp"

namespace {

std::string getDmabufDisableFlagPath() {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.nanostream_dmabuf_disabled";
    }
    return ".nanostream_dmabuf_disabled";
}

void writeDmabufDisableFlag() {
    std::ofstream flag(getDmabufDisableFlagPath());
}

std::string buildPipelineString(const PipelineManager::PipelineConfig& config) {
    const std::string base_caps =
        "video/x-raw,width=" + std::to_string(config.width) +
        ",height=" + std::to_string(config.height) +
        ",framerate=" + std::to_string(config.framerate_num) +
        "/" + std::to_string(config.framerate_den);
    const std::string ai_caps =
        "video/x-raw,format=RGB,width=" + std::to_string(config.ai_width) +
        ",height=" + std::to_string(config.ai_height);

    const std::string dmabuf_pipeline =
        "libcamerasrc ! " + base_caps + ",format=NV12 ! tee name=t "
        "t. ! queue max-size-buffers=" + std::to_string(config.stream_queue_max) + " leaky=downstream ! "
        "v4l2convert output-io-mode=dmabuf-import ! video/x-raw,format=NV12 ! "
        "v4l2h264enc output-io-mode=dmabuf-import ! h264parse config-interval=1 ! "
        "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=" + std::to_string(config.stream_port) + " sync=false async=false "
        "t. ! queue leaky=downstream max-size-buffers=" + std::to_string(config.ai_queue_max) + " ! videoscale ! videoconvert ! "
        + ai_caps + " ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    const std::string dmabuf_direct_pipeline =
        "libcamerasrc ! " + base_caps + ",format=NV12 ! tee name=t "
        "t. ! queue max-size-buffers=" + std::to_string(config.stream_queue_max) + " leaky=downstream ! "
        "v4l2h264enc output-io-mode=dmabuf-import ! h264parse config-interval=1 ! "
        "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=" + std::to_string(config.stream_port) + " sync=false async=false "
        "t. ! queue leaky=downstream max-size-buffers=" + std::to_string(config.ai_queue_max) + " ! videoscale ! videoconvert ! "
        + ai_caps + " ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    const std::string software_pipeline =
        "libcamerasrc ! " + base_caps + " ! tee name=t "
        "t. ! queue max-size-buffers=" + std::to_string(config.stream_queue_max) + " leaky=downstream ! "
        "videoconvert ! video/x-raw,format=BGRx ! cairooverlay name=osd ! videoconvert ! video/x-raw,format=I420 ! "
        "x264enc speed-preset=ultrafast tune=zerolatency bitrate=1000 threads=4 ! h264parse config-interval=1 ! "
        "video/x-h264,stream-format=byte-stream ! udpsink host=127.0.0.1 port=" + std::to_string(config.stream_port) + " sync=false async=false "
        "t. ! queue leaky=downstream max-size-buffers=" + std::to_string(config.ai_queue_max) + " ! videoscale ! videoconvert ! "
        + ai_caps + " ! appsink name=ncnn_sink sync=false async=false emit-signals=true";

    if (config.useDmabuf && config.useDirect) {
        return dmabuf_direct_pipeline;
    }
    if (config.useDmabuf) {
        return dmabuf_pipeline;
    }
    return software_pipeline;
}

}

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

        int max_w = overlay_width > 0 ? overlay_width : 640;
        int max_h = overlay_height > 0 ? overlay_height : 480;
        x = std::max(0, std::min(x, max_w - 1));
        y = std::max(0, std::min(y, max_h - 1));
        w = std::min(w, max_w - x);
        h = std::min(h, max_h - y);
        if (w <= 0 || h <= 0) continue;

        // 画绿色边框
        cairo_set_source_rgb(cr, 0.0, 1.0, 0.0); 
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);
        
        // 写标签背景
        int label_x = x;
        int label_y = std::max(0, y - 25);
        int label_w = std::min(100, max_w - label_x);
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
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR && self->use_dmabuf_config && self->dmabuf_active) {
        const RuntimeConfig& runtime = getRuntimeConfig();
        bool debug = runtime.debug;
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
            writeDmabufDisableFlag();
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

bool PipelineManager::applyPipeline(const std::string& pipeline_desc) {
    GError *error = nullptr;
    pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
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

    if (!bus) {
        bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(bus, on_bus_message, this);
    }

    return true;
}

bool PipelineManager::buildPipeline() {
    const RuntimeConfig& runtime = getRuntimeConfig();
    bool use_dmabuf = runtime.useDmabuf;
    use_dmabuf_config = use_dmabuf;
    dmabuf_direct_tried = false;
    config.useDmabuf = use_dmabuf;
    config.useDirect = false;
    if (use_dmabuf) {
        std::ifstream flag(getDmabufDisableFlagPath());
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
    const RuntimeConfig& runtime = getRuntimeConfig();
    PipelineConfig local_config = config;
    local_config.useDmabuf = use_dmabuf;
    local_config.useDirect = use_direct;
    const std::string pipeline_desc = buildPipelineString(local_config);

    if (use_dmabuf && use_direct) {
        std::cout << "[NanoStream] Launching DMABUF Direct Pipeline..." << std::endl;
    } else if (use_dmabuf) {
        std::cout << "[NanoStream] Launching DMABUF Zero-Copy Pipeline..." << std::endl;
    } else {
        std::cout << "[NanoStream] Launching Stabilized OSD Engine (Software)..." << std::endl;
    }

    GError *error = nullptr;
    bool dmabuf_fallback = false;
    if (!applyPipeline(pipeline_desc)) {
        if (use_dmabuf && !use_direct) {
            std::cout << "[NanoStream] DMABUF pipeline failed, trying direct DMABUF pipeline." << std::endl;
            return buildPipelineInternal(true, true);
        }
        if (use_dmabuf && use_direct) {
            std::cout << "[NanoStream] DMABUF pipeline failed, falling back to software pipeline." << std::endl;
            dmabuf_fallback = true;
            dmabuf_disabled = true;
            PipelineConfig software_config = local_config;
            software_config.useDmabuf = false;
            software_config.useDirect = false;
            const std::string software_desc = buildPipelineString(software_config);
            if (!applyPipeline(software_desc)) {
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
        last_sample_us.store(0);
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!use_dmabuf_config || !dmabuf_active) return;
            long long last = last_sample_us.load();
            if (last == 0) {
                std::cout << "[NanoStream] DMABUF direct pipeline produced no samples, falling back to software." << std::endl;
                dmabuf_active = false;
                dmabuf_disabled = true;
                writeDmabufDisableFlag();
                rebuildSoftwarePipeline();
            }
        }).detach();
    }

    bool use_int8 = runtime.useInt8;
    std::string int8_param = runtime.int8Param;
    std::string int8_bin = runtime.int8Bin;
    if (use_int8) {
        std::ifstream p(int8_param);
        std::ifstream b(int8_bin);
        if (!p.good() || !b.good()) {
            std::cout << "[NanoStream] INT8 model files missing, falling back to FP32." << std::endl;
            detector.loadModel("models/nanodet_m.param", "models/nanodet_m.bin");
        } else if (!detector.loadModel(int8_param, int8_bin)) {
            std::cout << "[NanoStream] INT8 model load failed, falling back to FP32." << std::endl;
            detector.loadModel("models/nanodet_m.param", "models/nanodet_m.bin");
        } else {
            std::cout << "[NanoStream] INT8 model active." << std::endl;
        }
    } else {
        detector.loadModel("models/nanodet_m.param", "models/nanodet_m.bin");
    }
    return true;
}

void PipelineManager::rebuildSoftwarePipeline() {
    dmabuf_active = false;
    resetPipeline();
    buildPipelineInternal(false, false);
    start();
}

void PipelineManager::rebuildDmabufDirectPipeline() {
    resetPipeline();
    buildPipelineInternal(true, true);
    start();
}

void PipelineManager::start() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void PipelineManager::stop() {
    resetPipeline();
}

void PipelineManager::resetPipeline() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) {
            gst_object_unref(bus);
            bus = nullptr;
        }
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }
    caps_logged = false;
}

GstFlowReturn PipelineManager::on_new_sample_wrapper(GstElement *sink, gpointer user_data) {
    return static_cast<PipelineManager*>(user_data)->on_new_sample(sink);
}

GstFlowReturn PipelineManager::on_new_sample(GstElement *sink) {
    GstSample *sample;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample) {
        if (!caps_logged) {
            GstCaps *caps = gst_sample_get_caps(sample);
            if (caps) {
                const GstStructure* s = gst_caps_get_structure(caps, 0);
                int w = 0;
                int h = 0;
                if (gst_structure_get_int(s, "width", &w)) {
                    overlay_width = w;
                }
                if (gst_structure_get_int(s, "height", &h)) {
                    overlay_height = h;
                }
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
