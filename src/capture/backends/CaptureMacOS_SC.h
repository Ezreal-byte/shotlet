#pragma once
#include "capture/ScreenCapture.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#include <CoreGraphics/CoreGraphics.h>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>

// 前向声明 Objective-C 类
#ifdef __OBJC__
@ class ScreenCaptureDelegate;
@ class SCStream;
#else
typedef struct objc_object ScreenCaptureDelegate;
typedef struct objc_object SCStream;
#endif

namespace shotlet {
    // 权限状态
    enum class ScreenCapturePermission {
        Unknown, // 未知
        Granted, // 已授权
        Denied, // 被拒绝
        NotDetermined // 未决定（需要请求）
    };

    class CaptureMacOS : public IScreenCapture {
    public:
        CaptureMacOS();

        ~CaptureMacOS() override;

        bool Initialize() override;

        std::vector<DisplayInfo> GetDisplays() override;

        CaptureImage Capture(const CaptureOptions &options) override;

        std::string GetLastErrorMessage() const override;

        // 权限相关
        ScreenCapturePermission CheckPermission();

        bool RequestPermission(); // 返回是否已授权

        // 预热 ScreenCaptureKit 元数据缓存，避免首次截图阻塞在 getShareableContent。
        void WarmupShareableContentAsync();
        bool IsCoreGraphicsCaptureAvailable() const;

        // 使用回调的异步捕获（用于 SCStream）
        using CaptureCallback = std::function<void(CaptureImage &&)>;

        void CaptureAsync(const CaptureOptions &options, CaptureCallback callback);

    private:
        // 初始化 ScreenCaptureKit
        bool InitializeSC();

        void CleanupSC();

        // 权限缓存（进程内存）：进程内只做一次真实权限查询，
        // 截图失败时通过 InvalidatePermissionCache() 强制下次重查
        void InvalidatePermissionCache();

        // SCShareableContent 缓存（避免重复调用 getShareableContentWithCompletionHandler）
        void* GetCachedShareableContent();
        void ClearShareableContentCache();

        // 线程安全写错误信息（m_lastError 可能被 SC 回调线程与主线程并发访问）
        void SetLastError(const std::string& error);

        // 同步捕获（内部使用异步 + 等待）
        CaptureImage CaptureSync(const CaptureOptions &options);
        CaptureImage CaptureWithCoreGraphics(const CaptureOptions &options);

        // 获取显示器 ID 映射
        CGDirectDisplayID GetCGDisplayID(int id);

        // 图像处理
        CaptureImage ConvertCVPixelBufferToRGBA(void *pixelBuffer, int displayId);

        CaptureImage ConvertCGImageToRGBA(void *cgImage, int displayId);

        std::string m_lastError;
        mutable std::mutex m_errorMutex; // 保护 m_lastError（回调线程/主线程并发访问）
        bool m_initialized = false;
        bool m_scAvailable = false; // ScreenCaptureKit 是否可用

        // 权限缓存（进程内存）
        ScreenCapturePermission m_cachedPermission = ScreenCapturePermission::Unknown;
        bool m_permissionChecked = false;

        // SCShareableContent 缓存
        void* m_cachedShareableContent = nullptr; // SCShareableContent*
        std::mutex m_contentMutex; // 保护缓存访问
        std::condition_variable m_contentCv;
        bool m_shareableContentLoading = false;
        bool m_shareableContentWarmupStarted = false;
        std::thread m_shareableContentWarmupThread;

        // SCStream 相关（Objective-C 对象）
        void *m_stream = nullptr; // SCStream*
        void *m_delegate = nullptr; // ScreenCaptureDelegate*

        // 同步等待
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_captureComplete = false;
        CaptureImage m_capturedImage;
    };
} // namespace shotlet

#endif // SHOTLET_PLATFORM_MACOS
