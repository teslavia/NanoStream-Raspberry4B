# NanoStream-Raspberry4B 后续演进实施方案 (Implementation Plan)

基于当前架构图与 README 中列出的待优化点，制定以下分阶段实施方案。本方案旨在进一步压榨树莓派 4B 硬件性能，提升产品的交互体验与稳定性。

## 阶段一：可视化体验升级 (Visual Feedback)
**目标**: 在视频流中实时叠加 AI 检测框 (OSD)，实现“所见即所得”。

### 1. OSD 叠加方案
- **技术选型**: `cairooverlay` (GStreamer 插件)
- **架构调整**:
    - 在 **Branch A (推流分支)** 的 `videoconvert` 与 `v4l2h264enc` 之间插入 `cairooverlay` 元素。
    - **数据流向**: AI Worker 线程计算出坐标 -> 更新共享状态变量 (Mutex保护) -> `cairooverlay` 的 `draw` 回调函数读取坐标并绘制。
- **难点攻克**:
    - **同步问题**: AI 延迟约 100ms，直接绘制会导致框滞后于画面。
    - **解决方案**: 采用“最近邻插值”策略，总是绘制最新的检测结果。虽然有延迟，但对于监控场景可接受。

### 2. WebRTC 低延迟接入
- **技术选型**: `mediamtx` (外部轻量级流媒体服务器) 或 `webrtcbin` (嵌入式)。
- **实施路径**:
    - 推荐使用 **MediaMTX** 作为旁路服务。它能直接拉取我们现有的 RTSP 流并转为 WebRTC，无需修改 C++ 核心代码，解耦性最好。
    - 部署方式: 提供一个 `docker-compose.yml` 或安装脚本一键部署 MediaMTX。

---

## 阶段二：VPU 深度性能压榨 (Deep VPU Optimization)
**目标**: 彻底移除 CPU 格式转换，实现“零拷贝”编码。

### 1. DMABUF 零拷贝链路
- **现状**: `libcamerasrc` (DMABUF) -> `videoconvert` (CPU Copy) -> `v4l2h264enc`。
- **优化目标**: `libcamerasrc` (DMABUF) -> `v4l2convert` (Hardware) -> `v4l2h264enc`。
- **实施步骤**:
    - 深入调试 `v4l2convert` 的 `output-io-mode=dmabuf-import` 参数。
    - 尝试锁定摄像头输出格式为 `NV12`，并强制 `v4l2h264enc` 接受 `NV12` (需解决 stride 对齐问题)。
    - **预期收益**: CPU 占用率降低 15-20%。

### 2. 动态负载平衡 (Thermal Throttling)
- **机制**: 增加系统监控线程。
- **逻辑**:
    - 每 5 秒读取 `/sys/class/thermal/thermal_zone0/temp`。
    - 若温度 > 75°C，强制 AI 线程 `sleep(100ms)`，降低推理帧率。
    - 若温度 > 80°C，暂停 AI 线程，优先保推流。

---

## 阶段三：AI 能力扩展 (AI Expansion)
**目标**: 提升检测精度与多任务能力。

### 1. INT8 量化定制
- **工具**: `ncnn2table`
- **步骤**:
    - 收集树莓派摄像头实际场景的校准数据集 (Calibration Dataset)。
    - 在 RPi4 上重新生成量化表，而非使用通用的。
    - **预期收益**: 推理速度提升 10-20%，精度损失减小。

### 2. 多模型热切换
- **架构调整**: 将 `NCNNDetector` 抽象为基类，派生出 `NanoDet`, `YoloV8`, `Pose` 等子类。
- **功能**: 支持通过 HTTP API 或 MQTT 指令动态切换当前运行的模型。

---

## 实施路线图 (Roadmap)

| 阶段 | 任务 | 预计工时 | 优先级 |
| :--- | :--- | :--- | :--- |
| **P1** | 集成 `cairooverlay` 实现 OSD | 2 天 | High |
| **P1** | 编写 MediaMTX 部署脚本 (WebRTC) | 0.5 天 | Medium |
| **P2** | 攻克 `v4l2convert` 零拷贝问题 | 3-5 天 | High |
| **P2** | 实现温度监控与动态降频 | 1 天 | Medium |
| **P3** | 定制 INT8 量化表 | 2 天 | Low |

---

## 进展更新（dev 合并）
- P1 OSD/WebRTC 已落地：cairooverlay 叠加、MediaMTX 旁路模板、本地 WebRTC 播放页兼容 path 与多端点。
- 检测管线修复与稳定性提升：NanoDet-m 分布式回归解码正确化；阈值/去重/NMS 调优，减少抖动重复框；appsink/OSD 防护。
- 工程健壮性：禁用 packing layout、完善头部诊断与日志；gitignore 更新。

## P2 实施记录（进行中）
- 新增 DMABUF 零拷贝链路开关：通过 `NANOSTREAM_DMABUF=1` 启用。
- 推流分支采用 `v4l2convert output-io-mode=dmabuf-import` → `v4l2h264enc output-io-mode=dmabuf-import`，锁定 NV12。
- 视觉稳定性增强：IOU NMS + EMA 平滑，默认输出 top1。
- 温控降频开关：`NANOSTREAM_THERMAL=1` 启用，阈值可配置：
  - `NANOSTREAM_THERMAL_HIGH` (默认 75000)
  - `NANOSTREAM_THERMAL_CRIT` (默认 80000)
  - `NANOSTREAM_THERMAL_SLEEP` (默认 100)
- DMABUF 启动反馈：会打印 `DMABUF status: active/fallback` 便于确认是否回退。
- 若平台不支持 DMABUF，会生成 `~/.nanostream_dmabuf_disabled`，后续启动自动回退到软件管线。

## P2 性能对比测试（记录模板）
- 测试条件：分辨率 640x480 @15fps，环境温度、供电稳定。
- 对比项：DMABUF=1 vs DMABUF=0。
- 记录指标：
  - CPU 占用（top/htop）
  - AI 延迟（日志 Lat）
  - 推流稳定性（掉帧/卡顿主观）
  - 温度（/sys/class/thermal/thermal_zone0/temp）
  - 编码器报错（dmesg/日志）
**制定人**: mikylee
**日期**: 2026-01-27
