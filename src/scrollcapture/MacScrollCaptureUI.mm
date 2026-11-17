#include "platform/LocalizedText.h"
#include "scrollcapture/MacScrollCaptureUI.h"
#import "scrollcapture/MacScrollMaskView.h"
#include "scrollcapture/MacFrameSource.h"
#include "scrollcapture/MacScrollDriver.h"
#include "scrollcapture/SmoothScrollInput.h"
#include <mach/mach_time.h>
#include "ui/Toolbar.h"
#include "utils/Colors.h"
#include "utils/Logger.h"
#include <SDL3/SDL.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cstring>

static NSColor* ShotletScrollBorderColor() {
    constexpr ImU32 color = shotlet::Colors::SelectionBorder;
    return [NSColor colorWithDeviceRed:CGFloat((color >> IM_COL32_R_SHIFT) & 255) / 255.0
                                green:CGFloat((color >> IM_COL32_G_SHIFT) & 255) / 255.0
                                 blue:CGFloat((color >> IM_COL32_B_SHIFT) & 255) / 255.0
                                alpha:1.0];
}

@implementation ShotletScrollMaskView
- (BOOL)isOpaque { return NO; }
- (void)drawRect:(NSRect)dirtyRect {
    // 清空视图后只绘制选区外遮罩，中心保留真正透明的像素。
    [[NSColor clearColor] setFill];
    NSRectFillUsingOperation(dirtyRect, NSCompositingOperationCopy);
    NSBezierPath* shade = [NSBezierPath bezierPathWithRect:self.bounds];
    [shade appendBezierPathWithRect:self.selectionRect];
    shade.windingRule = NSWindingRuleEvenOdd;
    const auto mask = shotlet::Colors::ScrollMaskVec4;
    [[NSColor colorWithWhite:mask.x alpha:mask.w] setFill];
    [shade fill];
    [ShotletScrollBorderColor() setStroke];
    NSBezierPath* border = [NSBezierPath bezierPathWithRect:self.selectionRect];
    border.lineWidth = 1;
    [border stroke];

    // 提示靠选区左上角；反向警告在顶部空间不足时移入选区，保证可见。
    NSString* hint = shotlet::TrNative(self.language, self.reverseScrollBlocked ? shotlet::TextId::ScrollReverseHint : shotlet::TextId::ScrollHint);
    NSDictionary* attributes = @{
        NSFontAttributeName: [NSFont systemFontOfSize:14],
        NSForegroundColorAttributeName: self.reverseScrollBlocked ? NSColor.systemRedColor : NSColor.whiteColor
    };
    const NSSize textSize = [hint sizeWithAttributes:attributes];
    const CGFloat gap = 8;
    CGFloat y = NSMaxY(self.selectionRect) + gap;
    if (y + textSize.height > NSMaxY(self.bounds)) {
        if (!self.reverseScrollBlocked) return;
        y = NSMaxY(self.selectionRect) - gap - textSize.height;
    }
    const CGFloat x = std::max(NSMinX(self.bounds),
        std::min(NSMinX(self.selectionRect), NSMaxX(self.bounds) - textSize.width));
    [hint drawAtPoint:NSMakePoint(x, std::max(NSMinY(self.bounds), y)) withAttributes:attributes];
}
@end

@interface ShotletScrollActions : NSObject
@property(nonatomic, copy) void (^editAction)(void);
@property(nonatomic, copy) void (^saveAction)(void);
@property(nonatomic, copy) void (^finishAction)(void);
@property(nonatomic, copy) void (^cancelAction)(void);
- (void)edit:(id)sender;
- (void)save:(id)sender;
- (void)finish:(id)sender;
- (void)cancel:(id)sender;
@end
@implementation ShotletScrollActions
- (void)edit:(id)sender { if (self.editAction) self.editAction(); }
- (void)save:(id)sender { if (self.saveAction) self.saveAction(); }
- (void)finish:(id)sender { if (self.finishAction) self.finishAction(); }
- (void)cancel:(id)sender { if (self.cancelAction) self.cancelAction(); }
@end

namespace shotlet {
namespace {
NSRect AppKitRect(GlobalRect r) {
    const double top = CGDisplayBounds(CGMainDisplayID()).size.height;
    return NSMakeRect(r.x, top - r.y - r.h, r.w, r.h);
}
GlobalRect CGRectFromAppKit(NSRect r) {
    const double top = CGDisplayBounds(CGMainDisplayID()).size.height;
    return {int(r.origin.x), int(top - r.origin.y - r.size.height), int(r.size.width), int(r.size.height)};
}
NSImage* Image(const uint8_t* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return nil;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
        pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
        isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:width * 4 bitsPerPixel:32];
    if (!rep) return nil;
    std::memcpy(rep.bitmapData, pixels, size_t(width) * height * 4);
    NSImage* result = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [result addRepresentation:rep];
    return result;
}
NSImage* Image(const ScrollImage& image) {
    return Image(image.View().data, image.width, image.height);
}
NSPanel* Panel(NSRect rect, NSInteger level, bool interactive) {
    NSPanel* p = [[NSPanel alloc] initWithContentRect:rect
        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered defer:NO];
    p.releasedWhenClosed = NO;
    p.level = level;
    p.opaque = NO;
    p.hidesOnDeactivate = NO;
    p.ignoresMouseEvents = !interactive;
    p.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    p.backgroundColor = NSColor.clearColor;
    return p;
}
class Backend final : public ScrollCaptureBackend {
public:
    Backend(uint32_t display, GlobalRect rect, double scale, GlobalRect controls, Language language)
        : m_display(display), m_rect(rect), m_language(language) {
        MacScrollDriver::Config config;
        config.displayScaleFactor = scale;
        config.protectSelection = true;
        config.controlRect = controls;
        m_driver.SetConfig(config);
    }
    ~Backend() override { Stop(); }
    bool Start(ScrollImage& first, std::function<void(int64_t)> cb, std::string& error) override {
        m_callback = std::move(cb);
        if (!m_driver.Start(m_rect, [this](int64_t delta) {
            if (!delta || m_paused.load()) return;
            const int direction = delta > 0 ? 1 : -1;
            int expected = 0;
            m_inputDirection.compare_exchange_strong(expected, direction);
            if (m_inputDirection.load() != direction) {
                m_reversed.store(true);
                return;
            }
            m_reversed.store(false);
            m_notice.store(direction);
            m_inputTime.store(NowMs());
            // tap 无锁且最多四次尝试；距离有界，过载输入不会在松手后长时间回放。
            const int64_t cap = m_maxPending.load();
            auto pending = m_pending.load();
            for (int attempt = 0; attempt < 4; ++attempt) {
                const auto next = std::clamp(pending + std::clamp(delta, -cap, cap), -cap, cap);
                if (m_pending.compare_exchange_weak(pending, next)) {
                    if (std::abs(next - pending) < std::abs(std::clamp(delta, -cap, cap))) ++m_dropped;
                    return;
                }
            }
            ++m_dropped;
        })) {
            error = Tr(m_language, TextId::ScrollInputFailed);
            return false;
        }
        if (!m_source.Start(m_display, m_rect, true, [this] { return m_cancelled.load(); })) {
            error = Tr(m_language, TextId::ScrollCaptureFailed);
            return false;
        }
        CapturedFrame frame;
        if (!m_source.WaitForFrameReference(0, frame, 3000)) {
            error = Tr(m_language, TextId::ScrollFirstFrameFailed);
            return false;
        }
        first = {frame.width, frame.height, {}, std::move(frame.sharedPixels)};
        m_frameId = frame.frameId;
        m_pending.store(0);
        m_notice.store(0);
        m_inputDirection.store(0);
        m_reversed.store(false);
        return true;
    }
    bool ContinuousInput() const override { return true; }
    void ConfigureContinuousInput(int height, int overlap) override {
        SmoothScrollInput input(height, overlap);
        m_maxPending.store(input.Budget());
        m_contentHeight.store(height);
        m_minOverlap = overlap;
        m_inputThread = std::thread([this, input]() mutable { RunInput(input); });
    }
    bool CaptureStep(int64_t, ScrollImage& image, bool& stable, std::string& error) override {
        CapturedFrame frame;
        if (!m_source.WaitForFrameReference(m_frameId, frame, 30)) {
            if (!m_source.GetLastErrorMessage().empty())
                error = Tr(m_language, TextId::ScrollCaptureFailed);
            return false;
        }
        m_frameId = frame.frameId;
        m_capturedTime = frame.displayTime;
        stable = false; // 连续运动帧始终使用更严格的像素验证。
        image = {frame.width, frame.height, {}, std::move(frame.sharedPixels)};
        Logger::Debug("[ScrollCapture] frame={} ageMs={:.2f}", m_frameId,
            ElapsedMs(mach_absolute_time(), m_capturedTime));
        return true;
    }
    bool ValidateTarget(std::string& error) override {
        ForwardPendingInput();
        const bool valid = m_driver.ValidateTarget(error);
        m_targetValid.store(valid);
        if (!valid) error = Tr(m_language, m_driver.GetTargetErrorId());
        return valid;
    }
    void FrameFeedback(const StitchResult& result, bool matched, int contentHeight) override {
        if (contentHeight > 0) m_contentHeight.store(contentHeight);
        const uint64_t injectedAt = m_injectedAt.load();
        // 把位移、匹配结果和静止样本作为同一条反馈发布，避免跨帧混用。
        std::lock_guard lock(m_feedbackMutex);
        if (matched) m_matchedRows += std::abs(result.dy);
        m_quietSample = injectedAt && m_capturedTime > injectedAt &&
                        ElapsedMs(m_capturedTime, injectedAt) >= 400;
        m_matched.store(matched);
        m_feedbackPending = true;
    }
    void PauseInput() override {
        m_paused.store(true);
        m_pending.store(0);
        m_inputWake.notify_all();
    }
    bool FinalFrameSettled() const override {
        const auto paused = m_pauseAcknowledged.load();
        const auto injected = m_injectedAt.load();
        // 使用样本生成时间，防止把确认后迟到的旧帧当成尾帧。
        // 最近仍有注入时至少留出 200 ms 响应窗口；控制器另要求 80 ms 静止。
        return paused && m_capturedTime >= paused &&
            m_frameId == m_source.CurrentFrameId() &&
            (!injected || (m_capturedTime > injected && ElapsedMs(m_capturedTime, injected) >= 200));
    }
    bool CancelRequested() const override { return m_driver.CancelRequested(); }
    void RequestCancel() override {
        PauseInput();
        m_cancelled.store(true);
        m_driver.RequestStop();
        m_source.RequestStop();
    }
    void Stop() override {
        PauseInput();
        m_inputStopped.store(true);
        m_inputWake.notify_all();
        if (m_inputThread.joinable()) m_inputThread.join();
        m_driver.Stop();
        m_source.Stop();
    }
private:
    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static double ElapsedMs(uint64_t newer, uint64_t older) {
        static const mach_timebase_info_data_t base = [] {
            mach_timebase_info_data_t value; mach_timebase_info(&value); return value;
        }();
        return newer > older ? double(newer - older) * base.numer / base.denom / 1e6 : 0;
    }
    void RunInput(SmoothScrollInput input) {
        int64_t lastReport = NowMs(), injected = 0;
        bool gesture = false;
        while (!m_inputStopped.load()) {
            const auto now = NowMs();
            input.SetContentHeight(m_contentHeight.load(), m_minOverlap);
            m_maxPending.store(input.Budget());
            {
                std::lock_guard lock(m_feedbackMutex);
                if (m_feedbackPending) {
                    input.Feedback(m_matchedRows, m_matched.load(), now, m_quietSample);
                    m_matchedRows = 0;
                    m_feedbackPending = false;
                }
            }
            int64_t step = 0;
            if (!m_paused.load() && m_targetValid.load() && m_matched.load()) {
                input.Add(m_pending.exchange(0), m_inputTime.load(), now);
                step = input.Next(now);
            } else {
                m_pending.store(0);
                input.DiscardPending();
            }
            if (step && !m_paused.load() && m_targetValid.load() && m_matched.load()) {
                m_injectedAt.store(mach_absolute_time());
                m_driver.Inject(step, gesture ? MacScrollDriver::Phase::Changed : MacScrollDriver::Phase::Began);
                gesture = true;
                injected += std::abs(step);
            } else if (gesture) {
                m_driver.Inject(0, MacScrollDriver::Phase::Ended);
                gesture = false;
            }
            if (m_paused.load() && !m_pauseAcknowledged.load()) {
                m_pauseAcknowledged.store(mach_absolute_time());
            }
            if (now - lastReport >= 1000) {
                Logger::Debug("[ScrollCapture] input injected={}px pending={} debt={} droppedEvents={}",
                    injected, input.Pending(), input.Debt(), m_dropped.exchange(0));
                lastReport = now; injected = 0;
            }
            std::unique_lock lock(m_inputMutex);
            m_inputWake.wait_for(lock, std::chrono::milliseconds(16), [this] {
                return m_inputStopped.load();
            });
        }
        if (gesture) m_driver.Inject(0, MacScrollDriver::Phase::Ended);
    }
    void ForwardPendingInput() {
        const int64_t reverseNotice = ConsumeReverseNotice(m_reversed, m_inputDirection.load());
        const auto delta = m_notice.exchange(0);
        if (!m_callback) return;
        if (delta) m_callback(delta);
        if (reverseNotice != 0) m_callback(reverseNotice);
    }
    uint32_t m_display;
    GlobalRect m_rect;
    MacScrollDriver m_driver;
    const Language m_language;
    MacFrameSource m_source;
    uint64_t m_frameId = 0, m_capturedTime = 0;
    int m_minOverlap = 0;
    std::thread m_inputThread;
    std::mutex m_inputMutex;
    std::mutex m_feedbackMutex;
    std::condition_variable m_inputWake;
    int64_t m_matchedRows = 0;
    bool m_quietSample = false, m_feedbackPending = false;
    std::atomic<int64_t> m_pending{0}, m_inputTime{0}, m_maxPending{1};
    std::atomic<int> m_notice{0}, m_inputDirection{0}, m_contentHeight{0};
    std::atomic<uint64_t> m_injectedAt{0}, m_dropped{0};
    std::atomic<uint64_t> m_pauseAcknowledged{0};
    std::atomic<bool> m_reversed{false}, m_cancelled{false}, m_paused{false}, m_inputStopped{false};
    std::atomic<bool> m_targetValid{false}, m_matched{true};
    std::function<void(int64_t)> m_callback;
};

class UI final : public ScrollCaptureUI {
public:
    UI(NSWindow* overlay, uint32_t display, GlobalRect selection, double scale, Language language, Theme theme)
        : m_language(language), m_controller(language), m_overlay(overlay) {
        NSRect screen = AppKitRect({int(CGDisplayBounds(display).origin.x), int(CGDisplayBounds(display).origin.y),
            int(CGDisplayBounds(display).size.width), int(CGDisplayBounds(display).size.height)});
        NSRect rect = AppKitRect(selection);
        m_mask = Panel(screen, overlay.level + 1, false);
        m_mask.opaque = NO;
        m_mask.hasShadow = NO;
        m_mask.backgroundColor = NSColor.clearColor;
        ShotletScrollMaskView* maskView = [[ShotletScrollMaskView alloc]
            initWithFrame:NSMakeRect(0, 0, screen.size.width, screen.size.height)];
        maskView.language = language;
        maskView.selectionRect = NSOffsetRect(rect, -screen.origin.x, -screen.origin.y);
        m_mask.contentView = maskView;
        constexpr CGFloat controlW = Toolbar::ScrollCaptureWidth;
        constexpr CGFloat controlH = Toolbar::ScrollCaptureHeight;
        constexpr CGFloat gap = 8;
        CGFloat x = std::clamp<CGFloat>(NSMaxX(rect) - controlW,
                                       NSMinX(screen) + gap, NSMaxX(screen) - controlW - gap);
        CGFloat y = NSMinY(rect) - controlH - gap;
        if (y < NSMinY(screen) + gap) y = NSMaxY(rect) + gap;
        y = std::clamp<CGFloat>(y, NSMinY(screen) + gap, NSMaxY(screen) - controlH - gap);
        m_controls = Panel(NSMakeRect(x, y, controlW, controlH), overlay.level + 4, true);
        m_controls.hasShadow = YES;
        const auto toolbarBitmap = Toolbar::MakeScrollCaptureBitmap(float(m_controls.backingScaleFactor), theme);
        NSImageView* background = [[NSImageView alloc] initWithFrame:m_controls.contentView.bounds];
        m_toolbarBackground = background;
        background.image = Image(toolbarBitmap.rgba.data(), toolbarBitmap.width, toolbarBitmap.height);
        background.image.size = NSMakeSize(controlW, controlH);
        [m_controls.contentView addSubview:background];
        m_actions = [ShotletScrollActions new];
        m_actions.editAction = ^{ m_action = ScrollCaptureAction::Edit; m_controller.Finish(); };
        m_actions.saveAction = ^{ m_action = ScrollCaptureAction::Save; m_controller.Finish(); };
        m_actions.finishAction = ^{ m_action = ScrollCaptureAction::Copy; m_controller.Finish(); };
        m_actions.cancelAction = ^{ m_controller.Cancel(); };
        NSArray<NSString*>* titles = @[TrNative(language, TextId::ScrollEdit), TrNative(language, TextId::ScrollSave), TrNative(language, TextId::ScrollCancel), TrNative(language, TextId::ActionCopyDone)];
        SEL selectors[] = {@selector(edit:), @selector(save:), @selector(cancel:), @selector(finish:)};
        for (NSInteger i = 0; i < 4; ++i) {
            NSButton* button = [NSButton buttonWithTitle:@"" target:m_actions action:selectors[i]];
            button.frame = NSMakeRect(8 + i * Toolbar::ScrollCaptureButtonStep,
                Toolbar::ScrollCaptureButtonInsetY, 44, Toolbar::ScrollCaptureButtonHeight);
            button.bordered = NO;
            button.toolTip = titles[i];
            [m_controls.contentView addSubview:button];
        }
        // 右侧能容纳标准预览宽度时始终放右侧，否则靠选框左侧显示。
        const double right = NSMaxX(screen) - NSMaxX(rect) - 16;
        const double left = NSMinX(rect) - NSMinX(screen) - 16;
        m_previewLeft = right < 180;
        m_previewMax = NSMakeSize(m_previewLeft ? std::clamp(left, 64.0, 180.0) : 180.0,
                                   std::min(600.0, double(screen.size.height - 16)));
        m_previewScreen = screen;
        m_previewAnchor = NSMakePoint(m_previewLeft ? NSMinX(rect) - 8 : NSMaxX(rect) + 8,
                                      NSMaxY(rect));
        m_previewPanel = Panel(NSMakeRect(m_previewAnchor.x, m_previewAnchor.y, 1, 1), overlay.level + 3, false);
        m_preview = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
        m_preview.imageScaling = NSImageScaleProportionallyUpOrDown;
        m_preview.wantsLayer = YES;
        m_preview.layer.borderWidth = 1;
        m_preview.layer.borderColor = ShotletScrollBorderColor().CGColor;
        m_previewPanel.contentView = m_preview;
        m_wasIgnoring = overlay.ignoresMouseEvents;
        overlay.ignoresMouseEvents = YES;
        m_wasVisible = overlay.visible;
        m_wasKey = overlay.keyWindow;
        // SDL 的父视图会自行重画黑底，采集时彻底移出屏幕合成。
        [overlay orderOut:nil];
        [m_mask orderFrontRegardless];
        [m_controls orderFrontRegardless];
        m_controller.Start(std::make_unique<Backend>(display, selection, scale, CGRectFromAppKit(m_controls.frame), language));
    }
    ~UI() override {
        m_controller.Cancel();
        m_controller.Join(); // 先卸载 tap，再关闭允许点击的面板和恢复 overlay。
        m_actions.editAction = nil;
        m_actions.saveAction = nil;
        m_actions.finishAction = nil;
        m_actions.cancelAction = nil;
        [m_controls close]; [m_previewPanel close]; [m_mask close];
        m_overlay.ignoresMouseEvents = m_wasIgnoring;
        if (m_wasVisible) {
            if (m_wasKey) [m_overlay makeKeyAndOrderFront:nil];
            else [m_overlay orderFrontRegardless];
        }
    }
    void SetTheme(Theme theme) override {
        if (!m_controls || !m_toolbarBackground) return;
        const auto bitmap = Toolbar::MakeScrollCaptureBitmap(float(m_controls.backingScaleFactor), theme);
        m_toolbarBackground.image = Image(bitmap.rgba.data(), bitmap.width, bitmap.height);
        m_toolbarBackground.image.size = NSMakeSize(Toolbar::ScrollCaptureWidth, Toolbar::ScrollCaptureHeight);
    }
    void Cancel() override { m_controller.Cancel(); }
    void SetToolbarVisible(bool visible) override {
        if (visible) [m_controls orderFrontRegardless];
        else [m_controls orderOut:nil];
    }
    ScrollCaptureAction TakeAction() override {
        const auto action = m_action;
        m_action = ScrollCaptureAction::None;
        return action;
    }
    ScrollCaptureSnapshot Poll() override {
        auto state = m_controller.Snapshot();
        if (state.state == ScrollCaptureState::Cancelled && !m_controller.IsStopped()) {
            state.state = ScrollCaptureState::Finalizing;
            state.message = Tr(m_language, TextId::ScrollCancelling);
        }
        if (state.revision == m_revision && state.epoch == m_epoch) return state;
        m_revision = state.revision; m_epoch = state.epoch;
        ShotletScrollMaskView* maskView = (ShotletScrollMaskView*)m_mask.contentView;
        if (maskView.reverseScrollBlocked != state.reverseScrollBlocked) {
            maskView.reverseScrollBlocked = state.reverseScrollBlocked;
            maskView.needsDisplay = YES;
        }
        const auto shownPreview = state.preview ? state.preview : state.viewport;
        if (shownPreview && shownPreview != m_lastPreview && m_previewMax.width >= 64) {
            const double ratio = std::min(m_previewMax.width / shownPreview->width,
                                          m_previewMax.height / shownPreview->height);
            m_preview.image = Image(*shownPreview);
            const NSSize size = NSMakeSize(std::max(1.0, shownPreview->width * ratio),
                                           std::max(1.0, shownPreview->height * ratio));
            const CGFloat x = std::clamp(m_previewLeft ? m_previewAnchor.x - size.width : m_previewAnchor.x,
                NSMinX(m_previewScreen) + 8, NSMaxX(m_previewScreen) - size.width - 8);
            const CGFloat y = std::clamp(m_previewAnchor.y - size.height,
                NSMinY(m_previewScreen) + 8, NSMaxY(m_previewScreen) - size.height - 8);
            [m_previewPanel setFrame:NSMakeRect(x, y, size.width, size.height) display:YES];
            [m_previewPanel orderFrontRegardless];
            m_lastPreview = shownPreview;
        }
        return state;
    }
private:
    const Language m_language;
    ScrollCaptureController m_controller;
    NSWindow* __strong m_overlay;
    NSPanel* __strong m_mask;
    NSImageView* __strong m_toolbarBackground = nil;
    NSPanel* __strong m_controls;
    NSPanel* __strong m_previewPanel;
    NSImageView* __strong m_preview;
    ShotletScrollActions* __strong m_actions;
    NSSize m_previewMax = {0, 0};
    NSPoint m_previewAnchor = {0, 0};
    NSRect m_previewScreen = NSZeroRect;
    bool m_previewLeft = false;
    std::shared_ptr<const ScrollImage> m_lastPreview;
    ScrollCaptureAction m_action = ScrollCaptureAction::None;
    bool m_wasIgnoring = false, m_wasVisible = false, m_wasKey = false;
    uint64_t m_revision = 0, m_epoch = 0;
};
} // namespace

std::unique_ptr<ScrollCaptureUI> ScrollCaptureUI::Create(SDL_Window* window, int,
    GlobalRect selection, double scale, std::string& error, Language language, Theme theme) {
    if (!MacScrollDriver::CheckPermission(true)) {
        error = Tr(language, TextId::ScrollAccessibility);
        return {};
    }
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
        error = Tr(language, TextId::ScrollRecording);
        return {};
    }
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    CGGetDisplaysWithPoint(CGPointMake(selection.x + selection.w / 2,
                                      selection.y + selection.h / 2),
                           1, &display, &count);
    if (count == 0) { error = Tr(language, TextId::ScrollDisplayMissing); return {}; }
    auto props = SDL_GetWindowProperties(window);
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!native) { error = Tr(language, TextId::ScrollWindowMissing); return {}; }
    if (selection.w <= 0 || selection.h * scale <= 200) { error = Tr(language, TextId::ScrollTooSmall); return {}; }
    if (!CGRectContainsRect(CGDisplayBounds(display), CGRectMake(selection.x, selection.y, selection.w, selection.h))) {
        error = Tr(language, TextId::ScrollSameDisplay); return {};
    }
    return std::make_unique<UI>(native, display, selection, scale, language, theme);
}
} // namespace shotlet
