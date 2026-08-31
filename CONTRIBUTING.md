# 贡献指南

感谢你关注本项目。提交改动前，请先说明问题、目标硬件和可复现条件。

## 开发原则

- 保持 C++17，避免为简单功能引入重量级依赖；
- 实时队列必须有明确上限，不允许随运行时间无限增长；
- DMA-BUF、V4L2 buffer 和线程对象必须有清晰 ownership；
- 性能结论必须注明硬件、分辨率、编码参数、样本时长与测量方法；
- 不把内部管线耗时写成未经光学测量的 glass-to-glass 延迟；
- 新增用户可见说明时优先提供中文，必要时可补充英文版本。

## 提交前检查

普通 Linux 主机至少运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
bash -n scripts/*.sh
```

改动涉及 V4L2、MPP、RGA 或 RTSP 时，还应在 RK3588 上运行：

```bash
./scripts/build_board.sh
```

涉及视频链路的行为变化，请附对应脚本 summary；不要提交摄像头画面、码流、构建目录、运行日志或第三方二进制。

## Pull Request

一个 PR 只解决一个清晰问题。说明：

1. 改了什么；
2. 为什么需要；
3. 如何验证；
4. 是否改变资源上限、ownership、延迟或兼容性；
5. 哪些硬件路径没有实际验证。

代码注释应解释设计原因、所有权和边界，不逐行翻译语法。
