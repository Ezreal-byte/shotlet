#include "shotlet/capture.h"
#include "platform/PlatformMainContext.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {
constexpr char ResultMagic[] = "ZMSHOT1\n";
constexpr std::size_t DimensionBytes = sizeof(uint32_t);

bool WriteDimension(uint32_t value) {
    std::array<unsigned char, DimensionBytes> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<unsigned char>(value >> (index * 8));
    return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size();
}

int WriteResult(uint32_t width, uint32_t height, const unsigned char* rgba, uint64_t size) {
    if (!rgba || width == 0 || height == 0 || static_cast<uint64_t>(width) * height > SHOTLET_CAPTURE_MAX_PIXELS ||
        size != static_cast<uint64_t>(width) * height * SHOTLET_CAPTURE_PIXEL_BYTES)
        return SHOTLET_CAPTURE_ERROR;
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const bool written = std::fwrite(ResultMagic, 1, sizeof(ResultMagic) - 1, stdout) == sizeof(ResultMagic) - 1 &&
        WriteDimension(width) && WriteDimension(height) &&
        std::fwrite(rgba, 1, static_cast<std::size_t>(size), stdout) == size && std::fflush(stdout) == 0;
    return written ? SHOTLET_CAPTURE_SUCCESS : SHOTLET_CAPTURE_ERROR;
}

int WritePermissions() {
    shotlet_capture_permissions permissions{};
    permissions.struct_size = sizeof(permissions);
    const int status = shotlet_capture_query_permissions(&permissions);
    if (status != SHOTLET_CAPTURE_SUCCESS) return status;
    const int written = std::printf("{\"version\":1,\"settings_available\":%s,\"screen_recording\":%s,\"accessibility\":%s}\n",
        permissions.settings_available ? "true" : "false", permissions.screen_recording ? "true" : "false",
        permissions.accessibility ? "true" : "false");
    return written > 0 && std::fflush(stdout) == 0 ? SHOTLET_CAPTURE_SUCCESS : SHOTLET_CAPTURE_ERROR;
}

int Run(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--permissions") == 0) return WritePermissions();
    if (argc == 3 && std::strcmp(argv[1], "--request-permission") == 0) {
        uint32_t permission = 0;
        if (std::strcmp(argv[2], "screen") == 0) permission = SHOTLET_PERMISSION_SCREEN_RECORDING;
        else if (std::strcmp(argv[2], "accessibility") == 0) permission = SHOTLET_PERMISSION_ACCESSIBILITY;
        if (shotlet_capture_request_permission(permission) != SHOTLET_CAPTURE_SUCCESS) return SHOTLET_CAPTURE_ERROR;
        return WritePermissions();
    }

    // 自检只返回合成像素，不初始化 SDL、窗口或屏幕采集。
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) {
        constexpr std::array<unsigned char, SHOTLET_CAPTURE_PIXEL_BYTES> pixel{255, 0, 0, 255};
        return WriteResult(1, 1, pixel.data(), pixel.size());
    }
#if defined(__APPLE__)
    shotlet_capture_options options{sizeof(options), SHOTLET_CAPTURE_ABI_VERSION, "auto", "auto", nullptr, 0};
    bool screenRecordingProvided = false;
#else
    shotlet_capture_options options{sizeof(options), SHOTLET_CAPTURE_ABI_VERSION, "auto", "auto", nullptr, 1};
#endif
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--no-console-log") continue;
        if (index + 1 >= argc) return SHOTLET_CAPTURE_ERROR;
        const char* value = argv[++index];
        if (argument == "--theme") options.theme = value;
        else if (argument == "--language") options.language = value;
        else if (argument == "--log-dir") options.log_directory = value;
        else if (argument == "--screen-recording") {
            if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0) return SHOTLET_CAPTURE_ERROR;
            options.screen_recording = value[0] == '1' ? 1 : 0;
#if defined(__APPLE__)
            screenRecordingProvided = true;
#endif
        }
        else return SHOTLET_CAPTURE_ERROR;
    }
#if defined(__APPLE__)
    // 权限状态必须由宿主明确传入，缺失时报错，避免静默按未授权或已授权处理。
    if (!screenRecordingProvided) return SHOTLET_CAPTURE_ERROR;
#endif
    shotlet_capture_result result{};
    result.struct_size = sizeof(result);
    const int status = shotlet_capture_run(&options, &result);
    const int output = status == SHOTLET_CAPTURE_SUCCESS
        ? WriteResult(result.width_pixels, result.height_pixels, result.rgba, result.data_size) : status;
    shotlet_capture_result_release(&result);
    return output;
}
}

int main(int argc, char** argv) {
    try {
        return shotlet::RunWithPlatformMainContext(Run, argc, argv);
    } catch (...) {
        std::fprintf(stderr, "capture helper failed\n");
        return SHOTLET_CAPTURE_ERROR;
    }
}
