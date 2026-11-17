#include "PinWindow.h"
#include "output/LongImageLayout.h"
#include "platform/Appearance.h"
#include "platform/ClipboardService.h"
#include "platform/FileService.h"
#include "platform/LocalizedText.h"
#include "utils/Colors.h"
#include "utils/CoordinateUtils.h"
#include "utils/Logger.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// 每个原生窗口持有自己的图片和置顶状态，视图通过所属 window 访问，避免跨窗口共享。
@interface PinnedImageWindow : NSWindow <NSWindowDelegate> {
    NSBitmapImageRep *_imageBitmap;
}
@property(nonatomic) shotlet::Language language;
@property(nonatomic) shotlet::Theme theme;
@property(nonatomic) BOOL pinActive;
- (BOOL)loadImage:(const unsigned char *)rgbaData width:(int)width height:(int)height;
- (void)copyToClipboard;
- (void)saveToFile;
- (void)applyTheme:(shotlet::Theme)theme;
@end

@interface PinImageView : NSImageView
@end
@interface PinTitlebarPinButton : NSButton {
    BOOL _pinHover;
}
@end

static NSMutableArray<PinnedImageWindow *> *s_pinWindows = nil;

static CGFloat ColorComponent(ImU32 color, int shift) {
    return static_cast<CGFloat>((color >> shift) & 0xff) / 255.0;
}

static NSColor* ColorFromImU32(ImU32 color) {
    return [NSColor colorWithCalibratedRed:ColorComponent(color, IM_COL32_R_SHIFT)
                                     green:ColorComponent(color, IM_COL32_G_SHIFT)
                                      blue:ColorComponent(color, IM_COL32_B_SHIFT)
                                     alpha:ColorComponent(color, IM_COL32_A_SHIFT)];
}

static NSPoint PinPointTowards(const NSPoint from, const NSPoint to, const CGFloat distance) {
    const CGFloat dx = to.x - from.x;
    const CGFloat dy = to.y - from.y;
    const CGFloat len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0) {
        return from;
    }

    const CGFloat t = std::min(distance / len, static_cast<CGFloat>(0.45));
    return NSMakePoint(from.x + dx * t, from.y + dy * t);
}

static void PinQuadraticCurveTo(NSBezierPath* path, const NSPoint control, const NSPoint end) {
    const NSPoint start = [path currentPoint];
    const NSPoint control1 = NSMakePoint(start.x + (control.x - start.x) * 2.0 / 3.0,
                                         start.y + (control.y - start.y) * 2.0 / 3.0);
    const NSPoint control2 = NSMakePoint(end.x + (control.x - end.x) * 2.0 / 3.0,
                                         end.y + (control.y - end.y) * 2.0 / 3.0);
    [path curveToPoint:end controlPoint1:control1 controlPoint2:control2];
}

// ---- Objective-C 类实现（必须在 global scope） ----

@implementation PinnedImageWindow

- (BOOL)loadImage:(const unsigned char *)rgbaData width:(int)width height:(int)height {
    constexpr size_t rgbaBytesPerPixel = 4;
    constexpr int bitsPerComponent = 8;
    const size_t bytesPerRow = size_t(width) * rgbaBytesPerPixel;
    // 位图拥有像素，窗口与图片视图共享它；关闭窗口不会使仍被视图持有的图片悬空。
    // 不设置额外格式标志，使用末尾预乘 alpha，与截图原始 RGBA 一致。
    _imageBitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                           pixelsWide:width
                                                           pixelsHigh:height
                                                        bitsPerSample:bitsPerComponent
                                                      samplesPerPixel:rgbaBytesPerPixel
                                                             hasAlpha:YES
                                                             isPlanar:NO
                                                       colorSpaceName:NSDeviceRGBColorSpace
                                                         bitmapFormat:0
                                                          bytesPerRow:bytesPerRow
                                                         bitsPerPixel:bitsPerComponent * rgbaBytesPerPixel];
    if (!_imageBitmap.bitmapData) {
        shotlet::Logger::Error("[PinWindow] Failed to create image bitmap");
        return NO;
    }
    std::memcpy(_imageBitmap.bitmapData, rgbaData, bytesPerRow * size_t(height));
    // 显式记录像素尺寸，避免 Retina 将原始图片再次放大。
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:_imageBitmap];
    PinImageView *view = [[PinImageView alloc]
            initWithFrame:NSMakeRect(0, 0, self.contentView.bounds.size.width, self.contentView.bounds.size.height)];
    view.image = image;
    view.imageScaling = NSImageScaleProportionallyUpOrDown;
    view.imageAlignment = NSImageAlignCenter;
    view.wantsLayer = YES;
    self.contentView = view;
    return YES;
}

- (void)copyToClipboard {
    if (shotlet::CopyImageToClipboard(_imageBitmap.bitmapData, int(_imageBitmap.pixelsWide),
                                    int(_imageBitmap.pixelsHigh))) {
        shotlet::Logger::Info("[PinWindow] Image copied to clipboard");
    }
}

- (void)saveToFile {
    const auto path = shotlet::SaveFileWithDialog(_imageBitmap.bitmapData, int(_imageBitmap.pixelsWide),
                                                int(_imageBitmap.pixelsHigh), self.language);
    if (!path.empty())
        shotlet::Logger::Info("[PinWindow] event=image.saved");
}

- (void)applyTheme:(shotlet::Theme)theme {
    self.theme = theme;
    shotlet::ApplyNativeWindowTheme((__bridge void *) self, theme);
    self.contentView.layer.borderColor = ColorFromImU32(shotlet::GetThemePalette(theme).border).CGColor;
    for (NSTitlebarAccessoryViewController *accessory in self.titlebarAccessoryViewControllers) {
        [accessory.view setNeedsDisplay:YES];
        for (NSView *button in accessory.view.subviews)
            [button setNeedsDisplay:YES];
    }
    [self.contentView setNeedsDisplay:YES];
}

- (void)windowWillClose:(NSNotification *)notification {
    // 通知持有关闭中的窗口；只移除自身，其他窗口的图片与交互状态不受影响。
    PinnedImageWindow *closingWindow = (PinnedImageWindow *) notification.object;
    closingWindow.delegate = nil;
    [s_pinWindows removeObjectIdenticalTo:closingWindow];
    shotlet::Logger::Info("[PinWindow] Window closed");
}

@end

@implementation PinTitlebarPinButton

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        [self setBordered:NO];
        [self setButtonType:NSButtonTypeMomentaryChange];
        [self setImagePosition:NSNoImage];
        [self setTitle:@""];
        [self setTarget:self];
        [self setAction:@selector(togglePin:)];
    }
    return self;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];

    for (NSTrackingArea* area in [self trackingAreas]) {
        [self removeTrackingArea:area];
    }

    NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited |
                                    NSTrackingActiveInKeyWindow |
                                    NSTrackingInVisibleRect;
    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                        options:options
                                                          owner:self
                                                       userInfo:nil];
    [self addTrackingArea:area];
}

- (void)mouseEntered:(NSEvent *)event {
    _pinHover = true;
    [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent *)event {
    _pinHover = false;
    [self setNeedsDisplay:YES];
}

- (void)togglePin:(id)sender {
    PinnedImageWindow *window = (PinnedImageWindow *) self.window;
    window.pinActive = !window.pinActive;
    window.level = window.pinActive ? NSStatusWindowLevel : NSNormalWindowLevel;
    [self setNeedsDisplay:YES];
    shotlet::Logger::Info("[PinWindow] Topmost {}", window.pinActive ? "enabled" : "disabled");
}

- (BOOL)isFlipped {
    return YES;
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    PinnedImageWindow *window = (PinnedImageWindow *) self.window;

    if ([self isHighlighted] || _pinHover) {
        NSColor *backgroundColor = [self isHighlighted]
                                           ? [NSColor selectedControlColor]
                                           : ColorFromImU32(shotlet::GetThemePalette(window.theme).hoverBackground);
        [backgroundColor setFill];
        NSBezierPath* highlight = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 2.0, 2.0)
                                                                   xRadius:4.0
                                                                   yRadius:4.0];
        [highlight fill];
    }

    const CGFloat iconSize = 20.0;
    const NSRect bounds = self.bounds;
    const CGFloat iconLeft = NSMidX(bounds) - iconSize * 0.5;
    const CGFloat iconTop = NSMidY(bounds) - iconSize * 0.5;
    const CGFloat m = 4.0;
    const CGFloat s = iconSize - 2.0 * m;
    const CGFloat ox = iconLeft + m;
    const CGFloat oy = iconTop + m;

    NSColor *iconColor = window.pinActive ? ColorFromImU32(shotlet::GetThemePalette(window.theme).pinActive)
                                          : [NSColor secondaryLabelColor];
    [iconColor setStroke];

    NSBezierPath* head = [NSBezierPath bezierPath];
    [head setLineWidth:1.0];
    const NSPoint p0 = NSMakePoint(ox + s * 0.25, oy + s * 0.00);
    const NSPoint p1 = NSMakePoint(ox + s * 0.75, oy + s * 0.00);
    const NSPoint p2 = NSMakePoint(ox + s * 0.62, oy + s * 0.25);
    const NSPoint p3 = NSMakePoint(ox + s * 0.90, oy + s * 0.70);
    const NSPoint p4 = NSMakePoint(ox + s * 0.10, oy + s * 0.70);
    const NSPoint p5 = NSMakePoint(ox + s * 0.38, oy + s * 0.25);
    const CGFloat cornerRadius = s * 0.08;
    const NSPoint p0Out = PinPointTowards(p0, p1, cornerRadius);
    const NSPoint p1In = PinPointTowards(p1, p0, cornerRadius);
    const NSPoint p1Out = PinPointTowards(p1, p2, cornerRadius);
    const NSPoint p3In = PinPointTowards(p3, p2, cornerRadius);
    const NSPoint p3Out = PinPointTowards(p3, p4, cornerRadius);
    const NSPoint p4In = PinPointTowards(p4, p3, cornerRadius);
    const NSPoint p4Out = PinPointTowards(p4, p5, cornerRadius);
    const NSPoint p0In = PinPointTowards(p0, p5, cornerRadius);
    [head moveToPoint:p0Out];
    [head lineToPoint:p1In];
    PinQuadraticCurveTo(head, p1, p1Out);
    [head lineToPoint:p2];
    [head lineToPoint:p3In];
    PinQuadraticCurveTo(head, p3, p3Out);
    [head lineToPoint:p4In];
    PinQuadraticCurveTo(head, p4, p4Out);
    [head lineToPoint:p5];
    [head lineToPoint:p0In];
    PinQuadraticCurveTo(head, p0, p0Out);
    [head stroke];

    NSBezierPath* pin = [NSBezierPath bezierPath];
    [pin setLineWidth:1.0];
    const CGFloat pinX = ox + s * 0.50;
    [pin moveToPoint:NSMakePoint(pinX, oy + s * 0.70)];
    [pin lineToPoint:NSMakePoint(pinX, oy + s * 1.00)];
    [pin stroke];
}

@end

// 自定义 ImageView：支持双击复制和右键菜单
@implementation PinImageView

- (void)mouseDown:(NSEvent *)event {
    if (event.clickCount == 2)
        [self copyToClipboard];
    [super mouseDown:event];
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    PinnedImageWindow *window = (PinnedImageWindow *) self.window;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:shotlet::TrNative(window.language, shotlet::TextId::PinMenu)];
    NSMenuItem *copyItem =
            [[NSMenuItem alloc] initWithTitle:shotlet::TrNative(window.language, shotlet::TextId::CopyClipboard)
                                       action:@selector(copyToClipboard)
                                keyEquivalent:@"c"];
    NSMenuItem *saveItem = [[NSMenuItem alloc] initWithTitle:shotlet::TrNative(window.language, shotlet::TextId::SaveFile)
                                                      action:@selector(saveToFile)
                                               keyEquivalent:@"s"];
    // 显式绑定菜单所属视图，不能通过当前活动窗口寻找图片。
    copyItem.target = self;
    saveItem.target = self;
    [menu addItem:copyItem];
    [menu addItem:saveItem];
    return menu;
}

- (void)copyToClipboard {
    [(PinnedImageWindow *) self.window copyToClipboard];
}

- (void)saveToFile {
    // 保存对话框会进入嵌套事件循环，期间关闭窗口也必须保留原始图片。
    __attribute__((objc_precise_lifetime)) PinnedImageWindow *window = (PinnedImageWindow *) self.window;
    [window saveToFile];
}

@end

// ---- C++ 实现（namespace 内） ----

namespace shotlet {

namespace {

CGFloat GetGlobalMaxY() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    std::vector<RectD> rects;
    rects.reserve([screens count]);

    for (NSScreen* screen in screens) {
        NSRect frame = [screen frame];
        rects.push_back({frame.origin.x, frame.origin.y, frame.size.width, frame.size.height});
    }

    return static_cast<CGFloat>(GlobalMaxY(rects.data(), static_cast<int>(rects.size())));
}

NSScreen *TargetScreen(float screenX, float screenY) {
    const CGFloat globalMaxY = GetGlobalMaxY();
    for (NSScreen *screen in NSScreen.screens) {
        const NSRect frame = screen.frame;
        const CGFloat quartzMinY = globalMaxY - NSMaxY(frame);
        if (screenX >= NSMinX(frame) && screenX < NSMaxX(frame) && screenY >= quartzMinY &&
            screenY < quartzMinY + frame.size.height)
            return screen;
    }
    return NSScreen.mainScreen;
}

NSRect InitialWindowFrame(int width, int height, float screenX, float screenY, NSScreen *screen) {
    constexpr CGFloat horizontalScreenMargin = 24, verticalScreenMargin = 80;
    const NSRect visible = screen.visibleFrame;
    const CGFloat pixelScale = screen.backingScaleFactor;
    // 只缩小显示尺寸；复制和保存始终使用窗口持有的原始像素。
    const auto fitted = FitImageToScreen(width / pixelScale, height / pixelScale,
                                         std::max(1.0, double(visible.size.width - horizontalScreenMargin)),
                                         std::max(1.0, double(visible.size.height - verticalScreenMargin)));
    const CGFloat cocoaY = CGYToAppKitBottomY(screenY, fitted.height, GetGlobalMaxY());
    return NSMakeRect(std::clamp<CGFloat>(screenX, NSMinX(visible), NSMaxX(visible) - fitted.width),
                      std::clamp<CGFloat>(cocoaY, NSMinY(visible), NSMaxY(visible) - fitted.height), fitted.width,
                      fitted.height);
}
}

bool PinWindow::Create(const unsigned char* rgbaData, int width, int height,
                       float screenX, float screenY,
                       float scaleFactor, Language language, Theme theme) {
    (void)scaleFactor;
    @autoreleasepool {
        if (!rgbaData || width <= 0 || height <= 0) {
            Logger::Error("[PinWindow] Invalid parameters: data={}, size={}x{}", rgbaData != nullptr, width, height);
            return false;
        }
        NSScreen *targetScreen = TargetScreen(screenX, screenY);
        if (!targetScreen)
            return false;
        const NSRect frame = InitialWindowFrame(width, height, screenX, screenY, targetScreen);
        const NSUInteger styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        PinnedImageWindow *window = [[PinnedImageWindow alloc] initWithContentRect:frame
                                                                         styleMask:styleMask
                                                                           backing:NSBackingStoreBuffered
                                                                             defer:NO];
        window.releasedWhenClosed = NO;
        window.language = language;
        window.pinActive = YES;
        if (![window loadImage:rgbaData width:width height:height])
            return false;
        window.title = @"";
        window.level = NSStatusWindowLevel;
        window.titleVisibility = NSWindowTitleHidden;
        window.titlebarAppearsTransparent = NO;
        window.backgroundColor = NSColor.windowBackgroundColor;
        window.collectionBehavior =
                NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
        window.delegate = window;
        window.hasShadow = YES;

        constexpr CGFloat accessoryWidth = 37, buttonWidth = 32, buttonHeight = 28;
        NSView *container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, accessoryWidth, buttonHeight)];
        PinTitlebarPinButton *button =
                [[PinTitlebarPinButton alloc] initWithFrame:NSMakeRect(0, 0, buttonWidth, buttonHeight)];
        button.toolTip = TrNative(language, TextId::PinToggle);
        [container addSubview:button];
        NSTitlebarAccessoryViewController *accessory = [[NSTitlebarAccessoryViewController alloc] init];
        accessory.view = container;
        accessory.layoutAttribute = NSLayoutAttributeRight;
        [window addTitlebarAccessoryViewController:accessory];
        [window applyTheme:theme];

        // 仅在创建完整后交给集合持有，失败不会影响已经打开的窗口。
        if (!s_pinWindows)
            s_pinWindows = [[NSMutableArray alloc] init];
        [s_pinWindows addObject:window];
        [window makeKeyAndOrderFront:nil];
        [window makeMainWindow];
        [window invalidateShadow];
        Logger::Info("[PinWindow] Created: {}x{} pixels, {}x{} logical, level=NSStatusWindowLevel", width, height,
                     frame.size.width, frame.size.height);
        return true;
    }
}

void PinWindow::SetTheme(Theme theme) {
    for (PinnedImageWindow *window in [s_pinWindows copy])
        [window applyTheme:theme];
}

void PinWindow::Destroy() {
    // close 通知会移除集合元素，遍历快照保证所有窗口都能关闭。
    for (PinnedImageWindow *window in [s_pinWindows copy])
        [window close];
}

bool PinWindow::IsVisible() {
    // 最小化窗口仍可恢复，不能将其当作最后一个窗口关闭而结束进程。
    return s_pinWindows.count > 0;
}

void PinWindow::ProcessEvents() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        NSDate* untilDate = [NSDate dateWithTimeIntervalSinceNow:0];
        while (true) {
            NSEvent* event = [app nextEventMatchingMask:NSEventMaskAny
                                              untilDate:untilDate
                                                 inMode:NSDefaultRunLoopMode
                                                dequeue:YES];
            if (!event) {
                break;
            }
            [app sendEvent:event];
        }

        [app updateWindows];
    }
}

} // namespace shotlet
