#include "platform/LocalizedText.h"
#include "MagnifierOverlay.h"
#include "utils/Logger.h"
#include "utils/Colors.h"
#include "utils/CoordinateUtils.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

// 放大镜参数
static const int kMagnifierSize = 128;   // 放大区域尺寸
static const int kInfoHeight = 78;       // 信息区域高度（坐标+RGB+快捷键）
static const int kTotalHeight = kMagnifierSize + kInfoHeight;  // 总窗口高度
static const int kMagPixels = 15;
static const int kMagZoom = 4;
static const int kMagOffset = 16;

@interface MagnifierWindow : NSWindow
@end

@implementation MagnifierWindow
- (id)initWithContentRect:(NSRect)contentRect {
    self = [super initWithContentRect:contentRect
                             styleMask:NSWindowStyleMaskBorderless
                               backing:NSBackingStoreBuffered
                                 defer:NO];
    if (self) {
        [self setOpaque:NO];
        [self setBackgroundColor:[NSColor clearColor]];
        [self setHasShadow:NO];
        // 放大镜随截图覆盖层直接显隐，避免系统的窗口动画。
        [self setAnimationBehavior:NSWindowAnimationBehaviorNone];
        [self setIgnoresMouseEvents:YES];
        [self setLevel:NSMainMenuWindowLevel + 3];  // 高于 SDL 窗口的 NSMainMenuWindowLevel+2
        [self setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorStationary |
                                     NSWindowCollectionBehaviorIgnoresCycle];
        [self setReleasedWhenClosed:NO];
    }
    return self;
}

// 放大镜窗口永远不应获取键盘焦点
- (BOOL)canBecomeKeyWindow {
    return NO;
}

- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

@interface MagnifierView : NSView {
    id m_localMonitor;
    id m_globalMonitor;
    id m_localKeyMonitor;
    id m_globalKeyMonitor;
}
@property (nonatomic, assign) shotlet::MagnifierOverlay* overlay;
- (void)startMouseMonitor;
- (void)stopMouseMonitor;
- (void)startKeyMonitor;
- (void)stopKeyMonitor;
- (void)onMouseMoved:(int)x y:(int)y;
+ (CGPoint)appKitToCG:(NSPoint)appKitPoint;
+ (NSPoint)cgToAppKit:(CGPoint)cgPoint;
+ (NSScreen*)screenContainingAppKitPoint:(NSPoint)point;
+ (NSScreen*)screenContainingCGPoint:(CGPoint)point;
@end

@implementation MagnifierView

+ (double)globalMaxY {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    std::vector<shotlet::RectD> rects;
    rects.reserve([screens count]);
    for (NSScreen* screen in screens) {
        NSRect frame = screen.frame;
        rects.push_back({frame.origin.x, frame.origin.y, frame.size.width, frame.size.height});
    }
    return shotlet::GlobalMaxY(rects.data(), static_cast<int>(rects.size()));
}

- (id)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        m_localMonitor = nil;
        m_globalMonitor = nil;
        m_localKeyMonitor = nil;
        m_globalKeyMonitor = nil;
    }
    return self;
}

- (void)dealloc {
    [self stopKeyMonitor];
    [self stopMouseMonitor];
}

- (void)setOverlay:(shotlet::MagnifierOverlay*)overlay {
    _overlay = overlay;
}

- (void)startMouseMonitor {
    if (m_localMonitor || m_globalMonitor) return;
    __weak typeof(self) weakSelf = self;
    // localMonitor：使用 [NSEvent mouseLocation] 获取全局坐标，而非 event.locationInWindow（窗口内坐标）
    m_localMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMouseMoved handler:^NSEvent*(NSEvent* event) {
        CGPoint screenLocation = [NSEvent mouseLocation];
        CGPoint cgLocation = [MagnifierView appKitToCG:screenLocation];
        [weakSelf onMouseMoved:(int)cgLocation.x y:(int)cgLocation.y];
        return event;
    }];
    m_globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved handler:^(NSEvent* event) {
        CGPoint screenLocation = [NSEvent mouseLocation];
        CGPoint cgLocation = [MagnifierView appKitToCG:screenLocation];
        [weakSelf onMouseMoved:(int)cgLocation.x y:(int)cgLocation.y];
    }];
}

- (void)stopMouseMonitor {
    if (m_localMonitor) { [NSEvent removeMonitor:m_localMonitor]; m_localMonitor = nil; }
    if (m_globalMonitor) { [NSEvent removeMonitor:m_globalMonitor]; m_globalMonitor = nil; }
}

- (void)startKeyMonitor {
    if (m_localKeyMonitor || m_globalKeyMonitor) return;
    __weak typeof(self) weakSelf = self;
    // 监听 ⌘C 快捷键（复制色值）
    m_localKeyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        if ([event type] == NSEventTypeKeyDown &&
            (event.modifierFlags & NSEventModifierFlagCommand) &&
            !(event.modifierFlags & NSEventModifierFlagShift) &&
            !(event.modifierFlags & NSEventModifierFlagControl) &&
            !(event.modifierFlags & NSEventModifierFlagOption) &&
            [[event charactersIgnoringModifiers] isEqualToString:@"c"]) {
            if (weakSelf.overlay) {
                weakSelf.overlay->CopyColorToClipboard();
                return nil;  // 消费该事件，不继续传递
            }
        }
        return event;
    }];
    m_globalKeyMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^(NSEvent* event) {
        if ((event.modifierFlags & NSEventModifierFlagCommand) &&
            !(event.modifierFlags & NSEventModifierFlagShift) &&
            !(event.modifierFlags & NSEventModifierFlagControl) &&
            !(event.modifierFlags & NSEventModifierFlagOption) &&
            [[event charactersIgnoringModifiers] isEqualToString:@"c"]) {
            // globalMonitor 不能消费事件，只能观察
        }
    }];
}

- (void)stopKeyMonitor {
    if (m_localKeyMonitor) { [NSEvent removeMonitor:m_localKeyMonitor]; m_localKeyMonitor = nil; }
    if (m_globalKeyMonitor) { [NSEvent removeMonitor:m_globalKeyMonitor]; m_globalKeyMonitor = nil; }
}

- (void)onMouseMoved:(int)x y:(int)y {
    if (self.overlay) {
        self.overlay->UpdateMousePosition(x, y);
        [self setNeedsDisplay:YES];
    }
}

// 将 AppKit 全局坐标（原点在主屏左下角，Y 朝上）转换为 CG 全局坐标（原点在主屏左上角，Y 朝下）
+ (CGPoint)appKitToCG:(NSPoint)appKitPoint {
    NSScreen* screen = [MagnifierView screenContainingAppKitPoint:appKitPoint];
    if (screen) {
        NSDictionary* desc = [screen deviceDescription];
        NSNumber* screenNumber = desc[@"NSScreenNumber"];
        if (screenNumber) {
            CGDirectDisplayID displayID = static_cast<CGDirectDisplayID>([screenNumber unsignedIntValue]);
            CGRect cgBounds = CGDisplayBounds(displayID);
            NSRect appKitFrame = screen.frame;
            CGFloat cgY = static_cast<CGFloat>(shotlet::AppKitYToCGYInScreen(
                appKitPoint.y,
                appKitFrame.origin.y,
                appKitFrame.size.height,
                cgBounds.origin.y));
            return CGPointMake(appKitPoint.x, cgY);
        }
    }

    const double globalMaxY = [MagnifierView globalMaxY];
    CGFloat cgY = static_cast<CGFloat>(shotlet::AppKitYToCGY(appKitPoint.y, globalMaxY));
    return CGPointMake(appKitPoint.x, cgY);
}

+ (NSPoint)cgToAppKit:(CGPoint)cgPoint {
    NSScreen* screen = [MagnifierView screenContainingCGPoint:cgPoint];
    if (screen) {
        NSDictionary* desc = [screen deviceDescription];
        NSNumber* screenNumber = desc[@"NSScreenNumber"];
        if (screenNumber) {
            CGDirectDisplayID displayID = static_cast<CGDirectDisplayID>([screenNumber unsignedIntValue]);
            CGRect cgBounds = CGDisplayBounds(displayID);
            NSRect appKitFrame = screen.frame;
            CGFloat appKitY = static_cast<CGFloat>(shotlet::CGYToAppKitBottomYInScreen(
                cgPoint.y,
                0.0,
                appKitFrame.origin.y,
                appKitFrame.size.height,
                cgBounds.origin.y));
            return NSMakePoint(cgPoint.x, appKitY);
        }
    }

    const double globalMaxY = [MagnifierView globalMaxY];
    CGFloat appKitY = static_cast<CGFloat>(shotlet::AppKitYToCGY(cgPoint.y, globalMaxY));
    return NSMakePoint(cgPoint.x, appKitY);
}

+ (NSScreen*)screenContainingAppKitPoint:(NSPoint)point {
    for (NSScreen* screen in [NSScreen screens]) {
        if (NSPointInRect(point, screen.frame)) {
            return screen;
        }
    }
    return [NSScreen mainScreen];
}

+ (NSScreen*)screenContainingCGPoint:(CGPoint)point {
    for (NSScreen* screen in [NSScreen screens]) {
        NSDictionary* desc = [screen deviceDescription];
        NSNumber* screenNumber = desc[@"NSScreenNumber"];
        if (!screenNumber) continue;

        CGDirectDisplayID displayID = static_cast<CGDirectDisplayID>([screenNumber unsignedIntValue]);
        CGRect bounds = CGDisplayBounds(displayID);
        if (point.x >= bounds.origin.x && point.x < bounds.origin.x + bounds.size.width &&
            point.y >= bounds.origin.y && point.y < bounds.origin.y + bounds.size.height) {
            return screen;
        }
    }
    return [NSScreen mainScreen];
}

// 根据 CG 坐标找到鼠标所在的屏幕数据索引
- (int)findScreenIndexForCGPoint:(CGPoint)cgPoint {
    shotlet::MagnifierOverlay* overlay = self.overlay;
    if (!overlay) return -1;

    for (size_t i = 0; i < overlay->m_screens.size(); i++) {
        const auto& screen = overlay->m_screens[i];
        // screen.screenX/Y 是逻辑点坐标（CG 坐标系），screen.pixelWidth/Height 是像素
        // 需要转换回逻辑点来比较
        float sf = screen.scaleFactor;
        float logicalW = screen.pixelWidth / sf;
        float logicalH = screen.pixelHeight / sf;
        if (cgPoint.x >= screen.screenX && cgPoint.x < screen.screenX + logicalW &&
            cgPoint.y >= screen.screenY && cgPoint.y < screen.screenY + logicalH) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];

    shotlet::MagnifierOverlay* overlay = self.overlay;
    if (!overlay || overlay->m_screens.empty()) {
        return;
    }

    // m_mouseX/Y 使用与截图数据一致的 CG/SDL 全局坐标（左上原点）
    int mouseX = overlay->m_mouseX;
    int mouseY = overlay->m_mouseY;

    CGPoint cgPoint = CGPointMake(mouseX, mouseY);

    // 根据鼠标位置找到对应的屏幕数据
    int screenIdx = [self findScreenIndexForCGPoint:cgPoint];
    if (screenIdx < 0) {
        // 鼠标不在任何已知屏幕上，尝试第一个屏幕作为 fallback
        if (!overlay->m_screens.empty()) {
            screenIdx = 0;
        } else {
            return;
        }
    }

    const auto& screen = overlay->m_screens[screenIdx];
    const uint8_t* screenData = screen.GetData();
    if (!screenData || screen.pixelWidth <= 0 || screen.pixelHeight <= 0) return;

    // 计算鼠标在该屏幕截图缓冲区内的局部坐标
    // screen.screenX/Y 是逻辑点，cgPoint 也是逻辑点
    CGFloat localXPoints = cgPoint.x - screen.screenX;
    CGFloat localYPoints = cgPoint.y - screen.screenY;

    // 截图缓冲区以物理像素存储，乘以 scaleFactor 得到像素坐标
    float sf = screen.scaleFactor;
    int localX = static_cast<int>(localXPoints * sf);
    int localY = static_cast<int>(localYPoints * sf);

    // 计算抓取矩形（在像素缓冲区坐标系中）
    int magPixelsPx = static_cast<int>(kMagPixels * sf);
    int captureSizePx = 2 * magPixelsPx + 1;
    int magX = std::max(0, std::min(localX - magPixelsPx, screen.pixelWidth - captureSizePx));
    int magY = std::max(0, std::min(localY - magPixelsPx, screen.pixelHeight - captureSizePx));

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bitmapInfo = (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big;

    std::unique_ptr<uint8_t[]> captureBuffer = std::make_unique<uint8_t[]>(captureSizePx * captureSizePx * 4);
    for (int y = 0; y < captureSizePx; y++) {
        int srcY = magY + y;
        if (srcY >= 0 && srcY < screen.pixelHeight) {
            const uint8_t* srcRow = screenData + srcY * screen.rowPitch;
            uint8_t* dstRow = captureBuffer.get() + y * captureSizePx * 4;
            int copyWidth = std::min(captureSizePx, screen.pixelWidth - magX);
            if (copyWidth > 0) {
                memcpy(dstRow, srcRow + magX * 4, copyWidth * 4);
            }
        }
    }

    CGContextRef captureContext = CGBitmapContextCreate(captureBuffer.get(), captureSizePx, captureSizePx, 8, captureSizePx * 4, colorSpace, bitmapInfo);
    CGImageRef captureImage = nil;
    if (captureContext) {
        captureImage = CGBitmapContextCreateImage(captureContext);
        CGContextRelease(captureContext);
    }

    if (!captureImage) {
        CGColorSpaceRelease(colorSpace);
        return;
    }

    CGSize windowSize = self.bounds.size;
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];

    // ---- 放大区域（顶部 kMagnifierSize 高度） ----
    CGRect zoomedRect = CGRectMake(0, kInfoHeight, kMagnifierSize, kMagnifierSize);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, zoomedRect, captureImage);

    CGImageRelease(captureImage);
    CGColorSpaceRelease(colorSpace);

    // 更新像素 RGB 数据到 overlay
    if (localX >= 0 && localX < screen.pixelWidth && localY >= 0 && localY < screen.pixelHeight) {
        const uint8_t* pixel = screenData + localY * screen.rowPitch + localX * 4;
        overlay->m_pixelR = pixel[0];
        overlay->m_pixelG = pixel[1];
        overlay->m_pixelB = pixel[2];
    }
    overlay->m_pixelX = static_cast<int>(localXPoints);
    overlay->m_pixelY = static_cast<int>(localYPoints);

    // 十字线（在放大区域内）
    float centerX = floor(kMagnifierSize / 2) + 0.5;
    float centerY = floor(kMagnifierSize / 2) + 0.5;
    const auto crosshair = ImGui::ColorConvertU32ToFloat4(shotlet::Colors::MagnifierCrosshair);
    [[NSColor colorWithRed:crosshair.x green:crosshair.y blue:crosshair.z alpha:crosshair.w] setStroke];

    NSBezierPath* hLine = [NSBezierPath bezierPath];
    [hLine moveToPoint:NSMakePoint(0, kInfoHeight + centerY)];
    [hLine lineToPoint:NSMakePoint(kMagnifierSize, kInfoHeight + centerY)];
    hLine.lineWidth = 1.8;
    [hLine stroke];

    NSBezierPath* vLine = [NSBezierPath bezierPath];
    [vLine moveToPoint:NSMakePoint(centerX, kInfoHeight)];
    [vLine lineToPoint:NSMakePoint(centerX, kInfoHeight + kMagnifierSize)];
    vLine.lineWidth = 1.8;
    [vLine stroke];

    const auto& palette = shotlet::GetThemePalette(overlay->m_theme);
    auto nativeColor = [](ImU32 color) {
        const auto c = ImGui::ColorConvertU32ToFloat4(color);
        return [NSColor colorWithSRGBRed:c.x green:c.y blue:c.z alpha:c.w];
    };
    // ---- 信息区域（底部 kInfoHeight 高度） ----
    NSRect infoRect = NSMakeRect(0, 0, kMagnifierSize, kInfoHeight);

    // 半透明黑色背景（底部两角圆角，与放大镜圆角一致）
    [nativeColor(palette.panelBackground) setFill];
    CGFloat cr = 8.0; // 圆角半径
    NSBezierPath* infoBgPath = [NSBezierPath bezierPath];
    // 从左上角开始，顺时针绘制
    [infoBgPath moveToPoint:NSMakePoint(0, kInfoHeight)];           // 左上角
    [infoBgPath lineToPoint:NSMakePoint(0, cr)];                     // 左边向下到圆角起点
    [infoBgPath appendBezierPathWithArcWithCenter:NSMakePoint(cr, cr)  // 左下角圆弧
                                radius:cr startAngle:180.0 endAngle:270.0];
    [infoBgPath lineToPoint:NSMakePoint(kMagnifierSize - cr, 0)];   // 底边
    [infoBgPath appendBezierPathWithArcWithCenter:NSMakePoint(kMagnifierSize - cr, cr) // 右下角圆弧
                                radius:cr startAngle:270.0 endAngle:360.0];
    [infoBgPath lineToPoint:NSMakePoint(kMagnifierSize, kInfoHeight)]; // 右边向上到顶边
    [infoBgPath lineToPoint:NSMakePoint(0, kInfoHeight)];           // 顶边（直线）
    [infoBgPath fill];

    // 分隔线
    [nativeColor(palette.border) setStroke];
    NSBezierPath* separatorLine = [NSBezierPath bezierPath];
    [separatorLine moveToPoint:NSMakePoint(4, kInfoHeight)];
    [separatorLine lineToPoint:NSMakePoint(kMagnifierSize - 4, kInfoHeight)];
    separatorLine.lineWidth = 0.5;
    [separatorLine stroke];

    // 绘制文字信息
    NSFont* infoFont = [NSFont systemFontOfSize:12.0f];
    NSColor* textColor = nativeColor(palette.text);
    NSDictionary* textAttrs = @{
        NSFontAttributeName: infoFont,
        NSForegroundColorAttributeName: textColor
    };

    // 第一行：坐标
    NSString* coordText = [NSString stringWithFormat:@"%@  %d,%d", shotlet::TrNative(overlay->m_language, shotlet::TextId::MagnifierPosition), overlay->m_pixelX, overlay->m_pixelY];
    NSPoint coordPos = NSMakePoint(6, kInfoHeight - 30);
    [coordText drawAtPoint:coordPos withAttributes:textAttrs];

    // 第二行：RGB + 十六进制色值
    NSString* rgbText = [NSString stringWithFormat:@"%@  #%02X%02X%02X", shotlet::TrNative(overlay->m_language, shotlet::TextId::MagnifierColor),
                         overlay->m_pixelR, overlay->m_pixelG, overlay->m_pixelB];
    NSPoint rgbPos = NSMakePoint(6, kInfoHeight - 50);
    [rgbText drawAtPoint:rgbPos withAttributes:textAttrs];

    // 第三行：快捷键提示
    NSFont* shortcutFont = [NSFont systemFontOfSize:10.0f];
    NSColor* shortcutColor = nativeColor(palette.secondaryText);
    NSDictionary* shortcutAttrs = @{
        NSFontAttributeName: shortcutFont,
        NSForegroundColorAttributeName: shortcutColor
    };
    NSString* shortcutText = shotlet::TrNative(overlay->m_language, shotlet::TextId::MagnifierCopyMac);
    NSPoint shortcutPos = NSMakePoint(6, kInfoHeight - 70);
    [shortcutText drawAtPoint:shortcutPos withAttributes:shortcutAttrs];

    // 圆角边框（整个窗口）
    NSRect borderRect = NSInsetRect(self.bounds, 0.5, 0.5);
    const auto border = shotlet::Colors::MagnifierBorderVec4;
    [[NSColor colorWithDeviceWhite:border.x alpha:border.w] setStroke];
    CGFloat cornerRadius = 8.0;
    NSBezierPath* borderPath = [NSBezierPath bezierPathWithRoundedRect:borderRect xRadius:cornerRadius yRadius:cornerRadius];
    borderPath.lineWidth = 1.0;
    [borderPath stroke];
}
@end

struct shotlet::MagnifierOverlay::Impl {
    MagnifierWindow* window;
    MagnifierView* view;
};

shotlet::MagnifierOverlay::MagnifierOverlay() : m_impl(std::make_unique<Impl>()) {
    m_impl->window = nil;
    m_impl->view = nil;
}

shotlet::MagnifierOverlay::~MagnifierOverlay() { Shutdown(); }

bool shotlet::MagnifierOverlay::Initialize() {
    if (m_impl->window) return true;
    NSScreen* mainScreen = [NSScreen mainScreen];
    if (!mainScreen) return false;

    NSRect windowRect = NSMakeRect(0, 0, kMagnifierSize, kTotalHeight);
    m_impl->window = [[MagnifierWindow alloc] initWithContentRect:windowRect];
    m_impl->view = [[MagnifierView alloc] initWithFrame:NSMakeRect(0, 0, kMagnifierSize, kTotalHeight)];
    m_impl->view.overlay = this;
    [m_impl->window.contentView addSubview:m_impl->view];
    return true;
}

void shotlet::MagnifierOverlay::Shutdown() {
    if (m_impl->view) { [m_impl->view stopMouseMonitor]; m_impl->view = nil; }
    if (m_impl->window) { [m_impl->window close]; m_impl->window = nil; }
}

void shotlet::MagnifierOverlay::Show() {
    if (!m_impl->window) Initialize();

    // 在显示前强制同步位置和渲染状态
    RefreshMousePosition();
    [m_impl->view setNeedsDisplay:YES];
    [m_impl->view startMouseMonitor];
    [m_impl->view startKeyMonitor];

    // 此时数据已就绪，再上屏
    [m_impl->window orderFront:nil];
    m_visible = true;
}

void shotlet::MagnifierOverlay::Hide() {
    if (m_impl->view) { [m_impl->view stopKeyMonitor]; [m_impl->view stopMouseMonitor]; }
    if (m_impl->window) { [m_impl->window orderOut:nil]; }
    m_visible = false;
}

void shotlet::MagnifierOverlay::RefreshMousePosition() {
    NSPoint mouseLocation = [NSEvent mouseLocation];
    CGPoint cgMouseLocation = [MagnifierView appKitToCG:mouseLocation];
    UpdateMousePosition(static_cast<int>(cgMouseLocation.x),
                        static_cast<int>(cgMouseLocation.y));
}

void shotlet::MagnifierOverlay::UpdateMousePosition(int x, int y) {
    if (!m_impl->window) return;

    m_mouseX = x;
    m_mouseY = y;

    // 找到包含该点的屏幕，以获取正确的边界信息
    CGPoint cgMousePoint = CGPointMake(x, y);
    NSPoint mousePoint = [MagnifierView cgToAppKit:cgMousePoint];
    NSScreen* currentScreen = nil;
    for (NSScreen* screen in [NSScreen screens]) {
        if (NSPointInRect(mousePoint, screen.frame)) {
            currentScreen = screen;
            break;
        }
    }
    
    if (!currentScreen) currentScreen = [NSScreen mainScreen];
    if (!currentScreen) return;

    // 使用 screen.frame（完整帧）而非 visibleFrame（扣除 Dock/菜单栏）
    NSRect screenFrame = currentScreen.frame;

    // 计算放大镜窗口位置（AppKit 坐标系，原点在主屏左下角，Y 朝上）
    // 默认放在鼠标右下方
    CGFloat newX = mousePoint.x + 20;
    CGFloat newY = mousePoint.y - kTotalHeight - 20;

    // 边界修正
    if (newX + kMagnifierSize > screenFrame.origin.x + screenFrame.size.width) {
        newX = mousePoint.x - kMagnifierSize - 20;
    }
    if (newX < screenFrame.origin.x) {
        newX = screenFrame.origin.x;
    }
    if (newY < screenFrame.origin.y) {
        newY = mousePoint.y + 20;  // 放到鼠标上方
    }
    if (newY + kTotalHeight > screenFrame.origin.y + screenFrame.size.height) {
        newY = screenFrame.origin.y + screenFrame.size.height - kTotalHeight;
    }

    // 直接设置窗口位置，不做 orderOut/orderFront 切换
    // macOS NSWindow 原生支持跨屏幕移动，orderOut/orderFront 反而造成残影
    [m_impl->window setFrameOrigin:NSMakePoint(newX, newY)];
}

void shotlet::MagnifierOverlay::SetScreenData(std::vector<ScreenData>&& screens) {
    m_screens = std::move(screens);

    // 如果窗口已经显示，立即重绘
    if (m_visible && m_impl->view) {
        [m_impl->view setNeedsDisplay:YES];
    }
}

void shotlet::MagnifierOverlay::ClearScreenData() {
    m_screens.clear();
}

void shotlet::MagnifierOverlay::SetScreenshotData(int width, int height, const uint8_t* data, int rowPitch, int x, int y, float scaleFactor) {
    // 兼容旧接口：包装为单屏幕数据
    ScreenData sd;
    sd.pixelWidth = width;
    sd.pixelHeight = height;
    sd.screenX = x;
    sd.screenY = y;
    sd.rowPitch = rowPitch;
    sd.scaleFactor = scaleFactor;
    sd.displayId = -1;
    int dataSize = rowPitch * height;
    sd.data = std::make_unique<uint8_t[]>(dataSize);
    memcpy(sd.data.get(), data, dataSize);

    m_screens.clear();
    m_screens.push_back(std::move(sd));

    if (m_visible && m_impl->view) {
        [m_impl->view setNeedsDisplay:YES];
    }
}

void shotlet::MagnifierOverlay::UpdateScreenshot() {
    if (m_impl->view) [m_impl->view setNeedsDisplay:YES];
}

const shotlet::ScreenData* shotlet::MagnifierOverlay::GetScreenData(int displayId) const {
    for (const auto& sd : m_screens) {
        if (sd.displayId == displayId) return &sd;
    }
    return nullptr;
}

void shotlet::MagnifierOverlay::CopyColorToClipboard() {
    NSString* colorString = [NSString stringWithFormat:@"#%02X%02X%02X",
                              m_pixelR, m_pixelG, m_pixelB];
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:colorString forType:NSPasteboardTypeString];
    Logger::Info("[MagnifierOverlay] Copied color to clipboard: {}", [colorString UTF8String]);
}

#endif
