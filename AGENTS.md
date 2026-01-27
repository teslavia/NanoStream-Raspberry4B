# Developer Agent Guide: NanoStream

This guide provides essential information for AI agents and developers working on the NanoStream project, a high-performance asynchronous real-time detection system based on GStreamer and NCNN for Raspberry Pi 4B.

## 🛠 Build and Development

### Build Commands
The project uses CMake. A helper script is provided for standard builds.
- **Full Build**: `sh scripts/build.sh`
- **Manual Build**:
  ```bash
  mkdir -p build && cd build
  cmake ..
  make -j$(nproc)
  ```

### Run Commands
- **Main Application**: `./build/NanoStream`
- **RTSP Access**: Connect to `rtsp://<device-ip>:8554/live`

### Linting and Testing
Currently, the project does not have an integrated testing framework (like GTest) or automated linting.
- **Verification**: Manually verify pipeline state and AI output in the console.
- **Note**: When adding new features, prefer adding unit tests for pure logic components in a `tests/` directory.

---

## 🎨 Code Style Guidelines

### General Conventions
- **Language**: C++17
- **Naming**:
  - **Classes**: `PascalCase` (e.g., `PipelineManager`)
  - **Methods/Functions**: `camelCase` (e.g., `buildPipeline`)
  - **Variables**: `snake_case` or `camelCase` (consistent with local context).
  - **Constants/Macros**: `SCREAMING_SNAKE_CASE` (e.g., `MAX_BUFFERS`)
- **Headers**: Use `#pragma once` for header guards.

### Imports and Includes
- Header order: Standard libraries, GStreamer headers, NCNN headers, Local headers.
- Use absolute paths relative to `include/` for local headers.

### Formatting
- **Indentation**: 4 spaces.
- **Braces**: K&R style (braces on the same line as the statement).
- **Line Length**: Aim for < 100 characters.

### Types and Safety
- Prefer `std::vector` and `std::string` over raw arrays and `char*`.
- Use `std::unique_ptr` or `std::shared_ptr` for memory management where GStreamer doesn't already manage the lifecycle.
- **GStreamer Objects**: Always use `gst_object_unref()` or `gst_buffer_unref()` for ref-counted objects when ownership is transferred to you.

### Error Handling
- Use boolean return values for complex initialization (e.g., `bool loadModel(...)`).
- Log errors to `std::cerr` with a prefix: `[Fatal]`, `[Error]`, or `[Warning]`.
- For AI-specific logs, use `[AI]`.

---

## 🏗 Project Architecture & Patterns

### Asynchronous AI Path
The project implements a "leaky" AI path to prevent inference latency from blocking the stream:
1. **Branch A (Streaming)**: Direct hardware encoding (`v4l2h264enc`) -> RTSP.
2. **Branch B (AI)**: `appsink` with `drop=true` and `max-buffers=1`.
- **Worker Thread**: Inference runs in a dedicated thread (`NCNNDetector::workerLoop`) using a mutex/condition_variable pattern.

### GStreamer Pipeline Tips
- **Caps Filtering**: Always specify caps (resolution, format, framerate) between elements to ensure hardware acceleration works correctly.
- **Zero-Copy**: Use `gst_buffer_map` to access raw data from `appsink` samples.
- **Async Sinks**: Set `async=false` on sinks to prevent pipeline preroll deadlocks in multi-branch setups.

---

## ⚠️ Common Pitfalls (Troubleshooting)

1. **Memory Alignment**: If `libcamerasrc` fails to link to `v4l2h264enc`, insert a `videoconvert` element to force memory copy/realignment.
2. **NCNN Paths**: NCNN paths are currently hardcoded in `CMakeLists.txt` to `/usr/local/include/ncnn`. Ensure `scripts/install_ncnn.sh` has been run.
3. **Firewall**: Ensure port 8554 (RTSP) and 5004 (RTP/UDP) are open.
