#include "CaptureMacOS_SC.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <dlfcn.h>
#include <limits>

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreImage/CoreImage.h>
#import <AppKit/AppKit.h>
#import <objc/message.h>

// ScreenCaptureKit (macOS 12.3+)
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
#import <ScreenCaptureKit/ScreenCaptureKit.h>

@interface SHOTLETStreamOutput : NSObject <SCStreamOutput>
@property (nonatomic, copy) void (^frameHandler)(CGImageRef image, NSError* error);
@property (nonatomic, assign) BOOL gotFrame;
@end

@implementation SHOTLETStreamOutput

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || self.gotFrame) {
        return;
    }
    
    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) {
        if (self.frameHandler) {
            self.frameHandler(nil, [NSError errorWithDomain:@"shotlet" code:-1 userInfo:@{NSLocalizedDescriptionKey: @"No image buffer"}]);
        }
        return;
    }
    
    // 创建 CIImage 然后转换为 CGImage
    CIImage* ciImage = [CIImage imageWithCVImageBuffer:imageBuffer];
    CIContext* context = [CIContext contextWithOptions:nil];
    CGImageRef cgImage = [context createCGImage:ciImage fromRect:[ciImage extent]];
    
    if (cgImage) {
        self.gotFrame = YES;
        if (self.frameHandler) {
            self.frameHandler(cgImage, nil);
        }
        CGImageRelease(cgImage);
    }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    if (!self.gotFrame && self.frameHandler) {
        self.frameHandler(nil, error);
    }
}

@end

#endif

namespace shotlet {

namespace {
using Clock = std::chrono::high_resolution_clock;

float ElapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<float, std::milli>(end - start).count();
}

using CGDisplayCreateImageFunc = CGImageRef (*)(CGDirectDisplayID);
using SCScreenshotCompletionHandler = void (^)(CGImageRef image, NSError* error);
using SCScreenshotCaptureImageFunc = void (*)(id, SEL, SCContentFilter*, SCStreamConfiguration*, SCScreenshotCompletionHandler);

CGDisplayCreateImageFunc GetCGDisplayCreateImageFunc() {
    static auto* func = reinterpret_cast<CGDisplayCreateImageFunc>(
        dlsym(RTLD_DEFAULT, "CGDisplayCreateImage"));
    return func;
}

bool CaptureImageWithSCScreenshotManager(SCContentFilter* filter,
                                         SCStreamConfiguration* config,
                                         SCScreenshotCompletionHandler completionHandler) {
    Class screenshotManagerClass = NSClassFromString(@"SCScreenshotManager");
    SEL captureSelector = NSSelectorFromString(@"captureImageWithFilter:configuration:completionHandler:");
    if (!screenshotManagerClass || ![screenshotManagerClass respondsToSelector:captureSelector]) {
        return false;
    }

    auto captureImage = reinterpret_cast<SCScreenshotCaptureImageFunc>(objc_msgSend);
    captureImage(screenshotManagerClass, captureSelector, filter, config, completionHandler);
    return true;
}
}

CaptureMacOS::CaptureMacOS() = default;
CaptureMacOS::~CaptureMacOS() {
    if (m_shareableContentWarmupThread.joinable()) {
        m_shareableContentWarmupThread.join();
    }
    // GetCachedShareableContent 的晚到回调持有 this 写缓存，
    // 析构前等待尚在飞行中的请求落地（回调完成时会 notify）
    {
        std::unique_lock<std::mutex> lock(m_contentMutex);
        if (m_shareableContentLoading) {
            m_contentCv.wait_for(lock, std::chrono::seconds(5), [this]() {
                return !m_shareableContentLoading;
            });
        }
    }
    CleanupSC();
}

bool CaptureMacOS::Initialize() {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
    if (@available(macOS 12.3, *)) {
        m_scAvailable = true;
    } else {
        m_scAvailable = false;
        Logger::Info("ScreenCaptureKit not available (requires macOS 12.3+)");
    }
#else
    m_scAvailable = false;
    Logger::Info("ScreenCaptureKit not available (compile time)");
#endif
    
    m_initialized = true;
    return true;
}

bool CaptureMacOS::InitializeSC() {
    return true;
}

void CaptureMacOS::CleanupSC() {
    // 清理 SCShareableContent 缓存
    std::lock_guard<std::mutex> lock(m_contentMutex);
    if (m_cachedShareableContent) {
        SCShareableContent* content = (__bridge_transfer SCShareableContent*)m_cachedShareableContent;
        (void)content; // 避免未使用警告
        m_cachedShareableContent = nullptr;
    }
}

void* CaptureMacOS::GetCachedShareableContent() {
    {
        std::unique_lock<std::mutex> lock(m_contentMutex);
        if (m_cachedShareableContent) {
            return m_cachedShareableContent;
        }
        if (m_shareableContentLoading) {
            // 有界等待：loading 由回调复位，回调异常丢失时不能永久挂起
            m_contentCv.wait_for(lock, std::chrono::seconds(10), [this]() {
                return m_cachedShareableContent || !m_shareableContentLoading;
            });
            return m_cachedShareableContent;
        }
        m_shareableContentLoading = true;
    }

    // 使用信号量同步等待获取 SCShareableContent。
    // 缓存写入与 loading 复位全部放在回调内完成（只执行一次）：
    // 即使等待方超时返回，晚到的回调仍会把结果写入缓存供后续调用使用，
    // 等待方超时后只读取缓存，绝不触碰回调可能正在写的共享变量。
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* c, NSError* error) {
        {
            std::lock_guard<std::mutex> lock(m_contentMutex);
            if (c) {
                // 保存到缓存（需要 retain）
                m_cachedShareableContent = (__bridge_retained void*)c;
            }
            m_shareableContentLoading = false;
        }
        m_contentCv.notify_all();
        dispatch_semaphore_signal(semaphore);
    }];

    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

    void* cachedContent = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_contentMutex);
        cachedContent = m_cachedShareableContent;
    }

    return cachedContent;
}

void CaptureMacOS::ClearShareableContentCache() {
    std::lock_guard<std::mutex> lock(m_contentMutex);
    if (m_cachedShareableContent) {
        SCShareableContent* content = (__bridge_transfer SCShareableContent*)m_cachedShareableContent;
        (void)content; // 避免未使用警告
        m_cachedShareableContent = nullptr;
    }
    m_shareableContentLoading = false;
    m_contentCv.notify_all();
}

void CaptureMacOS::WarmupShareableContentAsync() {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
    if (!m_scAvailable) return;

    {
        std::lock_guard<std::mutex> lock(m_contentMutex);
        if (m_cachedShareableContent || m_shareableContentWarmupStarted) {
            return;
        }
        m_shareableContentWarmupStarted = true;
    }

    m_shareableContentWarmupThread = std::thread([this]() {
        auto t0 = Clock::now();
        void* content = GetCachedShareableContent();
        auto t1 = Clock::now();
        Logger::Debug("[CaptureMacOS] ShareableContent warmup: {} in {:.1f}ms",
                      content ? "success" : "failed",
                      ElapsedMs(t0, t1));
    });
#endif
}

bool CaptureMacOS::IsCoreGraphicsCaptureAvailable() const {
    return GetCGDisplayCreateImageFunc() != nullptr;
}

void CaptureMacOS::InvalidatePermissionCache() {
    m_permissionChecked = false;
    m_cachedPermission = ScreenCapturePermission::Unknown;
}

void CaptureMacOS::SetLastError(const std::string& error) {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = error;
}

ScreenCapturePermission CaptureMacOS::CheckPermission() {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
    if (@available(macOS 12.3, *)) {
        // 进程内只做一次真实权限查询（替代原 24 小时磁盘缓存盲信：
        // 用户在系统设置中撤销"屏幕录制"权限后，截图失败路径会调用
        // InvalidatePermissionCache() 强制下次重查，感知延迟从最长 24h 降为一次截图）。
        if (m_permissionChecked) {
            return m_cachedPermission;
        }

        __block ScreenCapturePermission permission = ScreenCapturePermission::Unknown;
        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
            if (error) {
                NSString* errorDesc = [error localizedDescription];
                if ([errorDesc containsString:@"denied"] || error.code == -3801) {
                    permission = ScreenCapturePermission::Denied;
                } else if ([errorDesc containsString:@"permission"]) {
                    permission = ScreenCapturePermission::NotDetermined;
                } else {
                    permission = ScreenCapturePermission::Denied;
                }
            } else if (content) {
                permission = ScreenCapturePermission::Granted;
            }
            dispatch_semaphore_signal(semaphore);
        }];

        if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
            // 超时：回调仍可能晚到并写 permission，不读取可能被并发写的值
            return ScreenCapturePermission::Unknown;
        }

        m_cachedPermission = permission;
        m_permissionChecked = true;

        return permission;
    }
#endif
    return ScreenCapturePermission::Granted;
}

bool CaptureMacOS::RequestPermission() {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
    if (@available(macOS 12.3, *)) {
        NSString* urlString = @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
        NSURL* url = [NSURL URLWithString:urlString];
        [[NSWorkspace sharedWorkspace] openURL:url];
        
        Logger::Info("Please grant Screen Recording permission in System Settings");
        Logger::Info("System Settings > Privacy & Security > Screen Recording");
        
        return false;
    }
#endif
    return true;
}

std::vector<DisplayInfo> CaptureMacOS::GetDisplays() {
    @autoreleasepool {
    std::vector<DisplayInfo> displays;
    
    // 使用 CoreGraphics 获取显示器信息（更可靠）
    uint32_t displayCount = 0;
    CGError err = CGGetActiveDisplayList(0, nullptr, &displayCount);
    
    if (err == kCGErrorSuccess && displayCount > 0) {
        std::vector<CGDirectDisplayID> displayIDs(displayCount);
        err = CGGetActiveDisplayList(displayCount, displayIDs.data(), &displayCount);
        
        for (uint32_t i = 0; i < displayCount; i++) {
            CGDirectDisplayID displayID = displayIDs[i];
            
            DisplayInfo info;
            info.id = displayID <= static_cast<CGDirectDisplayID>(std::numeric_limits<int>::max())
                ? static_cast<int>(displayID)
                : static_cast<int>(i);
            info.name = "Display " + std::to_string(i + 1);
            
            CGRect bounds = CGDisplayBounds(displayID);
            info.displayX = static_cast<int>(bounds.origin.x);
            info.displayY = static_cast<int>(bounds.origin.y);

            // 获取像素尺寸（Retina 屏幕需要从 displayMode 获取）
            CGDisplayMode* mode = CGDisplayCopyDisplayMode(displayID);
            if (mode) {
                info.displayWidth = CGDisplayModeGetPixelWidth(mode);
                info.displayHeight = CGDisplayModeGetPixelHeight(mode);
                // 计算缩放比例
                info.scaleFactor = static_cast<float>(CGDisplayModeGetPixelWidth(mode)) / static_cast<float>(bounds.size.width);
                if (info.scaleFactor < 1.0f) info.scaleFactor = 1.0f;
                CGDisplayModeRelease(mode);
            } else {
                info.displayWidth = static_cast<int>(bounds.size.width);
                info.displayHeight = static_cast<int>(bounds.size.height);
                info.scaleFactor = 1.0f;
            }

            info.isPrimary = (displayID == CGMainDisplayID());
            
            info.refreshRate = 60;
            
            displays.push_back(info);
        }
    }
    
    return displays;
    }
}

CaptureImage CaptureMacOS::Capture(const CaptureOptions& options) {
    @autoreleasepool {
    auto t0 = Clock::now();

    if (!m_scAvailable) {
        SetLastError("ScreenCaptureKit not available");
        return CaptureImage{};
    }
    
    // 检查权限
    auto permission = CheckPermission();
    auto t_permission = Clock::now();
    if (permission != ScreenCapturePermission::Granted) {
        SetLastError("Screen Recording permission required");
        Logger::Debug("[CaptureMacOS] Capture display={} timing: CheckPermission={:.1f}ms, total={:.1f}ms (permission denied)",
                      options.displayId,
                      ElapsedMs(t0, t_permission),
                      ElapsedMs(t0, t_permission));
        return CaptureImage{};
    }
    
    if (options.displayId < 0) {
        SetLastError("displayId required (CaptureAll removed)");
        Logger::Debug("[CaptureMacOS] Capture display={} timing: CheckPermission={:.1f}ms, total={:.1f}ms (invalid display)",
                      options.displayId,
                      ElapsedMs(t0, t_permission),
                      ElapsedMs(t0, t_permission));
        return CaptureImage{};
    }

    auto cgResult = CaptureWithCoreGraphics(options);
    auto t_cg = Clock::now();
    if (cgResult.data) {
        Logger::Debug("[CaptureMacOS] Capture display={} timing: CheckPermission={:.1f}ms, CoreGraphics={:.1f}ms, total={:.1f}ms",
                      options.displayId,
                      ElapsedMs(t0, t_permission),
                      ElapsedMs(t_permission, t_cg),
                      ElapsedMs(t0, t_cg));
        return std::move(cgResult);
    }
    
    auto result = CaptureSync(options);
    auto t_end = Clock::now();
    Logger::Debug("[CaptureMacOS] Capture display={} timing: CheckPermission={:.1f}ms, CoreGraphicsFallback={:.1f}ms, CaptureSync={:.1f}ms, total={:.1f}ms",
                  options.displayId,
                  ElapsedMs(t0, t_permission),
                  ElapsedMs(t_permission, t_cg),
                  ElapsedMs(t_cg, t_end),
                  ElapsedMs(t0, t_end));
    if (!result.data) {
        // 截图失败可能是权限被系统设置撤销所致，强制下次 Capture 重查权限
        InvalidatePermissionCache();
    }
    return std::move(result);
    }
}

CaptureImage CaptureMacOS::CaptureWithCoreGraphics(const CaptureOptions& options) {
    @autoreleasepool {
    auto t0 = Clock::now();

    CGDirectDisplayID displayID = GetCGDisplayID(options.displayId);
    auto t_display = Clock::now();
    if (displayID == kCGNullDirectDisplay) {
        Logger::Debug("[CaptureMacOS] CoreGraphics display={} timing: GetCGDisplayID={:.1f}ms, failed",
                      options.displayId,
                      ElapsedMs(t0, t_display));
        return CaptureImage{};
    }

    auto* createImage = GetCGDisplayCreateImageFunc();
    if (!createImage) {
        Logger::Debug("[CaptureMacOS] CoreGraphics display={} timing: GetCGDisplayID={:.1f}ms, "
                      "CGDisplayCreateImage unavailable",
                      options.displayId,
                      ElapsedMs(t0, t_display));
        return CaptureImage{};
    }

    CGImageRef image = createImage(displayID);
    auto t_capture = Clock::now();
    if (!image) {
        Logger::Debug("[CaptureMacOS] CoreGraphics display={} timing: GetCGDisplayID={:.1f}ms, CGDisplayCreateImage={:.1f}ms, failed",
                      options.displayId,
                      ElapsedMs(t0, t_display),
                      ElapsedMs(t_display, t_capture));
        return CaptureImage{};
    }

    auto result = ConvertCGImageToRGBA(image, options.displayId);
    CGImageRelease(image);
    auto t_convert = Clock::now();

    if (result.data) {
        if (options.hasDisplayInfo) {
            result.captureX = options.displayInfo.displayX;
            result.captureY = options.displayInfo.displayY;
            result.scaleFactor = options.displayInfo.scaleFactor;
        } else {
            CGRect bounds = CGDisplayBounds(displayID);
            result.captureX = static_cast<int>(bounds.origin.x);
            result.captureY = static_cast<int>(bounds.origin.y);
            auto displays = GetDisplays();
            if (options.displayId >= 0 && options.displayId < (int)displays.size()) {
                result.scaleFactor = displays[options.displayId].scaleFactor;
            }
        }
    }

    Logger::Debug("[CaptureMacOS] CoreGraphics display={} timing: GetCGDisplayID={:.1f}ms, "
                  "CGDisplayCreateImage={:.1f}ms, ConvertCGImageToRGBA={:.1f}ms, total={:.1f}ms",
                  options.displayId,
                  ElapsedMs(t0, t_display),
                  ElapsedMs(t_display, t_capture),
                  ElapsedMs(t_capture, t_convert),
                  ElapsedMs(t0, t_convert));

    return std::move(result);
    }
}

CaptureImage CaptureMacOS::CaptureSync(const CaptureOptions& options) {
    @autoreleasepool {
    CaptureImage result;
    auto t0 = Clock::now();

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300
    if (@available(macOS 12.3, *)) {
        // 使用缓存的 SCShareableContent（避免每次调用 ~3s 的 API）
        SCShareableContent* content = (__bridge SCShareableContent*)GetCachedShareableContent();
        auto t_content = Clock::now();
        if (!content) {
            SetLastError("Failed to get shareable content");
            Logger::Debug("[CaptureMacOS] CaptureSync display={} timing: GetCachedShareableContent={:.1f}ms, total={:.1f}ms (no content)",
                          options.displayId,
                          ElapsedMs(t0, t_content),
                          ElapsedMs(t0, t_content));
            return std::move(result);
        }

        // 获取显示器信息。调用方通常已经在 OverlayManager 中获取过，优先复用。
        std::vector<DisplayInfo> displays;
        const DisplayInfo* targetInfo = nullptr;
        if (options.hasDisplayInfo) {
            targetInfo = &options.displayInfo;
        } else {
            displays = GetDisplays();
            if (options.displayId >= 0 && options.displayId < (int)displays.size()) {
                targetInfo = &displays[options.displayId];
            }
        }
        auto t_displays = Clock::now();

        // 找到目标显示器
        SCDisplay* targetDisplay = nil;

        for (SCDisplay* d in content.displays) {
            if (options.displayId < 0) {
                targetDisplay = d;
                result.captureX = (int)CGDisplayBounds(d.displayID).origin.x;
                result.captureY = (int)CGDisplayBounds(d.displayID).origin.y;
                break;
            }
            // 通过对比尺寸和位置来匹配显示器
            if (targetInfo) {
                const auto& info = *targetInfo;
                CGRect bounds = CGDisplayBounds(d.displayID);
                if ((int)bounds.origin.x == info.displayX && (int)bounds.origin.y == info.displayY) {
                    targetDisplay = d;
                    result.captureX = info.displayX;
                    result.captureY = info.displayY;
                    break;
                }
            }
        }

        if (!targetDisplay && content.displays.count > 0) {
            targetDisplay = content.displays[0];
            CGRect bounds = CGDisplayBounds(targetDisplay.displayID);
            result.captureX = (int)bounds.origin.x;
            result.captureY = (int)bounds.origin.y;
        }
        auto t_match = Clock::now();

        if (!targetDisplay) {
            Logger::Error("No target display found");
            Logger::Debug("[CaptureMacOS] CaptureSync display={} timing: GetCachedShareableContent={:.1f}ms, GetDisplays={:.1f}ms, MatchDisplay={:.1f}ms, total={:.1f}ms (no target)",
                          options.displayId,
                          ElapsedMs(t0, t_content),
                          ElapsedMs(t_content, t_displays),
                          ElapsedMs(t_displays, t_match),
                          ElapsedMs(t0, t_match));
            return std::move(result);
        }

        // 创建过滤器
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:targetDisplay excludingWindows:@[]];
        auto t_filter = Clock::now();

        // 配置 - 使用像素尺寸而非点尺寸
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        CGDirectDisplayID cgDisplayID = targetDisplay.displayID;
        CGDisplayMode* mode = CGDisplayCopyDisplayMode(cgDisplayID);
        if (mode) {
            config.width = CGDisplayModeGetPixelWidth(mode);
            config.height = CGDisplayModeGetPixelHeight(mode);
            CGDisplayModeRelease(mode);
        } else {
            config.width = targetDisplay.width;
            config.height = targetDisplay.height;
            Logger::Warning("Could not get pixel mode, using point dimensions");
        }
        config.showsCursor = options.cursor ? YES : NO;
        auto t_config = Clock::now();

        // 使用 SCScreenshotManager 进行一次性截图
        int targetX = result.captureX;
        int targetY = result.captureY;

        // 同步等待截图完成。
        // 超时后 SC 回调仍可能晚到：回调与等待方通过堆上的共享状态交互，
        // 对结果的所有读写都在锁内进行；晚到的回调发现已被放弃时只丢弃结果，
        // 不会触碰等待方已经返回的内存（修复 use-after-race）。
        struct SyncCaptureState {
            std::mutex mutex;
            bool abandoned = false; // 等待方已超时返回
            CaptureImage image;     // 仅在锁内读写
        };
        auto state = std::make_shared<SyncCaptureState>();

        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
        __block float callbackDelayMs = 0.0f;
        __block float convertMs = 0.0f;
        __block float callbackTotalMs = 0.0f;
        auto t_request_start = Clock::now();

        bool submitted = CaptureImageWithSCScreenshotManager(filter, config, ^(CGImageRef image, NSError* captureError) {
            auto t_callback_start = Clock::now();
            callbackDelayMs = ElapsedMs(t_request_start, t_callback_start);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->abandoned) {
                    Logger::Warning("[CaptureMacOS] CaptureSync display={} late callback dropped (timed out)",
                                    options.displayId);
                } else if (captureError) {
                    Logger::Error("Screenshot failed: {}",
                        [[captureError localizedDescription] UTF8String]);
                } else if (image) {
                    auto t_convert_start = Clock::now();
                    state->image = ConvertCGImageToRGBA(image, options.displayId);
                    auto t_convert_end = Clock::now();
                    convertMs = ElapsedMs(t_convert_start, t_convert_end);
                    state->image.captureX = targetX;
                    state->image.captureY = targetY;
                } else {
                    Logger::Error("Screenshot returned nil image");
                }
            }
            auto t_callback_end = Clock::now();
            callbackTotalMs = ElapsedMs(t_callback_start, t_callback_end);
            dispatch_semaphore_signal(semaphore);
        });
        auto t_request_submitted = Clock::now();
        if (!submitted) {
            SetLastError("SCScreenshotManager not available");
            Logger::Debug("[CaptureMacOS] CaptureSync display={} timing: "
                          "GetCachedShareableContent={:.1f}ms, GetDisplays={:.1f}ms, MatchDisplay={:.1f}ms, "
                          "CreateFilter={:.1f}ms, Configure={:.1f}ms, SubmitRequest={:.1f}ms, total={:.1f}ms "
                          "(SCScreenshotManager unavailable)",
                          options.displayId,
                          ElapsedMs(t0, t_content),
                          ElapsedMs(t_content, t_displays),
                          ElapsedMs(t_displays, t_match),
                          ElapsedMs(t_match, t_filter),
                          ElapsedMs(t_filter, t_config),
                          ElapsedMs(t_request_start, t_request_submitted),
                          ElapsedMs(t0, t_request_submitted));
            return std::move(result);
        }

        // 等待截图完成（最多 10 秒）
        bool timedOut = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0;
        auto t_capture_done = Clock::now();

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (timedOut) {
                // 标记放弃：晚到的回调看到后只丢弃结果
                state->abandoned = true;
                SetLastError("Screenshot timed out after 10s");
                Logger::Error("[CaptureMacOS] CaptureSync display={} timed out after 10s",
                              options.displayId);
            }
            // 取走结果（超时时通常为空；若回调恰好在超时判定后、加锁前完成，仍能安全拿到）
            result = std::move(state->image);
            state->image = CaptureImage{};
        }

        if (!timedOut) {
            Logger::Debug("Capture completed: {}", result.data ? "success" : "failed");
            Logger::Debug("[CaptureMacOS] CaptureSync display={} timing: "
                          "GetCachedShareableContent={:.1f}ms, GetDisplays={:.1f}ms, MatchDisplay={:.1f}ms, "
                          "CreateFilter={:.1f}ms, Configure={:.1f}ms, SubmitRequest={:.1f}ms, "
                          "CallbackDelay={:.1f}ms, ConvertCGImageToRGBA={:.1f}ms, CallbackTotal={:.1f}ms, "
                          "WaitIncludingCallback={:.1f}ms",
                          options.displayId,
                          ElapsedMs(t0, t_content),
                          ElapsedMs(t_content, t_displays),
                          ElapsedMs(t_displays, t_match),
                          ElapsedMs(t_match, t_filter),
                          ElapsedMs(t_filter, t_config),
                          ElapsedMs(t_request_start, t_request_submitted),
                          callbackDelayMs,
                          convertMs,
                          callbackTotalMs,
                          ElapsedMs(t_request_start, t_capture_done));
        }
    }
#endif

    // 设置 scaleFactor：从 displayId 查找对应的显示器缩放比
    if (result.data && options.displayId >= 0) {
        auto t_scale_start = Clock::now();
        if (options.hasDisplayInfo) {
            result.scaleFactor = options.displayInfo.scaleFactor;
        } else {
            auto displays = GetDisplays();
            if (options.displayId < (int)displays.size()) {
                result.scaleFactor = displays[options.displayId].scaleFactor;
            }
        }
        auto t_scale_end = Clock::now();
        Logger::Debug("[CaptureMacOS] CaptureSync display={} scaleFactor lookup: {}={:.1f}ms",
                      options.displayId,
                      options.hasDisplayInfo ? "ProvidedDisplayInfo" : "GetDisplays",
                      ElapsedMs(t_scale_start, t_scale_end));
    }

    auto t_end = Clock::now();
    Logger::Debug("[CaptureMacOS] CaptureSync display={} total={:.1f}ms",
                  options.displayId,
                  ElapsedMs(t0, t_end));
    return std::move(result);
    }
}

void CaptureMacOS::CaptureAsync(const CaptureOptions& options, CaptureCallback callback) {
    auto result = CaptureSync(options);
    callback(std::move(result));
}

CaptureImage CaptureMacOS::ConvertCVPixelBufferToRGBA(void* pixelBufferPtr, int displayId) {
    CaptureImage result;
    return std::move(result);  // 未使用
}

CaptureImage CaptureMacOS::ConvertCGImageToRGBA(void* cgImage, int displayId) {
    @autoreleasepool {
    CGImageRef image = (CGImageRef)cgImage;
    CaptureImage result;
    auto t0 = Clock::now();

    size_t width = CGImageGetWidth(image);
    size_t height = CGImageGetHeight(image);
    
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    size_t bytesPerRow = width * 4;
    uint8_t* data = new uint8_t[bytesPerRow * height];
    auto t_alloc = Clock::now();
    
    // 使用简单的 RGBA 格式
    CGBitmapInfo bitmapInfo = kCGImageAlphaPremultipliedLast;
    CGContextRef context = CGBitmapContextCreate(
        data, width, height, 8, bytesPerRow, colorSpace, bitmapInfo
    );
    
    CGColorSpaceRelease(colorSpace);
    
    if (!context) {
        delete[] data;
        SetLastError("Failed to create bitmap context");
        Logger::Debug("[CaptureMacOS] ConvertCGImageToRGBA display={} timing: Alloc={:.1f}ms, CreateBitmapContext failed, total={:.1f}ms",
                      displayId,
                      ElapsedMs(t0, t_alloc),
                      ElapsedMs(t0, Clock::now()));
        return std::move(result);
    }
    auto t_context = Clock::now();
    
    CGRect rect = CGRectMake(0, 0, width, height);
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, rect, image);
    CGContextRelease(context);
    auto t_draw = Clock::now();

    // 屏幕截图所有像素 alpha=255（全透明），反预乘运算 r*255/255=r 不改变值，
    // 因此跳过反预乘循环。省去 14.7M 像素的无用遍历，节省 ~20-40ms。

    result.data = data;
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.rowPitch = static_cast<int>(bytesPerRow);
    result.displayId = displayId;

    Logger::Debug("[CaptureMacOS] ConvertCGImageToRGBA display={} timing: size={}x{}, bytes={:.1f}MB, "
                  "Alloc={:.1f}ms, CreateBitmapContext={:.1f}ms, DrawImage={:.1f}ms, total={:.1f}ms",
                  displayId,
                  result.width,
                  result.height,
                  static_cast<double>(bytesPerRow * height) / (1024.0 * 1024.0),
                  ElapsedMs(t0, t_alloc),
                  ElapsedMs(t_alloc, t_context),
                  ElapsedMs(t_context, t_draw),
                  ElapsedMs(t0, t_draw));
    
    return std::move(result);
    }
}

CGDirectDisplayID CaptureMacOS::GetCGDisplayID(int id) {
    uint32_t displayCount = 0;
    CGError err = CGGetActiveDisplayList(0, nullptr, &displayCount);
    
    if (err != kCGErrorSuccess || displayCount == 0) {
        return kCGNullDirectDisplay;
    }
    
    std::vector<CGDirectDisplayID> displayIDs(displayCount);
    err = CGGetActiveDisplayList(displayCount, displayIDs.data(), &displayCount);
    
    if (err != kCGErrorSuccess) {
        return kCGNullDirectDisplay;
    }

    if (id >= 0) {
        const auto requestedDisplayID = static_cast<CGDirectDisplayID>(id);
        for (CGDirectDisplayID displayID : displayIDs) {
            if (displayID == requestedDisplayID) {
                return displayID;
            }
        }
    }

    // 兼容旧调用方：如果传入的是显示器列表下标，仍然允许解析。
    if (id < 0 || id >= static_cast<int>(displayCount)) {
        return kCGNullDirectDisplay;
    }
    
    return displayIDs[id];
}

std::string CaptureMacOS::GetLastErrorMessage() const {
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

} // namespace shotlet

#endif // SHOTLET_PLATFORM_MACOS
