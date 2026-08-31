cmake_minimum_required(VERSION 3.16)

project(rk3588_low_latency_stream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# 采集库只依赖 Linux/V4L2 和线程库，因此主机也能编译接口与离线测试。
# MPP、RGA、FFmpeg 目标放在后面的条件块中：主机缺少板端 SDK 时不会伪造硬件构建。
add_library(rkstream_capture
    src/pixel_format.cpp
    src/v4l2_capture.cpp
    src/v4l2_dmabuf_capture.cpp
)
target_include_directories(rkstream_capture PUBLIC include)
target_link_libraries(rkstream_capture PUBLIC Threads::Threads)
target_compile_options(rkstream_capture PRIVATE -Wall -Wextra -Wpedantic)

add_executable(capture_probe apps/capture_probe.cpp)
target_link_libraries(capture_probe PRIVATE rkstream_capture)
target_compile_options(capture_probe PRIVATE -Wall -Wextra -Wpedantic)

# 板端 Debian 包把 MPP 头文件装在 /usr/include/rockchip，RGA 头文件通常在
# /usr/include/rga。find_path/find_library 让脚本不依赖硬编码完整绝对路径。
find_path(MPP_INCLUDE_DIR rk_mpi.h PATH_SUFFIXES rockchip)
find_library(MPP_LIBRARY rockchip_mpp)
find_path(RGA_INCLUDE_DIR im2d.h PATH_SUFFIXES rga)
find_library(RGA_LIBRARY rga)
if(MPP_INCLUDE_DIR AND MPP_LIBRARY)
    add_library(rkstream_mpp src/mpp_h264_encoder.cpp src/annexb.cpp)
    target_include_directories(rkstream_mpp PUBLIC include PRIVATE ${MPP_INCLUDE_DIR})
    target_link_libraries(rkstream_mpp PUBLIC ${MPP_LIBRARY})
    target_compile_options(rkstream_mpp PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(mpp_file_encode apps/mpp_file_encode.cpp)
    target_link_libraries(mpp_file_encode PRIVATE rkstream_capture rkstream_mpp)
    target_compile_options(mpp_file_encode PRIVATE -Wall -Wextra -Wpedantic)

    # RTSP 只在 libavformat/libavcodec/libavutil 开发包齐全时构建。
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libavutil)
    endif()
    if(FFMPEG_FOUND)
        add_library(rkstream_rtsp src/rtsp_publisher.cpp)
        target_include_directories(rkstream_rtsp PUBLIC include)
        target_link_libraries(rkstream_rtsp PUBLIC PkgConfig::FFMPEG)
        target_compile_options(rkstream_rtsp PRIVATE -Wall -Wextra -Wpedantic)

        add_executable(rtsp_stream apps/rtsp_stream.cpp)
        target_link_libraries(rtsp_stream PRIVATE
            rkstream_capture rkstream_mpp rkstream_rtsp Threads::Threads)
        target_compile_options(rtsp_stream PRIVATE -Wall -Wextra -Wpedantic)
        message(STATUS "FFmpeg found: rtsp_stream enabled")
    else()
        message(STATUS "FFmpeg development libraries not found: rtsp_stream skipped")
    endif()
    message(STATUS "Rockchip MPP found: mpp_file_encode enabled")
else()
    message(STATUS "Rockchip MPP not found: hardware encoder target skipped")
endif()

# DMA-BUF 优化目标必须同时具有 MPP 与 RGA；缺任意一项都明确跳过。
if(MPP_INCLUDE_DIR AND MPP_LIBRARY AND RGA_INCLUDE_DIR AND RGA_LIBRARY)
    add_library(rkstream_rga src/rga_nv12_aligner.cpp)
    target_include_directories(rkstream_rga PUBLIC include PRIVATE ${RGA_INCLUDE_DIR})
    target_link_libraries(rkstream_rga PUBLIC ${RGA_LIBRARY})
    target_compile_options(rkstream_rga PRIVATE -Wall -Wextra)

    add_executable(dmabuf_probe apps/dmabuf_probe.cpp)
    target_include_directories(dmabuf_probe PRIVATE
        include ${MPP_INCLUDE_DIR} ${RGA_INCLUDE_DIR})
    target_link_libraries(dmabuf_probe PRIVATE ${MPP_LIBRARY} ${RGA_LIBRARY})
    # librga 2.2.0 public headers contain anonymous structs, so -Wpedantic would report
    # third-party declarations rather than project code for this board-only diagnostic target.
    target_compile_options(dmabuf_probe PRIVATE -Wall -Wextra)

    add_executable(dmabuf_mpp_encode apps/dmabuf_mpp_encode.cpp)
    target_link_libraries(dmabuf_mpp_encode PRIVATE
        rkstream_capture rkstream_mpp rkstream_rga)
    target_compile_options(dmabuf_mpp_encode PRIVATE -Wall -Wextra)

    if(FFMPEG_FOUND)
        add_executable(dmabuf_rtsp_stream apps/dmabuf_rtsp_stream.cpp)
        target_link_libraries(dmabuf_rtsp_stream PRIVATE
            rkstream_capture rkstream_mpp rkstream_rga rkstream_rtsp Threads::Threads)
        target_compile_options(dmabuf_rtsp_stream PRIVATE -Wall -Wextra -Wpedantic)
        message(STATUS "DMA-BUF/RGA RTSP target enabled")
    endif()
    message(STATUS "Rockchip MPP/RGA found: dmabuf_probe enabled")
else()
    message(STATUS "Rockchip MPP/RGA not found: dmabuf_probe skipped")
endif()

if(BUILD_TESTING)
    # 单元测试刻意不依赖摄像头。真实 ioctl/RGA/MPP/RTSP 由板端运行脚本验证。
    add_executable(test_bounded_latest_queue tests/test_bounded_latest_queue.cpp)
    target_include_directories(test_bounded_latest_queue PRIVATE include)
    target_link_libraries(test_bounded_latest_queue PRIVATE Threads::Threads)
    target_compile_options(test_bounded_latest_queue PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME bounded_latest_queue COMMAND test_bounded_latest_queue)

    add_executable(test_pixel_format tests/test_pixel_format.cpp src/pixel_format.cpp)
    target_include_directories(test_pixel_format PRIVATE include)
    target_compile_options(test_pixel_format PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME pixel_format COMMAND test_pixel_format)

    add_executable(test_annexb tests/test_annexb.cpp src/annexb.cpp)
    target_include_directories(test_annexb PRIVATE include)
    target_compile_options(test_annexb PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME annexb COMMAND test_annexb)

    add_executable(test_keyframe_resync tests/test_keyframe_resync.cpp)
    target_include_directories(test_keyframe_resync PRIVATE include)
    target_compile_options(test_keyframe_resync PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME keyframe_resync COMMAND test_keyframe_resync)

    add_executable(test_nv12_layout tests/test_nv12_layout.cpp)
    target_include_directories(test_nv12_layout PRIVATE include)
    target_compile_options(test_nv12_layout PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME nv12_layout COMMAND test_nv12_layout)

    add_executable(test_dmabuf_queue_policy tests/test_dmabuf_queue_policy.cpp)
    target_include_directories(test_dmabuf_queue_policy PRIVATE include)
    target_compile_options(test_dmabuf_queue_policy PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME dmabuf_queue_policy COMMAND test_dmabuf_queue_policy)
endif()
