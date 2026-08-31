# 第三方依赖

本仓库不提交第三方 SDK、预编译模型、摄像头画面或运行生成的码流。项目源码采用 MIT License；外部组件继续遵循各自许可证与分发条款。

| 组件 | 用途 | 获取方式 |
|---|---|---|
| Linux V4L2 | 摄像头 ioctl、MMAP、DMA-BUF 导出 | 板端内核与系统头文件 |
| [Rockchip MPP](https://github.com/rockchip-linux/mpp) | RK3588 VPU H.264 硬件编码 | 板端系统开发包 |
| [librga](https://github.com/airockchip/librga) | NV12 DMA-BUF 布局转换 | 板端系统开发包 |
| [FFmpeg](https://ffmpeg.org/) | RTSP mux、ffprobe 与解码验证 | 系统运行/开发包 |
| [MediaMTX](https://github.com/bluenviron/mediamtx) | RTSP 服务端 | 安装脚本下载固定版本并校验 SHA256 |

发布或再分发包含这些组件的完整系统镜像前，请单独检查对应版本的许可证、NOTICE 和动态/静态链接义务。本文件不替代法律意见。
