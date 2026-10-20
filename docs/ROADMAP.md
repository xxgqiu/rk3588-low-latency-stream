# 开发路线

## M1：采集基线

- [x] OV8858 MIPI/RKISP NV12 采集
- [x] USB UVC YUYV/MJPEG 格式协商
- [x] single/multi-planar V4L2 MMAP
- [x] sequence/timestamp/FPS 指标
- [x] 有界 drop-oldest 队列
- [x] 主机测试和板端实测

## M2：MPP 硬件编码

- [x] NV12 → RKMPP H.264
- [x] 正确处理 width/height 与 stride/alignment
- [x] 输出 Annex-B `.h264`
- [x] ffprobe 信息检查与 FFmpeg 全量解码验证
- [x] 编码耗时、码率、GOP 和关键帧统计
- [x] 视觉抽帧检查 stride 与 NV12 色彩正确性

## M3：三线程实时推流

- [x] Capture / Encode / Network 三线程
- [x] 有界 frame/packet queue
- [x] 包序号缺口检测与 IDR 重同步
- [x] FFmpeg RTSP 发布与 MediaMTX 服务端
- [x] PC 跨局域网 ffprobe/FFmpeg 播放
- [x] 正常停止与脚本进程清理
- [x] 队列、编码、网络、CPU 与 RSS 可观测字段
- [ ] 使用光学时钟测量摄像头到显示器的精确端到端延迟

## M4：性能优化

- [x] V4L2 `VIDIOC_EXPBUF` 与 DMA-BUF ownership audit
- [x] MPP / RGA 外部 fd 导入验证
- [x] 单帧 RGA `1224 → 1232` 布局整理和完整可见内容校验
- [x] 明确区分 DMA-BUF 容量、有效数据大小和 NV12 UV offset
- [x] 将 DMA-BUF + RGA 路径接入独立持续编码实验
- [x] CPU-copy baseline 与 DMA-BUF + RGA 候选对照
- [x] 旧 CPU-copy RTSP 短时回归
- [x] 把 DMA-BUF + RGA 接入有界 RTSP pipeline
- [x] DMA-BUF RTSP 的 drop-oldest 与 buffer ownership 压力验证
- [x] packet sequence gap 与 IDR 恢复离线回归
- [x] CPU、RSS、FPS、内部延迟和板端网络回归
- [x] PC 跨局域网 FFmpeg 解码
- [ ] 使用光学时钟测量摄像头到显示器的精确端到端延迟

当前审计、持续编码和 RTSP 实测支持 `V4L2 DMA-BUF → RGA → aligned MPP DMA-BUF`。该方案仍有一次硬件拷贝，不称为纯零拷贝。详见 [DMA-BUF 可行性审计](DMABUF_FEASIBILITY.md)、[持续编码性能对照](ENCODING_BENCHMARK.md)与 [RTSP 完整链路验证](RTSP_VALIDATION.md)。

## M5：电脑端客户端与端到端测量

- [x] Qt6 + FFmpeg RTSP 客户端
- [x] 后台解码线程与容量 1 latest-frame mailbox
- [x] FPS、码率、解码耗时、包/帧错误和覆盖帧统计
- [x] TCP/UDP transport 选择
- [x] 拍照法时钟界面与同屏截图
- [ ] 采集至少 30 组样本并报告中位数、P95 和测量不确定度

## M6：主/子码流与故障隔离

- [x] 同一 DMA-BUF 输入生成 1632×1224 主码流和 640×480 子码流
- [x] RGA 同尺寸布局整理与异尺寸缩放
- [x] 两套 MPP 编码器、packet queue 和 Network Thread
- [x] 主/子流 ffprobe 与 FFmpeg 实际解码
- [x] 慢子分支 80 ms 故障注入
- [x] 主分支吞吐、队列隔离和 camera lease 守恒验证
- [ ] 30 分钟以上双码流稳定性验证
