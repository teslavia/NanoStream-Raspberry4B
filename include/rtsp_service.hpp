#pragma once

#include <string>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

class RTSPServer {
public:
    RTSPServer();
    ~RTSPServer();

    // Start RTSP Server
    // rtsp_port: Port for RTSP (e.g., 8554)
    // mount_point: URL suffix (e.g., "/live")
    // udp_port: Source UDP port to forward (e.g., 5004)
    void start(int rtsp_port, const std::string &mount_point, int udp_port);

private:
    GstRTSPServer *server = nullptr;
    guint source_id = 0; // Source ID for the main loop attachment
};
