# 滚动截图真实样例

`pure_color.png` 是用户提供的 1380×2652 终端截图，包含大面积深色背景、稀疏文字和时间戳。保留原文件，不缩放、不重绘。

对应 `tests/test_scroll_capture_real_image.cpp`，加入跨平台的 `shotlet_tests`。复用项目已有 SDL 的 `SDL_LoadPNG()` 解码 PNG，不增加解码库，不依赖 macOS 图像框架或网络下载。统一转换为 RGBA 字节排列，并按实际行跨度复制像素。测试从原图裁出 900 像素高的重叠视口，覆盖无先验向下／向上滚动、先验偏小一半及纯色画面不追加；完整滚动的导出结果与解码后的源图逐字节比较。

运行：`build/tests/shotlet_tests --gtest_filter="ScrollCaptureRealImageTest.*"`（Windows 使用实际构建目录下的 `shotlet_tests.exe`）。样例缺失或损坏会使测试失败，不会跳过。

这些用例验证静态源图的滚动帧回放，不模拟真实页面动画、固定控件或滚轮注入。
