#include "shotlet/capture.h"
#include "app/Application.h"
#include "platform/PlatformMainContext.h"
#include "utils/Logger.h"
#include "utils/ScopeExit.h"
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace {
std::mutex captureMutex;

bool ValidOption(const char* value, const char* first, const char* second, const char* third) {
    return value == nullptr || std::strcmp(value, first) == 0 || std::strcmp(value, second) == 0 ||
        std::strcmp(value, third) == 0;
}

bool ValidRequest(const shotlet_capture_options* options, const shotlet_capture_result* result) {
    return options && result && options->struct_size == sizeof(*options) &&
        options->abi_version == SHOTLET_CAPTURE_ABI_VERSION && result->struct_size == sizeof(*result) &&
        result->owner == nullptr && result->rgba == nullptr &&
        ValidOption(options->theme, "auto", "light", "dark") &&
        ValidOption(options->language, "auto", "zh-CN", "en");
}

bool AcceptImage(const shotlet::HistoryImage& image) {
    if (!image.pixels || image.width <= 0 || image.height <= 0) return false;
    return static_cast<uint64_t>(image.width) * image.height <= SHOTLET_CAPTURE_MAX_PIXELS;
}

int RunCapture(const shotlet_capture_options& options, shotlet_capture_result& result) {
    shotlet::PlatformAutoreleasePool pool;
    std::vector<std::string> arguments{"shotlet-capture", "--no-console-log", "--theme",
        options.theme ? options.theme : "auto", "--language", options.language ? options.language : "auto"};
    if (options.log_directory && options.log_directory[0] != '\0') {
        arguments.emplace_back("--log-dir");
        arguments.emplace_back(options.log_directory);
    }
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    shotlet::HistoryImage captured;
    {
        shotlet::Application application;
        application.SetHostCaptureOutput([&captured](const shotlet::HistoryImage& image) {
            if (!AcceptImage(image)) return false;
            // 共享已有像素的所有权，避免宿主接口额外复制整张长截图。
            captured = image;
            return true;
        });
        if (!application.Initialize(static_cast<int>(arguments.size()), argv.data())) return SHOTLET_CAPTURE_ERROR;
        const int status = application.Run();
        // 显式映射，避免内部退出码调整时 C ABI 静默错报。
        if (!captured.pixels && application.HostActionHandled()) return SHOTLET_CAPTURE_ACTION_DONE;
        switch (status) {
            case shotlet::ExitSuccess: break;
            case shotlet::ExitCancelled: return SHOTLET_CAPTURE_CANCELLED;
            default: return SHOTLET_CAPTURE_ERROR;
        }
    }
    if (!captured.pixels) return SHOTLET_CAPTURE_CANCELLED;
    auto owner = std::make_unique<shotlet::HistoryImage>(std::move(captured));
    result.width_pixels = owner->width;
    result.height_pixels = owner->height;
    result.row_bytes = result.width_pixels * SHOTLET_CAPTURE_PIXEL_BYTES;
    result.data_size = static_cast<uint64_t>(result.row_bytes) * result.height_pixels;
    result.rgba = owner->pixels.get();
    result.owner = owner.release();
    return SHOTLET_CAPTURE_SUCCESS;
}
}

extern "C" uint32_t shotlet_capture_abi_version(void) { return SHOTLET_CAPTURE_ABI_VERSION; }

extern "C" int shotlet_capture_run(const shotlet_capture_options* options, shotlet_capture_result* result) {
    if (!ValidRequest(options, result)) return SHOTLET_CAPTURE_ERROR;
#if defined(__APPLE__)
    if (!pthread_main_np()) return SHOTLET_CAPTURE_ERROR;
#endif
    int status = SHOTLET_CAPTURE_ERROR;
    try {
        std::unique_lock lock(captureMutex, std::try_to_lock);
        if (!lock.owns_lock() || shotlet::Application::Instance() != nullptr) return SHOTLET_CAPTURE_ERROR;
        // 日志资源先于互斥锁释放，异常时也不允许下一轮与清理过程交错。
        shotlet::ScopeExit logging([] { shotlet::Logger::Shutdown(); });
#if defined(__APPLE__)
        // 权限由宿主传入，SDK 不查询也不弹引导、不等待授权。
        if (!options->screen_recording) return SHOTLET_CAPTURE_PERMISSION_REQUIRED;
#endif
        status = RunCapture(*options, *result);
    } catch (...) {
        // C ABI 边界不传播 C++ 异常，已构造资源按作用域释放。
        shotlet_capture_result_release(result);
    }
    return status;
}

extern "C" void shotlet_capture_result_release(shotlet_capture_result* result) {
    if (!result || result->struct_size != sizeof(*result)) return;
    delete static_cast<shotlet::HistoryImage*>(result->owner);
    *result = {};
    result->struct_size = sizeof(*result);
}

extern "C" int shotlet_capture_query_permissions(shotlet_capture_permissions* permissions) {
    if (!permissions || permissions->struct_size != sizeof(*permissions)) return SHOTLET_CAPTURE_ERROR;
#if defined(__APPLE__)
    permissions->settings_available = 1;
    permissions->screen_recording = CGPreflightScreenCaptureAccess() ? 1 : 0;
    permissions->accessibility = AXIsProcessTrusted() ? 1 : 0;
#else
    // Windows 无对应的 macOS TCC 开关，不以无关权限阻止截图。
    permissions->settings_available = 0;
    permissions->screen_recording = 1;
    permissions->accessibility = 1;
#endif
    return SHOTLET_CAPTURE_SUCCESS;
}

extern "C" int shotlet_capture_request_permission(uint32_t permission) {
    if (permission != SHOTLET_PERMISSION_SCREEN_RECORDING && permission != SHOTLET_PERMISSION_ACCESSIBILITY)
        return SHOTLET_CAPTURE_ERROR;
#if defined(__APPLE__)
    if (permission == SHOTLET_PERMISSION_SCREEN_RECORDING) {
        if (!CGPreflightScreenCaptureAccess()) (void)CGRequestScreenCaptureAccess();
    } else if (!AXIsProcessTrusted()) {
        const void* keys[] = {kAXTrustedCheckOptionPrompt};
        const void* values[] = {kCFBooleanTrue};
        CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!options) return SHOTLET_CAPTURE_ERROR;
        (void)AXIsProcessTrustedWithOptions(options);
        CFRelease(options);
    }
    return SHOTLET_CAPTURE_SUCCESS;
#else
    return SHOTLET_CAPTURE_ERROR;
#endif
}
