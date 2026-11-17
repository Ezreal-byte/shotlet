#include "RecordingVisuals.h"
#include "platform/MediaPermissions_mac.h"
#include "utils/Colors.h"
#include "utils/CoordinateUtils.h"
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <mutex>

namespace shotlet {
namespace {
constexpr double BorderWidthLogical = 2;
constexpr double CameraCornerRadiusLogical = 16;
constexpr double ColorChannelMaximum = 255;

NSPanel* MakeVisualPanel(RectD region) {
    const double mainHeightLogical = CGDisplayBounds(CGMainDisplayID()).size.height;
    const NSRect frame = NSMakeRect(region.x, CGYToAppKitBottomY(region.y, region.height, mainHeightLogical), region.width, region.height);
    NSPanel* panel = [[NSPanel alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
        backing:NSBackingStoreBuffered defer:NO];
    panel.releasedWhenClosed = NO; panel.hidesOnDeactivate = NO;
    panel.ignoresMouseEvents = YES; panel.hasShadow = NO;
    panel.level = NSFloatingWindowLevel;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    return panel;
}

class MacRecordingBorder final : public RecordingBorder {
public:
    bool Show(const RecordingOptions& options) {
        constexpr auto color = Colors::SelectionBorder;
        NSColor* blue = [NSColor colorWithSRGBRed:((color >> IM_COL32_R_SHIFT) & 255) / ColorChannelMaximum
            green:((color >> IM_COL32_G_SHIFT) & 255) / ColorChannelMaximum
            blue:((color >> IM_COL32_B_SHIFT) & 255) / ColorChannelMaximum alpha:1];
        m_panels = [NSMutableArray array];
        const auto bounds = CGDisplayBounds(uint32_t(options.displayId));
        if (!CGDisplayIsActive(uint32_t(options.displayId)) || bounds.size.width <= 0 || bounds.size.height <= 0) return false;
        for (const auto strip : VisibleBorderRects(options.region,
            {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height}, BorderWidthLogical)) {
            auto* panel = MakeVisualPanel(strip);
            if (!panel) return false;
            panel.title = @"Shotlet Recording Border";
            panel.level = NSStatusWindowLevel + 1;
            panel.opaque = YES; panel.backgroundColor = blue;
            [m_panels addObject:panel]; [panel orderFrontRegardless];
        }
        return true;
    }
    ~MacRecordingBorder() override { for (NSPanel* panel in m_panels) [panel close]; }
    bool IsVisible() const override {
        for (NSPanel* panel in m_panels) if (!panel.visible) return false;
        return m_panels.count != 0;
    }
    std::vector<uint32_t> CaptureWindowIds() const override {
        std::vector<uint32_t> result;
        for (NSPanel* panel in m_panels) result.push_back(uint32_t(panel.windowNumber));
        return result;
    }
private:
    NSMutableArray<NSPanel*>* m_panels = nil;
};

struct CameraStatus { std::mutex mutex; std::string error; };
class MacRecordingCamera final : public RecordingCamera {
public:
    bool Show(const RecordingOptions& options, std::string& error) {
        if (!RequestMediaDevicePermission(AVMediaTypeVideo, @"NSCameraUsageDescription", "摄像头", error)) return false;
        auto* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!device) { error = "未找到可用摄像头"; return false; }
        NSError* failure = nil;
        auto* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&failure];
        if (!input) { error = failure.localizedDescription.UTF8String ?: "无法打开摄像头"; return false; }
        m_session = [[AVCaptureSession alloc] init];
        if ([m_session canSetSessionPreset:AVCaptureSessionPreset640x480]) m_session.sessionPreset = AVCaptureSessionPreset640x480;
        if (![m_session canAddInput:input]) { error = "无法配置摄像头输入"; return false; }
        [m_session addInput:input];
        const auto region = RecordingCameraRect(options, 1);
        m_panel = MakeVisualPanel(region);
        if (!m_panel) { error = "无法创建摄像头预览"; return false; }
        m_panel.title = @"Shotlet Recording Camera";
        m_panel.opaque = NO; m_panel.backgroundColor = NSColor.clearColor;
        NSView* view = m_panel.contentView;
        view.wantsLayer = YES;
        view.layer.backgroundColor = NSColor.blackColor.CGColor;
        view.layer.cornerRadius = std::min(CameraCornerRadiusLogical, region.width / 4);
        view.layer.masksToBounds = YES;
        auto* preview = [AVCaptureVideoPreviewLayer layerWithSession:m_session];
        preview.frame = view.bounds; preview.videoGravity = AVLayerVideoGravityResizeAspectFill;
        [view.layer addSublayer:preview];
        [m_panel orderFrontRegardless];
        m_status = std::make_shared<CameraStatus>();
        auto status = m_status;
        m_observer = [NSNotificationCenter.defaultCenter addObserverForName:AVCaptureSessionRuntimeErrorNotification
            object:m_session queue:nil usingBlock:^(NSNotification* notice) {
                NSError* failure = notice.userInfo[AVCaptureSessionErrorKey];
                std::lock_guard lock(status->mutex); status->error = failure.localizedDescription.UTF8String ?: "摄像头已断开";
            }];
        m_queue = dispatch_queue_create("shotlet.recording.camera", DISPATCH_QUEUE_SERIAL);
        AVCaptureSession* session = m_session;
        dispatch_async(m_queue, ^{
            @autoreleasepool {
                @try {
                    [session startRunning];
                    if (!session.running) { std::lock_guard lock(status->mutex); status->error = "摄像头未能启动"; }
                }
                @catch (NSException* exception) { std::lock_guard lock(status->mutex); status->error = exception.reason.UTF8String; }
            }
        });
        return true;
    }
    ~MacRecordingCamera() override {
        if (m_observer) [NSNotificationCenter.defaultCenter removeObserver:m_observer];
        [m_panel close];
        // 捕获任务只持有原生会话，不借用 C++ 对象；排队停止后才能释放摄像头设备。
        AVCaptureSession* session = m_session;
        if (m_queue) dispatch_async(m_queue, ^{ @autoreleasepool { [session stopRunning]; } });
    }
    uint32_t CaptureWindowId() const override { return uint32_t(m_panel.windowNumber); }
    std::string GetError() const override {
        if (!m_status) return {};
        std::lock_guard lock(m_status->mutex); return m_status->error;
    }
private:
    NSPanel* m_panel = nil;
    AVCaptureSession* m_session = nil;
    dispatch_queue_t m_queue = nil;
    id m_observer = nil;
    std::shared_ptr<CameraStatus> m_status;
};
}

std::unique_ptr<RecordingBorder> CreateRecordingBorder(const RecordingOptions& options, std::string& error) {
    auto border = std::make_unique<MacRecordingBorder>();
    if (!border->Show(options)) { error = "无法显示录制选区边框"; return {}; }
    return border;
}
std::unique_ptr<RecordingCamera> CreateRecordingCamera(const RecordingOptions& options, std::string& error) {
    auto camera = std::make_unique<MacRecordingCamera>();
    return camera->Show(options, error) ? std::move(camera) : nullptr;
}
}
