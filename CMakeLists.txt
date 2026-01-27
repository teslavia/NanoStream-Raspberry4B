cmake_minimum_required(VERSION 3.10)
project(NanoStream)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Platform Optimization
# -----------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    message(STATUS "AArch64 Detected: NEON is implicit. Enabling native tuning.")
    add_compile_options(-march=native)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    message(STATUS "ARMv7 Detected: Enabling NEON explicitly.")
    add_compile_options(-march=native -mfpu=neon)
endif()

# -----------------------------------------------------------------------------
# Dependencies: GStreamer, Cairo
# -----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0)
pkg_check_modules(CAIRO REQUIRED cairo)

# -----------------------------------------------------------------------------
# Dependencies: NCNN (Hardcoded Paths)
# -----------------------------------------------------------------------------
message(STATUS "Configuring NCNN with hardcoded paths...")
set(NCNN_INCLUDE_DIR "/usr/local/include/ncnn")
set(NCNN_LIBRARY "/usr/local/lib/libncnn.a")

if(NOT EXISTS "${NCNN_INCLUDE_DIR}/net.h")
    message(FATAL_ERROR "Critical Error: NCNN header net.h not found")
endif()

find_package(OpenMP REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

# -----------------------------------------------------------------------------
# Build Configuration
# -----------------------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GST_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${NCNN_INCLUDE_DIR}
)

set(SOURCES
    src/main.cpp
    src/pipeline_manager.cpp
    src/ncnn_detector.cpp
    src/rtsp_service.cpp
)

add_executable(NanoStream ${SOURCES})

target_link_libraries(NanoStream
    ${GST_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${NCNN_LIBRARY}
    OpenMP::OpenMP_CXX
    pthread
)

message(STATUS "Build Config Summary:")
message(STATUS "  - GST Libraries: ${GST_LIBRARIES}")
message(STATUS "  - Cairo Includes: ${CAIRO_INCLUDE_DIRS}")
message(STATUS "  - NCNN Lib: ${NCNN_LIBRARY}")
