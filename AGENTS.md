# Developer Agent Guide: NanoStream

This guide is for agentic coding assistants working in this repository.
It summarizes how to build/run, where to look, and the local style rules.

## Sources of Truth
- README: project overview and build/run scripts.
- scripts/build.sh: standard build entry point.
- CMakeLists.txt: build flags and dependencies.
- No Cursor or Copilot rules found in this repo.

## Build, Lint, Test

### Build (preferred)
- Full build: `sh scripts/build.sh`
- Output binary: `./build/NanoStream`

### Build (manual)
```
mkdir -p build
cd build
cmake ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

### Run
- Main app: `./build/NanoStream`
- RTSP stream: `rtsp://<device-ip>:8554/live`

### Dependencies (RPi target)
- System deps are documented in `README.md`.
- NCNN is built via `scripts/install_ncnn.sh`.
- Models are downloaded via `scripts/download_models.sh`.

### Linting
- No automated linting configured.
- Keep formatting consistent; do not reformat unrelated code.

### Tests
- No automated tests configured (no CTest or GTest in tree).
- If you add tests, wire them into CMake + CTest.
- Single test (when CTest is added): `ctest -R <test_name> --output-on-failure`

## Project Structure
- `src/`: C++ sources (`main.cpp`, `pipeline_manager.cpp`, `ncnn_detector.cpp`, `rtsp_service.cpp`).
- `include/`: public headers; use `#pragma once`.
- `scripts/`: build + setup scripts.
- `docs/` and `deploy/`: project docs and deployment artifacts.

## Code Style Guidelines

### Language and Standards
- C++17 (`CMakeLists.txt` sets `CMAKE_CXX_STANDARD 17`).
- Prefer standard library types and RAII.

### Naming
- Classes/structs: `PascalCase`.
- Methods/functions: `camelCase`.
- Variables: `camelCase` or `snake_case` (stay consistent within a file).
- Constants/macros: `SCREAMING_SNAKE_CASE`.

### Includes
- Order: standard library, GStreamer headers, Cairo/NCNN, local headers.
- Local headers use project-relative includes (from `include/`).

### Formatting
- Indentation: 4 spaces.
- Braces: K&R style.
- Line length: aim for < 100 chars.

### Types and Ownership
- Prefer `std::vector`/`std::string` over raw arrays and `char*`.
- Use `std::unique_ptr`/`std::shared_ptr` when owning heap memory.
- GStreamer objects are ref-counted: unref explicitly when ownership is yours.
- Avoid hidden ownership transfers; document when ownership changes.

### Error Handling and Logging
- Use `bool` return values for init steps (e.g., `buildPipeline`).
- Log errors to `std::cerr` with prefixes: `[Fatal]`, `[Error]`, `[Warning]`.
- AI-specific logs use `[AI]`.
- Avoid silent failures and empty `catch` blocks.

## Build System Notes
- NCNN is hardcoded at `/usr/local/include/ncnn` and `/usr/local/lib/libncnn.a`.
- OpenMP is required; CMake will fail if missing.
- On non-ARM hosts, dependencies must be installed locally.

## Runtime Behavior and Patterns

### Pipeline Architecture
- Two-branch GStreamer pipeline: streaming branch and AI branch.
- AI branch uses `appsink` with `drop=true` and `max-buffers=1`.
- Set `async=false` on sinks to avoid multi-branch preroll deadlocks.
- Always use explicit caps filters (format/resolution/fps) between elements.

### Buffer Handling
- Map buffers with `gst_buffer_map` to access raw data.
- Unmap and unref buffers as soon as possible.
- If DMABUF alignment fails, insert `videoconvert` to force system memory.

### RTSP and H.264
- Use `h264parse` with `config-interval=1` and `stream-format=byte-stream`.
- RTSP server bridges UDP on port 5004 and exposes port 8554.

### Threading
- Inference runs on a dedicated worker thread.
- Avoid blocking the main GStreamer thread.

### Common Pitfalls
- Pipeline preroll deadlock: ensure sinks set `async=false`.
- RTSP disconnects: confirm `h264parse` uses Annex-B byte-stream.
- DMABUF alignment failures: add `videoconvert` to force system memory.
- Host builds: install GStreamer, Cairo, and NCNN locally or CMake fails.

### Logging Conventions
- Startup logs use `[NanoStream]` and should be concise.
- Fatal exit paths should print `[Fatal]` before returning non-zero.
- Prefer `std::cout` for normal status, `std::cerr` for errors.

### Safe Refactor Boundaries
- Do not change pipeline element order unless required.
- Avoid touching RTSP server wiring when modifying AI logic.
- Keep AI thread behavior non-blocking and bounded.

## Environment Variables
- `NANOSTREAM_THERMAL=1` enables thermal throttling logic.
- `NANOSTREAM_DEBUG=1` enables debug thermal logs.
- `NANOSTREAM_THERMAL_HIGH`, `NANOSTREAM_THERMAL_CRIT`,
  `NANOSTREAM_THERMAL_SLEEP` tune throttling thresholds.
- Other runtime toggles may exist; check `src/` before adding new ones.

## Do and Do Not
- Do keep changes minimal and local to the feature or fix.
- Do not refactor while fixing a bug unless required.
- Do not add new dependencies without a strong reason.
- Do not change build scripts unless necessary for your task.

## When Adding Tests (Future)
- Add tests under `tests/` and register with CTest in `CMakeLists.txt`.
- Ensure tests are fast and deterministic.
- Provide a single-test command in this file after wiring CTest.
