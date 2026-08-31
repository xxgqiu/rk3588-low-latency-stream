# DMA-BUF + RGA 可行性审计

## 审计目的

本审计只验证硬件缓冲能否跨 RKISP、RGA 和 MPP 共享，以及 OV8858 当前 NV12 布局能否直接满足现有 MPP 编码器的输入契约。它不替换 RTSP 主路径，也不把“fd 导入成功”误写成“零拷贝已经完成”。

诊断链路只处理一帧，不保存摄像头画面：

```text
RKISP MMAP buffer
  ├─ VIDIOC_EXPBUF → DMA-BUF fd → MPP import audit
  └─ VIDIOC_EXPBUF → DMA-BUF fd → RGA layout copy
                                      ↓
                              MPP 分配的 DMA-BUF
```

## 板端实测结果

运行环境为 RK3588 开发板、OV8858 MIPI 摄像头和 `/dev/video-camera0`。

| 检查项 | 实测结果 |
|---|---:|
| V4L2 格式 | NV12，1632×1224，单 plane，多平面 API |
| `VIDIOC_EXPBUF` | 4/4 PASS |
| MPP 导入相机 DMA-BUF | 4/4 PASS |
| RGA 导入相机 DMA-BUF | 4/4 PASS |
| RGA 导入 MPP 目标 DMA-BUF | PASS |
| RGA 同步布局拷贝 | PASS，约 1.25 ms（单帧） |
| 拷贝前后可见 NV12 校验值 | 完全相同 |
| 摄像头画面落盘 | 无 |

一次审计中的完整可见区域校验值为：

```text
source      = 7238748721224972810
destination = 7238748721224972810
```

该结果证明 RGA 能在两个 DMA-BUF 之间搬运当前帧，并正确改变 UV plane 的起始位置；它不是长期吞吐量或端到端延迟 benchmark。

## 三种“大小”不能混为一谈

板端同一个 V4L2 buffer 同时观察到：

| 字段 | 字节数 | 含义 |
|---|---:|---|
| `size_image` / `bytesused` | 2,996,352 | 当前有效 NV12 图像数据 |
| `VIDIOC_QUERYBUF` length | 3,015,936 | V4L2 导出缓冲的可映射长度 |
| DMA-BUF `fstat.st_size` | 3,018,752 | 内核分配对象大小，含页级对齐 |

缓冲“容量足够大”不代表图像分量排布相同。相机有效 NV12 的 UV plane 从：

```text
1632 × 1224 = 1,997,568 bytes
```

开始；当前 MPP 生产编码器把垂直 stride 对齐为 1232，因此要求 UV plane 从：

```text
1632 × 1232 = 2,010,624 bytes
```

开始。两个 UV offset 相差 13,056 bytes；当前 MPP 输入总大小比有效相机数据多 19,584 bytes。

## OBSERVED 与 INFERENCE

### OBSERVED

- RKISP 的 4 个 MMAP buffer 都能通过 `VIDIOC_EXPBUF` 导出 DMA-BUF fd。
- 这些 fd 都能被 MPP `MPP_BUFFER_TYPE_EXT_DMA` 导入。
- 这些 fd 都能被 RGA 导入。
- MPP 分配的目标 DMA-BUF 也能被 RGA 导入。
- RGA 能把一帧从 `1632×1224` 布局整理到当前 MPP 使用的 `1632×1232` 布局。
- 完整可见 Y/UV 内容在整理前后逐字节哈希一致。

### INFERENCE / 尚未验证

- MPP 导入 fd 成功只证明 buffer ownership/API 可互操作，不证明当前图像布局可以直接编码。
- 在保持现有 MPP `ver_stride=1232` 契约时，不能直接把 UV offset 为 1224 行的相机 buffer 当作编码输入。
- 尚未机械验证把 MPP `ver_stride` 改为 1224 后硬件编码器能否稳定、正确编码。因此不能声称“RK3588 硬件绝对不支持相机直连 MPP”。
- 本审计只测一帧，不能据此声称 CPU、RSS 或持续运行性能已经改善。

## 当前设计决定

后续持续编码实验采用的候选路径是：

```text
V4L2 MMAP/EXPBUF
        ↓ DMA-BUF fd
RGA 硬件布局整理
        ↓ aligned DMA-BUF
MPP H.264
```

该路径消除 CPU 逐行 `memcpy`，但仍发生一次由 RGA 执行的硬件拷贝，所以准确称呼是“DMA-BUF + RGA 硬件拷贝路径”，不是“纯零拷贝”。持续编码与 RTSP 对照结果分别见 [持续编码性能对照](ENCODING_BENCHMARK.md)和 [RTSP 完整链路验证](RTSP_VALIDATION.md)。

## 复现

```bash
cd /home/cat/Projects/rk3588-low-latency-stream
./scripts/build_board.sh
./scripts/run_dmabuf_probe.sh
```

脚本只运行一次有界的一帧诊断，不生成 `.h264`、截图或录像。
