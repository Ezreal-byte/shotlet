#include "RecordingPlaybackWindow.h"
#include "Appearance.h"
#include "FileService.h"
#include "LocalizedText.h"
#include "utils/FilePathUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#import <AppKit/AppKit.h>
#import <AVKit/AVKit.h>

@interface ShotletRecordingPlaybackController : NSWindowController <NSWindowDelegate>
@property(nonatomic, strong) AVPlayerView* playerView;
@property(nonatomic, strong) NSImageView* imageView;
@property(nonatomic, strong) NSTextField* detailLabel;
@property(nonatomic, strong) NSURL* fileURL;
@property(nonatomic) BOOL closed;
- (void)stopPlayback;
- (void)revealFile:(id)sender;
@end

@interface ShotletRecordingPlaybackPanel : NSWindow
@end

@implementation ShotletRecordingPlaybackPanel
- (void)cancelOperation:(id)sender {
    if (self.styleMask & NSWindowStyleMaskFullScreen) [self toggleFullScreen:sender];
    else [self performClose:sender];
}
@end

@implementation ShotletRecordingPlaybackController
- (void)stopPlayback {
    [self.playerView.player pause];
    [self.playerView.player replaceCurrentItemWithPlayerItem:nil];
    self.playerView.player = nil;
    self.imageView.animates = NO;
    self.imageView.image = nil;
}
- (void)windowWillClose:(NSNotification*)notification {
    self.closed = YES;
    [self stopPlayback];
}
- (void)revealFile:(id)sender {
    shotlet::RevealInFileManager(std::filesystem::path(self.fileURL.fileSystemRepresentation));
}
@end

namespace shotlet {
namespace {
namespace Layout = RecordingPlaybackLayout;
constexpr int DetailHeightLogical = 20;

class MacRecordingPlaybackWindow final : public RecordingPlaybackWindow {
public:
    ~MacRecordingPlaybackWindow() override {
        // 先解除委托，避免关闭回调在 C++ 对象销毁期间再次操作窗口状态。
        m_controller.window.delegate = nil;
        [m_controller stopPlayback];
        [m_controller close];
    }
    bool Show(const std::filesystem::path& path, const RecordingOptions& options,
              Language language, Theme theme, std::string& error) {
        const auto utf8 = FilePathUtils::ToUtf8(path);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:utf8.c_str()]];
        if (![NSFileManager.defaultManager isReadableFileAtPath:url.path]) {
            error = Tr(language, TextId::RecordingPlaybackMissing); return false;
        }
        m_language = language;
        auto* window = CreateWindow(url, language);
        m_controller = [[ShotletRecordingPlaybackController alloc] initWithWindow:window];
        m_controller.fileURL = url;
        window.delegate = m_controller;
        if (!CreateMediaView(options.format, error)) return false;
        CreateFooter(language);
        Update(theme);
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        if (m_controller.playerView) {
            [window makeFirstResponder:m_controller.playerView];
            [m_controller.playerView.player play];
        }
        return true;
    }
    bool IsOpen() const override { return m_controller && !m_controller.closed; }
    void Update(Theme theme) override {
        if (!IsOpen()) return;
        ApplyNativeWindowTheme((__bridge void*)m_controller.window, theme);
        auto* item = m_controller.playerView.player.currentItem;
        if (!m_reportedError && item.status == AVPlayerItemStatusFailed) {
            m_reportedError = true;
            NSString* reason = item.error.localizedDescription ?: TrNative(m_language, TextId::RecordingPlaybackFailed);
            m_controller.detailLabel.stringValue = [NSString stringWithFormat:@"%@：%@",
                TrNative(m_language, TextId::RecordingPlaybackFailed), reason];
            m_controller.detailLabel.toolTip = m_controller.detailLabel.stringValue;
            Logger::Warning("event=recording.playback_failed error={}", reason.UTF8String);
        }
    }
private:
    NSWindow* CreateWindow(NSURL* url, Language language) {
        const NSRect work = NSScreen.mainScreen.visibleFrame;
        const NSSize size = NSMakeSize(std::min<double>(Layout::PreferredWidthLogical, work.size.width * Layout::WorkAreaFraction),
            std::min<double>(Layout::PreferredHeightLogical, work.size.height * Layout::WorkAreaFraction));
        auto* window = [[ShotletRecordingPlaybackPanel alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed = NO;
        window.identifier = @"shotlet.recording.playback";
        window.title = [NSString stringWithFormat:@"%@ · %@", TrNative(language, TextId::RecordingPlaybackTitle), url.lastPathComponent];
        window.representedURL = url;
        window.contentMinSize = NSMakeSize(std::min<double>(Layout::MinimumWidthLogical, size.width),
            std::min<double>(Layout::MinimumHeightLogical, size.height));
        window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        return window;
    }
    bool CreateMediaView(RecordingFormat format, std::string& error) {
        NSView* root = m_controller.window.contentView;
        const NSRect mediaFrame = NSMakeRect(0, Layout::FooterHeightLogical, root.bounds.size.width,
            root.bounds.size.height - Layout::FooterHeightLogical);
        if (format == RecordingFormat::Mp4) {
            auto* view = [[AVPlayerView alloc] initWithFrame:mediaFrame];
            view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            view.controlsStyle = AVPlayerViewControlsStyleFloating;
            view.showsFullScreenToggleButton = YES;
            view.videoGravity = AVLayerVideoGravityResizeAspect;
            view.player = [AVPlayer playerWithURL:m_controller.fileURL];
            m_controller.playerView = view;
            [root addSubview:view];
        } else {
            NSImage* image = [[NSImage alloc] initWithContentsOfURL:m_controller.fileURL];
            if (!image.isValid) { error = Tr(m_language, TextId::RecordingPlaybackFailed); return false; }
            auto* view = [[NSImageView alloc] initWithFrame:mediaFrame];
            view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            view.imageScaling = NSImageScaleProportionallyUpOrDown;
            view.wantsLayer = YES;
            view.layer.backgroundColor = NSColor.blackColor.CGColor;
            view.image = image; view.animates = YES;
            m_controller.imageView = view;
            [root addSubview:view];
        }
        return true;
    }
    void CreateFooter(Language language) {
        auto* root = m_controller.window.contentView;
        const auto width = root.bounds.size.width;
        auto* label = [NSTextField labelWithString:m_controller.fileURL.lastPathComponent];
        label.frame = NSMakeRect(Layout::PaddingLogical, (Layout::FooterHeightLogical - DetailHeightLogical) / 2,
            width - Layout::ButtonWidthLogical - Layout::PaddingLogical * 3, DetailHeightLogical);
        label.lineBreakMode = NSLineBreakByTruncatingMiddle;
        label.autoresizingMask = NSViewWidthSizable;
        label.toolTip = m_controller.fileURL.path;
        m_controller.detailLabel = label;
        [root addSubview:label];
        auto* button = [NSButton buttonWithTitle:TrNative(language, TextId::HistoryReveal)
            target:m_controller action:@selector(revealFile:)];
        button.frame = NSMakeRect(width - Layout::ButtonWidthLogical - Layout::PaddingLogical,
            (Layout::FooterHeightLogical - Layout::ButtonHeightLogical) / 2, Layout::ButtonWidthLogical, Layout::ButtonHeightLogical);
        button.autoresizingMask = NSViewMinXMargin;
        [root addSubview:button];
    }
    ShotletRecordingPlaybackController* m_controller = nil;
    Language m_language = Language::English;
    bool m_reportedError = false;
};
}

std::unique_ptr<RecordingPlaybackWindow> CreateRecordingPlaybackWindow(const std::filesystem::path& path,
    const RecordingOptions& options, Language language, Theme theme, std::string& error) {
    error.clear();
    auto window = std::make_unique<MacRecordingPlaybackWindow>();
    return window->Show(path, options, language, theme, error) ? std::move(window) : nullptr;
}
}
