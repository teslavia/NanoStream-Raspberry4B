#include <iostream>
#include <gst/gst.h>
#include <thread>
#include <chrono>

#include "pipeline_manager.hpp"
#include "rtsp_service.hpp"

int main(int argc, char *argv[]) {
    // 1. Initialize GStreamer
    gst_init(&argc, &argv);
    std::cout << "[NanoStream] System starting..." << std::endl;

    // 2. Start RTSP Server (runs in background via GMainLoop context)
    // It listens on 127.0.0.1:5004 for UDP packets from our pipeline
    // Clients connect to rtsp://<PI_IP>:8554/live
    RTSPServer rtspServer;
    rtspServer.start(8554, "/live", 5004);
    
    // 3. Initialize and Start Pipeline
    PipelineManager pipeline;
    if (!pipeline.buildPipeline()) {
        std::cerr << "[Fatal] Pipeline build failed. Exiting." << std::endl;
        return -1;
    }

    pipeline.start();
    std::cout << "[NanoStream] Pipeline is RUNNING." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;
    std::cout << ">> RTSP URL: rtsp://192.168.1.48:8554/live" << std::endl;
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
