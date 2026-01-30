#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <gst/gst.h>

#include "pipeline_manager.hpp"
#include "rtsp_service.hpp"
#include "runtime_config.hpp"

int main(int argc, char *argv[]) {
    // 1. Initialize GStreamer
    gst_init(&argc, &argv);
    std::cout << "[NanoStream] System starting..." << std::endl;

    // 2. Start RTSP Server (runs in background via GMainLoop context)
    // It listens on 127.0.0.1:5004 for UDP packets from our pipeline
    // Clients connect to rtsp://<PI_IP>:8554/live
    const RuntimeConfig& runtime = getRuntimeConfig();
    if (runtime.debug) {
        std::cout << "[NanoStream] Runtime config:\n" << formatRuntimeConfig(runtime) << std::endl;
    }
    RTSPServer rtspServer;
    std::string rtsp_host = resolveRtspHost(runtime);
    rtspServer.start(8554, "/live", 5004, rtsp_host);
    
    // 3. Initialize and Start Pipeline
    PipelineManager pipeline;
    if (!pipeline.buildPipeline()) {
        std::cerr << "[Fatal] Pipeline build failed. Exiting." << std::endl;
        return -1;
    }

    pipeline.start();
    std::cout << "[NanoStream] Pipeline is RUNNING." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    if (runtime.thermalEnabled) {
        std::thread([&pipeline]() {
            const RuntimeConfig& cfg = getRuntimeConfig();

            int last_mode = -1;
            while (true) {
                std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
                int temp_milli = 0;
                if (temp_file.good()) {
                    temp_file >> temp_milli;
                }

                int sleep_ms = 0;
                bool paused = false;
                if (temp_milli >= cfg.thermalCrit) {
                    paused = true;
                } else if (temp_milli >= cfg.thermalHigh) {
                    sleep_ms = cfg.thermalSleepMs;
                }

                int mode = paused ? 2 : (sleep_ms > 0 ? 1 : 0);
                if (cfg.debug && mode != last_mode) {
                    std::cout << "[Thermal] temp=" << (temp_milli / 1000.0f)
                              << "C, mode=" << mode << std::endl;
                    last_mode = mode;
                }

                pipeline.setAIThrottle(sleep_ms, paused);
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }).detach();
    }
    std::cout << ">> RTSP URL: rtsp://" << rtsp_host << ":8554/live" << std::endl;
    std::cout << ">> IMPORTANT: Ensure Pi's firewall is disabled (sudo ufw disable)" << std::endl;
    std::cout << ">> AI Inference: Running asynchronously on NCNN" << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    // 4. Main Event Loop
    // GStreamer relies on a GMainLoop to handle bus messages and RTSP server events
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    // Cleanup (This part is rarely reached in embedded loops unless signal handling is added)
    pipeline.stop();
    g_main_loop_unref(loop);

    return 0;
}
