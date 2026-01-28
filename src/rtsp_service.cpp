#include "rtsp_service.hpp"
#include <iostream>

#include "runtime_config.hpp"

RTSPServer::RTSPServer() {}

RTSPServer::~RTSPServer() {
    if (server) g_object_unref(server);
}

void RTSPServer::start(int rtsp_port, const std::string &mount_point, int udp_port, const std::string &host_label) {
    server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(server, std::to_string(rtsp_port).c_str());

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    // RECEIVE RAW H264:
    // This is the most robust way to bridge between the app and the server.
    // Explicitly set byte-stream format to match the sender.
    std::string launch_str = 
        "( udpsrc port=" + std::to_string(udp_port) + " address=127.0.0.1 caps=\"video/x-h264,stream-format=byte-stream,alignment=au\" ! "
        "h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )";

    gst_rtsp_media_factory_set_launch(factory, launch_str.c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE);

    gst_rtsp_mount_points_add_factory(mounts, mount_point.c_str(), factory);
    g_object_unref(mounts);

    source_id = gst_rtsp_server_attach(server, NULL);
    std::string host = host_label.empty() ? resolveRtspHost(getRuntimeConfig()) : host_label;
    std::cout << "[RTSP] Gateway active at rtsp://" << host << ":" << rtsp_port << mount_point << std::endl;
    std::cout << "[RTSP] Ensure you are using VLC with 'RTP over RTSP (TCP)' enabled if UDP fails." << std::endl;
}
