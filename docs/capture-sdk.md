# 截图 SDK 与宿主运行时

SDK 供 Go、C、C++ 等宿主复用。推荐宿主直接打包 `runtime/` 内的预编译辅助程序，通过进程通信接收图片，无需编译 Shotlet、SDL 或 ImGui，也无需 Node.js。

## 构建与导出

在 Shotlet 仓库运行：

```sh
python3 scripts/export-capture-sdk.py
```

默认使用独立 `build/capture-sdk/` 构建目录，导出到 `dist/capture-sdk/darwin-arm64/` 或相应平台架构目录。可用 `--build-dir`、`--output`、`--config` 与 `--jobs` 调整；不会改写既有应用或 Node addon 的构建缓存。`SHOTLET_BUILD_CAPTURE_SDK` 默认关闭，既有构建行为不变。

产物包含公共 C 头文件、SDK/core/SDL 静态库、`ShotletCaptureConfig.cmake`、辅助程序、资源、依赖许可与 `manifest.json`。清单记录格式、ABI、协议版本、平台、架构、最低系统版本、`capabilities`（包含 `capture`、`permissions-v1`、`host-permissions-v1` 与 `actions-v1`）及 SHA-256；SDK 静态库只适用于对应平台、架构与兼容 C++ 工具链。macOS 辅助程序使用本机 ad-hoc 签名，正式分发由宿主统一完成签名、公证。

## 宿主模式范围

宿主模式提供与独立应用相同的截图工具栏：标注、文字提取（OCR）、置顶贴图、保存文件、录制选区和完成。OCR、贴图、保存、录制由 SDK 在本进程内打开对应窗口并执行，不向宿主返回内容，结束后返回 `ACTION_DONE`（4）。不读取或保存独立应用设置，不创建截图历史、最近截图、托盘或全局热键，不改写系统剪贴板。贴图或 OCR 窗口存在期间，辅助程序保持运行，直到这些窗口全部关闭才退出，因此需要贴图常驻的宿主应使用辅助程序而不是 C 接口。原有独立应用和 Node addon 不注册宿主回调，保持原行为。

普通截图与长截图在相同完成出口交付本次图片，图像内存共享并保留到宿主释放结果，避免额外复制整张图片。长截图入口是否可见仍遵循 Shotlet 的现有开关，不为 SDK 自动启用尚未开放的功能。

## 辅助程序协议

macOS 可执行文件为 `runtime/ShotletCapture.app/Contents/MacOS/ShotletCapture`，Windows 为 `runtime/shotlet-capture.exe`。启动参数：

```sh
ShotletCapture --theme dark --language zh-CN --log-dir /path/to/logs --screen-recording 1
```

截图参数支持 `--theme auto|light|dark`、`--language auto|zh-CN|en`、`--log-dir` 和兼容的 `--no-console-log`。正常日志写文件，stdout 专用于结果：8 字节签名 `ZMSHOT1\n`、小端 uint32 实际像素宽高、紧密排列 RGBA 数据，每像素 4 字节。上限 6710 万像素（256MB RGBA，与滚动截图上限一致），不重复乘显示密度。0 表示成功并有图片，1 表示失败，2 表示用户取消，3 表示宿主声明缺少屏幕录制权限，4 表示用户选择了 OCR、贴图、保存或录制并已在 SDK 内完成（无图片，stdout 无内容）。macOS 必须传 `--screen-recording 0|1`，缺失时返回 1；其他平台忽略该参数。宿主取消任务时结束并等待此进程，不要据此读取系统剪贴板。

`--self-test` 返回合成的单个红色像素，不初始化窗口或采集真实屏幕，可用于安装包协议自检。

## 权限由宿主管理

`--permissions` 返回无窗口 JSON，不采集屏幕，也不弹系统授权提示：

```json
{"version":1,"settings_available":true,"screen_recording":false,"accessibility":false}
```

macOS 检查的是辅助程序当前权限身份；其他平台返回 `settings_available:false`，普通截图不受 macOS 权限门控。宿主应要求屏幕录制权限，将辅助功能视为可选增强，并自行绘制引导和打开系统设置。

仅在用户主动点击时调用 `--request-permission screen` 或 `--request-permission accessibility`，触发系统权限登记后返回同一 JSON。纯查询不申请权限。截图入口不再自行查询权限，由宿主通过 `--screen-recording`（C 接口为 `shotlet_capture_options.screen_recording`）传入；传入 0 时直接返回 3，不使用独立应用的自动设置跳转或等待授权流程。录制开始时系统仍会处理麦克风等录制专属权限。更改屏幕权限后，宿主可提示重启；授权、权限归属及设置列表名称应在最终安装包中手动验证。

C 调用方对应使用 `shotlet_capture_query_permissions` 和 `shotlet_capture_request_permission`；先初始化 `shotlet_capture_permissions.struct_size`。查询只读，request 必须来自用户操作。

## C 接口

```cmake
project(Consumer LANGUAGES C CXX)
find_package(ShotletCapture CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE Shotlet::Capture)
```

设置 `CMAKE_PREFIX_PATH` 指向 SDK 根目录；宿主无需 Shotlet、SDL、ImGui 内部头文件。

```c
#include <shotlet/capture.h>
shotlet_capture_options options = {
    sizeof(options), SHOTLET_CAPTURE_ABI_VERSION, "dark", "zh-CN", "/path/to/logs",
    1 /* 宿主已检测到屏幕录制权限 */
};
shotlet_capture_result result = {0};
result.struct_size = sizeof(result);
int status = shotlet_capture_run(&options, &result);
/* SUCCESS 时读取 result.rgba、width_pixels、height_pixels、row_bytes、data_size；
   ACTION_DONE 表示 OCR、贴图、保存或录制已在 SDK 内完成，无图片。 */
shotlet_capture_result_release(&result);
```

C 接口必须在主线程调用，阻塞到本次交互结束，不可与其他 Shotlet Application 同时运行；Go/Viva 等已有原生事件循环的宿主应使用辅助进程。字符串仅在调用期间借用，输出像素归 SDK 所有，不得直接 free；必须通过 `shotlet_capture_result_release` 释放，空结果与重复释放安全。结构体大小与 ABI 不匹配、无效主题/语言会在申请系统权限前失败，C++ 异常不跨 C ABI 传播。

导出命令运行不依赖 GPU 的 C ABI 和工具栏契约验证。实际屏幕权限、选区、多屏及显示密度仍需各平台手动验收。
