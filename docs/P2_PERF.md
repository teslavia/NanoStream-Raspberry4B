# P2 性能对比记录

本记录用于比较 DMABUF 与软件管线的性能差异，便于判断是否值得继续推进零拷贝链路。

## 测试步骤
1. 软件管线：
   - `NANOSTREAM_DMABUF=0 ./build/NanoStream`
2. DMABUF 方案：
   - `NANOSTREAM_DMABUF=1 ./build/NanoStream`
   - 若提示 `~/.nanostream_dmabuf_disabled`，先删除后再试：
     `rm -f ~/.nanostream_dmabuf_disabled`
3. 每种方案连续运行 10 分钟，记录数据。

## 记录表

### 环境信息
- 设备型号：Raspberry Pi 4B
- 内核版本：
- libcamera 版本：v0.5.2+99-bfd68f78
- 摄像头型号：UVC 摄像头（2bdf:0289）
- 电源与散热情况：

### 指标对比
| 指标 | 软件管线 | DMABUF | 备注 |
| --- | --- | --- | --- |
| CPU 占用（平均） |  |  | top/htop 采样 |
| AI 延迟（平均） | ~150ms | N/A（回退） | 日志 Lat |
| 推流稳定性 | 稳定 | DMABUF 不支持，自动回退 | 是否掉帧/花屏 |
| 温度（最高） |  |  | thermal_zone0 |
| 编码器/驱动错误 |  | v4l2convert cannot import buffers | dmesg/GST_DEBUG |

### 结论
- 是否继续推进 DMABUF：当前平台不支持 dmabuf-import，暂不推进
- 主要瓶颈：v4l2convert dmabuf-import 失败，direct DMABUF 无样本
- 后续建议：升级内核/驱动或更换相机栈后再评估
