// scroll_demo：滚动截图 M2 闭环验证（设计 17.2 M2"注入—稳定—匹配"命令行 demo）。
// 同时承担 spike A（排除自身窗口后抓实时内容）/ spike B（合成事件位置命中）实测。
//
// 用法：
//   scroll_demo --rect x,y,w,h --output out.png [--inject x,y] [--countdown N]
//   --rect 单位为点（全局显示坐标，主显示器）；CLI 权限归属父终端（需给 Terminal/iTerm 授权）
//
// 流程：权限检查 → 启动采集/首帧 → 启动 tap → 倒计时 →
// 单 in-flight 循环（意图累积 → TakeStep → Inject → WaitForStableFrame → ProcessFrame →
// CompleteStep）→ Ctrl-C 结束 → 导出 PNG + 统计。

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <optional>
#include <thread>
#include <vector>

#include "scrollcapture/MacFrameSource.h"
#include "scrollcapture/MacScrollDriver.h"
#include "scrollcapture/ScrollCaptureStitcher.h"
#include "scrollcapture/ScrollPlanner.h"
#include "utils/Colors.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int64_t> g_pending{0};  // tap 线程 → 主线程 的意图桥（O(1) 累加）
std::atomic<bool> g_stop{false};

void OnSigint(int) {
    g_stop.store(true);  // signal-safe：只写 atomic
}

struct Args {
    shotlet::GlobalRect rect{200, 150, 800, 600};  // 点单位，默认值覆盖常见浏览器窗口
    std::string output = "scroll_demo_out.png";
    int injectX = -1, injectY = -1;  // <0 = 用首次滚轮位置
    int countdownSec = 3;
};

bool ParseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rect" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d,%d,%d,%d", &args.rect.x, &args.rect.y, &args.rect.w,
                            &args.rect.h) != 4) {
                std::printf("错误: --rect 格式应为 x,y,w,h（点单位）\n");
                return false;
            }
        } else if (a == "--output" && i + 1 < argc) {
            args.output = argv[++i];
        } else if (a == "--inject" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d,%d", &args.injectX, &args.injectY) != 2) {
                std::printf("错误: --inject 格式应为 x,y（点单位）\n");
                return false;
            }
        } else if (a == "--countdown" && i + 1 < argc) {
            args.countdownSec = std::atoi(argv[++i]);
        } else {
            std::printf("用法: scroll_demo --rect x,y,w,h --output out.png "
                        "[--inject x,y] [--countdown N]\n");
            return false;
        }
    }
    return args.rect.w > 0 && args.rect.h > 0;
}

// 主显示器（选区所在）ID 与缩放系数
uint32_t MainDisplayId() {
    return CGMainDisplayID();
}

double DisplayScaleFactor(uint32_t displayId) {
    const CGRect bounds = CGDisplayBounds(displayId);
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
    if (!mode) {
        return 1.0;
    }
    const double scale = CGDisplayModeGetPixelWidth(mode) / bounds.size.width;
    CGDisplayModeRelease(mode);
    return scale;
}

// PNG 导出（FileService_mac.mm 同款 CGImageDestination 路径）
bool WritePng(const std::string& path, const std::vector<uint8_t>& rgba, int width, int height) {
    @autoreleasepool {
    CFMutableDataRef pngData = CFDataCreateMutable(nullptr, 0);
    if (!pngData) {
        return false;
    }
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData(pngData, CFSTR("public.png"), 1, nullptr);
    if (!destination) {
        CFRelease(pngData);
        return false;
    }

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    const CGBitmapInfo bitmapInfo =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
    CGContextRef context =
        CGBitmapContextCreate(const_cast<uint8_t*>(rgba.data()), width, height, 8, width * 4,
                              colorSpace, bitmapInfo);
    CGColorSpaceRelease(colorSpace);
    if (!context) {
        CFRelease(destination);
        CFRelease(pngData);
        return false;
    }
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (!image) {
        CFRelease(destination);
        CFRelease(pngData);
        return false;
    }

    CGImageDestinationAddImage(destination, image, nullptr);
    CGImageRelease(image);
    const bool ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    if (!ok) {
        CFRelease(pngData);
        return false;
    }

    NSData* nsData = (__bridge_transfer NSData*)pngData;
    return [nsData writeToFile:[NSString stringWithUTF8String:path.c_str()]
                    atomically:YES];
    }
}

const char* StatusName(shotlet::StitchStatus s) {
    switch (s) {
        case shotlet::StitchStatus::Ok: return "Ok";
        case shotlet::StitchStatus::NoChange: return "NoChange";
        case shotlet::StitchStatus::Ambiguous: return "Ambiguous";
        case shotlet::StitchStatus::InsufficientInfo: return "InsufficientInfo";
        case shotlet::StitchStatus::MatchFailed: return "MatchFailed";
        case shotlet::StitchStatus::LimitReached: return "LimitReached";
    }
    return "?";
}

}  // namespace

// 选区浮层（demo 专用，M3 才有正式选区 UI）：红色边框标记选区，鼠标事件穿透
// （滚动直达下层窗口）。属于本 pid 窗口，会被 MacFrameSource 排除，
// 不污染采集帧——即 spike A"排除自身窗口后抓到实时内容"的实测样本。
// ObjC 声明必须在全局作用域（不能放 C++ namespace 内）
@interface ScrollDemoOverlayView : NSView
@end

@implementation ScrollDemoOverlayView
- (void)drawRect:(NSRect)dirtyRect {
    NSBezierPath* const path = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 1.5, 1.5)];
    const auto fill = shotlet::Colors::ScrollDemoFillVec4;
    [[NSColor colorWithCalibratedRed:fill.x green:fill.y blue:fill.z alpha:fill.w] setFill];
    [path fill];
    const auto border = shotlet::Colors::ScrollDemoBorderVec4;
    [[NSColor colorWithCalibratedRed:border.x green:border.y blue:border.z alpha:border.w] setStroke];
    path.lineWidth = 3.0;
    [path stroke];
}
@end

namespace {

NSWindow* g_overlayWindow = nil;

// CG 全局坐标（左上原点，主显示器）→ AppKit 坐标（左下原点）
void ShowOverlay(const shotlet::GlobalRect& rect) {
    @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];  // 不出 Dock 图标

    const NSRect screenFrame = [[NSScreen mainScreen] frame];
    const NSRect frame = NSMakeRect(screenFrame.origin.x + rect.x,
                                    screenFrame.origin.y + screenFrame.size.height - rect.y -
                                        rect.h,
                                    rect.w, rect.h);
    NSWindow* const window =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskBorderless
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.level = NSStatusWindowLevel;
    window.opaque = NO;
    window.backgroundColor = [NSColor clearColor];
    window.hasShadow = NO;
    window.ignoresMouseEvents = YES;  // 滚轮穿透到下层窗口
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces;
    window.contentView = [[ScrollDemoOverlayView alloc] initWithFrame:frame];
    [window orderFrontRegardless];
    g_overlayWindow = window;
    // 短暂跑 run loop 让窗口完成首次绘制（demo 主体不进 NSApp 主循环）
    [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                          beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
    }
}

// 没有 AppKit 事件时 runMode 会立即返回，短暂休眠避免空闲/合并输入时忙轮询。
void PumpEvents() {
    @autoreleasepool {
        const BOOL handled = [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.002]];
        if (!handled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void CloseOverlay() {
    if (g_overlayWindow) {
        [g_overlayWindow orderOut:nil];
        [g_overlayWindow close];
        g_overlayWindow = nil;
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        return 1;
    }

    // 1. 权限：辅助功能（tap 拦截与注入）
    if (!shotlet::MacScrollDriver::CheckPermission(/*prompt=*/true)) {
        std::printf("缺少辅助功能权限。请到 系统设置 > 隐私与安全性 > 辅助功能\n"
                    "给父终端（Terminal/iTerm）授权后重试。\n");
        return 1;
    }

    const uint32_t displayId = MainDisplayId();
    const double scale = DisplayScaleFactor(displayId);

    // 选区浮层先于采集启动创建：属于本 pid，会被 SCContentFilter 排除，不进帧
    ShowOverlay(args.rect);
    std::printf("选区已用红框标记: (%d,%d) %dx%d pt\n", args.rect.x, args.rect.y, args.rect.w,
                args.rect.h);

    // 2. 采集启动（屏幕录制权限失效会在 Start 失败信息中体现）
    shotlet::MacFrameSource source;
    if (!source.Start(displayId, args.rect, /*excludeOwnWindows=*/true)) {
        std::printf("屏幕采集启动失败: %s\n"
                    "请检查屏幕录制权限（系统设置 > 隐私与安全性 > 屏幕录制，给父终端授权）。\n",
                    source.GetLastErrorMessage().c_str());
        return 1;
    }

    shotlet::CapturedFrame first;
    if (!source.WaitForNextFrame(0, first, 3000.0)) {
        CloseOverlay();
        std::printf("3 秒内未收到首帧: %s\n", source.GetLastErrorMessage().c_str());
        return 1;
    }
    std::printf("首帧 %dx%d px（选区 %dx%d pt，缩放 %.1fx）\n", first.width, first.height,
                args.rect.w, args.rect.h, scale);

    // 3. 拼接器与规划器（M1 模块）
    shotlet::ScrollCaptureStitcher stitcher;
    {
        const shotlet::PixelView firstView{first.pixels.data(), first.width, first.height,
                                         first.width * 4};
        if (!stitcher.StartSession(firstView)) {
            CloseOverlay();
            std::printf("StartSession 失败: %s\n", stitcher.GetLastErrorMessage().c_str());
            return 1;
        }
    }
    shotlet::PlannerConfig plannerConfig;
    // 合成滚轮单位与屏幕物理像素可能相差显示缩放倍数，首步使用保守先验。
    plannerConfig.initialGain = scale;
    shotlet::ScrollPlanner planner(plannerConfig);
    planner.SetViewportHeight(first.height);

    // 4. tap 启动 + 信号处理
    shotlet::MacScrollDriver driver;
    driver.SetConfig({scale});
    if (!driver.Start(args.rect, [](int64_t d) { g_pending.fetch_add(d); })) {
        CloseOverlay();
        std::printf("事件 tap 启动失败: %s\n", driver.GetLastErrorMessage().c_str());
        return 1;
    }
    if (args.injectX >= 0) {
        driver.SetInjectPoint(args.injectX, args.injectY);
    }
    std::signal(SIGINT, OnSigint);

    // 5. 倒计时（用户把鼠标放到选区内准备滚动）；用 run loop 替代纯 sleep，保持浮层绘制
    std::printf("在 %d 秒内把鼠标移到红框内并滚动（Ctrl-C 结束并导出）\n", args.countdownSec);
    for (int i = args.countdownSec; i > 0; --i) {
        std::printf("  %d...\n", i);
        std::fflush(stdout);
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                              beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        if (g_stop.load()) {
            break;
        }
    }

    // 6. 主循环：单 in-flight"注入 → 稳定 → 匹配"（设计 8.1）
    int steps = 0;
    int64_t totalDy = 0;
    double totalConf = 0.0;
    int unstableFrames = 0;
    double totalSettleMs = 0.0;
    bool captureFailed = false;
    std::optional<Clock::time_point> inputReadyAt;
    const auto tStart = Clock::now();

    while (!g_stop.load()) {
        if (captureFailed) {
            // 失败后仍吞掉选区滚轮，保留已有长图，等待用户结束导出。
            g_pending.exchange(0);
            PumpEvents();
            continue;
        }
        // 每一步都合并新输入，使反向滚动能立即抵消旧方向积压。
        const bool hadPending = planner.HasStep();
        planner.AddIntent(g_pending.exchange(0));
        if (!planner.HasStep()) {
            inputReadyAt.reset();
            PumpEvents();
            continue;
        }
        if (!hadPending && std::abs(planner.Pending()) <= 8) {
            // 平滑滚轮常以 ±2px 开始，约 20–30ms 后才送来主体意图。
            // 仅空闲后的微小起始事件合并 32ms；有积压时立即推进，不逐步加延迟。
            inputReadyAt = Clock::now() + std::chrono::milliseconds(32);
        }
        if (inputReadyAt && Clock::now() < *inputReadyAt) {
            PumpEvents();
            continue;
        }
        inputReadyAt.reset();

        const auto step = planner.TakeStep();
        const auto tInject = Clock::now();
        const auto frameBaseline = source.TakeBaseline();
        driver.Inject(step.injectPixels);

        shotlet::CapturedFrame frame;
        if (!source.WaitForStableFrame(frameBaseline, frame)) {
            std::printf("step %d: 未收到有效帧（%s），已停止注入，Ctrl-C 导出已有结果\n",
                        steps + 1, source.GetLastErrorMessage().c_str());
            planner.Reset();
            captureFailed = true;
            continue;
        }
        const auto tSettled = Clock::now();
        shotlet::FrameInput input;
        input.pixels = {frame.pixels.data(), frame.width, frame.height, frame.width * 4};
        input.expectedShift = static_cast<int>(step.expectedShift);
        input.hasPrior = true;
        input.stable = frame.stable;
        bool retried = false;
        shotlet::StitchResult result = stitcher.ProcessFrame(input);
        const auto needsRetry = [](shotlet::StitchStatus status) {
            return status == shotlet::StitchStatus::MatchFailed ||
                   status == shotlet::StitchStatus::Ambiguous ||
                   status == shotlet::StitchStatus::InsufficientInfo;
        };
        if (needsRetry(result.status) && !g_stop.load()) {
            // 不再继续前滚：先延长等待，尝试用同一步的最终画面重新匹配。
            retried = true;
            shotlet::CapturedFrame retry;
            if (source.WaitForStableFrame(frameBaseline, retry)) {
                frame = std::move(retry);
                input.pixels = {frame.pixels.data(), frame.width, frame.height, frame.width * 4};
                input.stable = frame.stable;
                result = stitcher.ProcessFrame(input);
            }
        }
        const auto tMatched = Clock::now();
        if (!frame.stable) {
            ++unstableFrames;
        }

        const double injectMs = std::chrono::duration<double, std::milli>(tSettled - tInject).count();
        const double matchMs =
            std::chrono::duration<double, std::milli>(tMatched - tSettled).count();
        totalSettleMs += injectMs;
        ++steps;
        if (result.status == shotlet::StitchStatus::Ok || result.status == shotlet::StitchStatus::NoChange) {
            totalDy += result.dy;
            totalConf += result.confidence;
            planner.CompleteStep(result.dy, step.injectPixels, /*matched=*/true,
                                 frame.stable && !retried && !result.usedFallbackSearch);
        } else {
            planner.CompleteStep(result.dy, step.injectPixels, /*matched=*/false);
        }
        if (result.status == shotlet::StitchStatus::NoChange) {
            planner.Reset();  // 已到边界时丢弃旧方向积压，新的反向输入仍可继续。
        } else if (result.status != shotlet::StitchStatus::Ok) {
            planner.Reset();
            captureFailed = true;
            std::printf("无法继续拼接（%s: %s），已停止注入，Ctrl-C 导出已有结果\n",
                        StatusName(result.status), stitcher.GetLastErrorMessage().c_str());
        }

        std::printf(
            "step %-3d inject=%+6lld prior=%+5lld dy=%+5d conf=%.2f %s%s%s top+%-4d bot+%-4d "
            "settle=%5.0fms match=%4.0fms\n",
            steps, static_cast<long long>(step.injectPixels),
            static_cast<long long>(step.expectedShift), result.dy, result.confidence,
            StatusName(result.status), frame.stable ? "" : " [unstable]",
            result.usedFallbackSearch ? " [full-search]" : "",
            result.appendedTopRows, result.appendedBottomRows, injectMs, matchMs);
        std::fflush(stdout);
    }

    // 7. 收尾：卸载 tap/采集/浮层 → 导出 → 统计
    driver.Stop();
    source.Stop();
    CloseOverlay();

    std::vector<uint8_t> outRgba;
    int outW = 0, outH = 0;
    if (!stitcher.ExportFinalImage(outRgba, outW, outH)) {
        std::printf("导出失败: %s\n", stitcher.GetLastErrorMessage().c_str());
        return 1;
    }
    if (!WritePng(args.output, outRgba, outW, outH)) {
        std::printf("PNG 写入失败: %s\n", args.output.c_str());
        return 1;
    }

    const double totalSec =
        std::chrono::duration<double>(Clock::now() - tStart).count();
    const auto* store = stitcher.GetStore();
    std::printf("\n==== 统计 ====\n步数 %d | 内容带高 %d px | 固定头 %d / 尾 %d px\n"
                "平均 dy %.1f px/步 | 平均置信度 %.2f | 不稳定帧 %d | 平均 settle %.0fms\n"
                "总时长 %.1fs | 输出 %s (%dx%d)\n",
                steps, store ? store->Height() : 0, stitcher.FixedHeaderHeight(),
                stitcher.FixedFooterHeight(),
                steps > 0 ? static_cast<double>(totalDy) / steps : 0.0,
                steps > 0 ? totalConf / steps : 0.0, unstableFrames,
                steps > 0 ? totalSettleMs / steps : 0.0, totalSec, args.output.c_str(), outW, outH);
    return 0;
}

#else  // !SHOTLET_PLATFORM_MACOS

int main() {
    std::printf("scroll_demo 仅支持 macOS\n");
    return 1;
}

#endif  // SHOTLET_PLATFORM_MACOS
