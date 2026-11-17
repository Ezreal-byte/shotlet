// MacScrollDriver：CGEventTap 拦截用户滚轮 + 合成 pixel 滚轮注入（设计 10.2，spike B）。
// tap 范式来自 platform/HotkeyService_mac.mm；线程模型为专用 run loop 线程
//（HotkeyService 是主线程 Poll，此处调用方会阻塞在 WaitForStableFrame，不能共用）。

#include "scrollcapture/MacScrollDriver.h"
#include "scrollcapture/ScrollPointerGuard.h"
#include "scrollcapture/ScrollUnits.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#import <CoreGraphics/CoreGraphics.h>
#include "scrollcapture/MacScrollEvent.h"
#import <Foundation/Foundation.h>
#include <Carbon/Carbon.h>  // AXIsProcessTrusted（HotkeyService 同款）
#include <unistd.h>

#include <chrono>
#include <thread>

namespace shotlet {

namespace {

// tap trampoline：真实 CG 签名 → 成员函数（O(1) 决策在 HandleTapEvent 内）
CGEventRef ScrollDriverTapTrampoline(CGEventTapProxy proxy, CGEventType type,
                                     CGEventRef event, void* refcon) {
    if (refcon) {
        MacScrollDriver* driver = static_cast<MacScrollDriver*>(refcon);
        return driver->HandleTapEvent(static_cast<uint32_t>(type), event) ? nullptr : event;
    }
    return event;
}

}  // namespace

bool MacScrollDriver::CheckPermission(bool prompt) {
    if (AXIsProcessTrusted()) {
        return true;
    }
    if (prompt) {
        // 弹系统引导（HotkeyService 同款）
        NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt: @YES};
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    }
    return false;
}

MacScrollDriver::~MacScrollDriver() {
    Stop();  // 所有退出路径必须卸载 tap（设计 15"遗留 tap"防御）
}

bool MacScrollDriver::Start(const GlobalRect& selectionRect, const IntentCallback& onIntent) {
    if (m_tapActive.load()) {
        Stop();
    }
    if (selectionRect.w <= 0 || selectionRect.h <= 0) {
        m_lastError = "invalid selection rect";
        return false;
    }
    if (!CheckPermission(false)) {
        m_lastError = "accessibility permission not granted";
        return false;
    }

    // 合成注入用事件源（进程内一份，CGEventSourceCreate 约束）。
    // Start 后只读；独立注入线程退出后才释放。
    if (!m_eventSource) {
        m_eventSource = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    }
    if (!m_eventSource) {
        m_lastError = "CGEventSourceCreate failed";
        return false;
    }

    m_rectX.store(selectionRect.x);
    m_rectY.store(selectionRect.y);
    m_rectW.store(selectionRect.w);
    m_rectH.store(selectionRect.h);
    m_injectPointSet.store(false);
    m_cancelRequested.store(false);
    m_disabledCount.store(0);
    m_swallowedButtons = 0;
    m_target = {};
    m_scaleFactor = (m_config.displayScaleFactor > 0.0) ? m_config.displayScaleFactor : 1.0;
    m_onIntent = onIntent;
    m_stopRequested.store(false);

    m_loopThread = std::thread([this]() { RunLoopThreadMain(); });

    // 等待线程内 tap 创建结果（有界 1s：失败不悬挂）
    for (int i = 0; i < 100 && !m_tapActive.load() && !m_stopRequested.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!m_tapActive.load()) {
        Stop();
        if (m_lastError.empty()) {
            m_lastError = "event tap creation failed (accessibility permission?)";
        }
        return false;
    }
    return true;
}

void MacScrollDriver::RunLoopThreadMain() {
    @autoreleasepool {
    CGEventMask eventMask = CGEventMaskBit(kCGEventScrollWheel);
    if (m_config.protectSelection) {
        eventMask |= CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
            CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDragged) |
            CGEventMaskBit(kCGEventOtherMouseDragged) | CGEventMaskBit(kCGEventKeyDown);
    }
    // 在 session 层拦截用户滚轮。垂直滚轮位于 Axis1；Axis2 为水平轴，
    // 不能根据 Axis2 为零判断 HID 事件缺失滚轮量。
    CFMachPortRef tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                         kCGEventTapOptionDefault, eventMask,
                                         ScrollDriverTapTrampoline, this);
    if (!tap) {
        m_lastError = "CGEventTapCreate failed (check accessibility permission)";
        m_stopRequested.store(true);
        return;
    }

    CFRunLoopRef runLoop = CFRunLoopGetCurrent();
    { std::lock_guard lock(m_runLoopMutex); m_runLoop = runLoop; }
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    CFRunLoopAddSource(runLoop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);

    m_tap = tap;
    m_runLoopSource = source;
    m_tapActive.store(true);
    Logger::Info("[MacScrollDriver] tap installed, rect=({},{},{},{})pt",
                 m_rectX.load(), m_rectY.load(), m_rectW.load(), m_rectH.load());

    while (!m_stopRequested.load()) {
        // 停止请求会主动唤醒，100 ms 超时仅作兜底。
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, /*returnAfterSourceHandled=*/false);
    }

    { std::lock_guard lock(m_runLoopMutex); m_runLoop = nullptr; }

    // 卸载：先摘 tap 再退出 run loop（所有路径必须干净，防遗留 tap）
    CGEventTapEnable(tap, false);
    CFRunLoopRemoveSource(runLoop, source, kCFRunLoopCommonModes);
    CFRelease(source);
    CFRelease(tap);
    m_tap = nullptr;
    m_runLoopSource = nullptr;
    m_tapActive.store(false);
    Logger::Info("[MacScrollDriver] tap removed");
    }
}

void MacScrollDriver::RequestStop() {
    m_stopRequested.store(true);
    std::lock_guard lock(m_runLoopMutex);
    if (m_runLoop) {
        auto loop = static_cast<CFRunLoopRef>(m_runLoop);
        CFRunLoopStop(loop);
        CFRunLoopWakeUp(loop);
    }
}

void MacScrollDriver::Stop() {
    RequestStop();
    if (m_loopThread.joinable()) m_loopThread.join();
    m_tapActive.store(false);
    if (m_eventSource) {
        CFRelease(m_eventSource);
        m_eventSource = nullptr;
    }
    m_onIntent = nullptr;
}

void MacScrollDriver::SetInjectPoint(int x, int y) {
    m_injectX.store(x);
    m_injectY.store(y);
    m_injectPointSet.store(true);
}

bool MacScrollDriver::HandleTapEvent(uint32_t cgEventType, void* cgEvent) {
    if (cgEventType == kCGEventTapDisabledByTimeout ||
        cgEventType == kCGEventTapDisabledByUserInput) {
        // 回调超时被系统禁用：立即重启（设计 10.2，HotkeyService 同款）
        ++m_disabledCount;
        if (m_tap) {
            CGEventTapEnable(static_cast<CFMachPortRef>(m_tap), true);
        }
        return false;  // 该通知事件本身放行
    }
    CGEventRef event = static_cast<CGEventRef>(cgEvent);
    if (!event) return false;
    if (m_config.protectSelection && cgEventType == kCGEventKeyDown &&
        CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) == 53) {
        m_cancelRequested.store(true);
        return true;
    }

    // 自吞防护：合成事件源自本进程 pid → 直接放行（设计 10.2 私有标记的 pid 实现）
    // TODO(spike): pid 字段经 HID tap 后是否保留；不可靠时改投递 kCGSessionEventTap
    const int64_t sourcePid = CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
    if (sourcePid == static_cast<int64_t>(getpid())) {
        return false;
    }

    // 位置过滤（CGEventGetLocation = 全局左上原点点坐标，与选区同坐标系）
    const CGPoint location = CGEventGetLocation(event);
    const int64_t x = static_cast<int64_t>(location.x);
    const int64_t y = static_cast<int64_t>(location.y);
    const GlobalRect rect{m_rectX.load(), m_rectY.load(), m_rectW.load(), m_rectH.load()};
    const bool inside = Contains(rect, int(x), int(y));
    if (m_config.protectSelection && cgEventType != kCGEventScrollWheel) {
        const bool down = cgEventType == kCGEventLeftMouseDown || cgEventType == kCGEventRightMouseDown || cgEventType == kCGEventOtherMouseDown;
        const bool up = cgEventType == kCGEventLeftMouseUp || cgEventType == kCGEventRightMouseUp || cgEventType == kCGEventOtherMouseUp;
        const bool drag = cgEventType == kCGEventLeftMouseDragged || cgEventType == kCGEventRightMouseDragged || cgEventType == kCGEventOtherMouseDragged;
        if (!down && !up && !drag) return false;
        unsigned button = unsigned(CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber));
        return SuppressScrollPointer(rect, m_config.controlRect, int(x), int(y), button,
            down ? ScrollPointerAction::Down : up ? ScrollPointerAction::Up : ScrollPointerAction::Drag,
            m_swallowedButtons);
    }
    if (cgEventType != kCGEventScrollWheel || !inside) return false;

    // 垂直为 Axis1，与 Inject 的 wheel1 参数一致。水平输入也吞掉，避免页面横移。
    const int64_t intent = wheel::VerticalIntent(event, m_scaleFactor);
    if (intent == 0) {
        return true;
    }

    // 只在首个有效垂直滚轮上锁定目标位置，回调内不做日志或文件 I/O。
    if (!m_injectPointSet.load()) {
        m_injectX.store(static_cast<int>(x));
        m_injectY.store(static_cast<int>(y));
        m_injectPointSet.store(true);
    }

    if (m_onIntent) {
        m_onIntent(intent);  // 调用方实现必须 O(1)（atomic 累加）
    }
    return true;  // 吞掉
}

bool MacScrollDriver::ValidateTarget(std::string& error) {
    if (m_disabledCount.load() >= 3 || !m_tapActive.load()) {
        m_targetErrorId = TextId::ScrollHookLost;
        error = "滚轮拦截失效，请结束采集后重试";
        return false;
    }
    if (!m_injectPointSet.load()) return true;
    @autoreleasepool {
        NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
        std::vector<ScrollTargetWindow> windows;
        for (NSDictionary* entry in list) {
            int pid = [entry[(id)kCGWindowOwnerPID] intValue];
            if (pid == getpid() || [entry[(id)kCGWindowAlpha] doubleValue] <= 0) continue;
            CGRect bounds;
            if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)entry[(id)kCGWindowBounds], &bounds)) continue;
            windows.push_back({[entry[(id)kCGWindowNumber] unsignedIntValue], pid,
                {int(bounds.origin.x), int(bounds.origin.y), int(bounds.size.width), int(bounds.size.height)}});
        }
        if (!ValidateScrollTarget(windows, m_target,
            {m_rectX.load(), m_rectY.load(), m_rectW.load(), m_rectH.load()}, m_injectX.load(), m_injectY.load())) {
            m_targetErrorId = TextId::ScrollTargetChanged;
            error = "目标窗口已移动、关闭或被遮挡";
            return false;
        }
        return true;
    }
}

void MacScrollDriver::Inject(int64_t pixelDelta, Phase phase) {
    if ((pixelDelta == 0 && phase != Phase::Ended) || !m_eventSource ||
        (m_stopRequested.load() && phase != Phase::Ended)) {
        return;
    }
    CGEventSourceRef source = static_cast<CGEventSourceRef>(m_eventSource);
    const int32_t wheelDelta = wheel::PixelIntentToWheelDelta(pixelDelta, m_config.displayScaleFactor);
    // pixel 单位合成滚轮（设计 10.2：kCGScrollEventUnitPixel）
    // TODO(spike): 各应用对 pixel 合成事件的接受度；IsContinuous 对平滑滚动语义的影响
    CGEventRef event =
        CGEventCreateScrollWheelEvent2(source, kCGScrollEventUnitPixel, 1, wheelDelta, 0, 0);
    if (!event) {
        Logger::Warning("[MacScrollDriver] CGEventCreateScrollWheelEvent2 failed");
        return;
    }
    CGEventSetIntegerValueField(event, kCGScrollWheelEventIsContinuous, 1);
    if (phase != Phase::None) {
        const int value = phase == Phase::Began ? kCGScrollPhaseBegan
            : phase == Phase::Changed ? kCGScrollPhaseChanged : kCGScrollPhaseEnded;
        CGEventSetIntegerValueField(event, kCGScrollWheelEventScrollPhase, value);
    }
    // 注入位置命中（spike B 核心验证项）：仅设事件位置，WindowServer 按坐标命中目标
    CGEventSetLocation(event, CGPointMake(m_injectX.load(), m_injectY.load()));
    // TODO(spike-diagnostic): 闭环实测诊断日志，验证通过后移除
    Logger::Debug("[MacScrollDriver] inject {}px -> wheel {} @({},{})", pixelDelta, wheelDelta,
                 m_injectX.load(), m_injectY.load());
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

}  // namespace shotlet

#endif  // SHOTLET_PLATFORM_MACOS
