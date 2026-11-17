#include "platform/LocalizedText.h"
#include "OcrWindow.h"
#include "platform/FontProvider.h"
#include "utils/CoordinateUtils.h"
#include "utils/Logger.h"
#include "platform/Appearance.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <cmath>
#include <algorithm>
#include <vector>

// ---- 全局状态 ----

@interface OcrWindowDelegate : NSObject <NSWindowDelegate>
@end

@interface OcrImageView : NSImageView
@end

static NSWindow* s_ocrWindow = nil;
static NSTextView* s_textView = nil;
static NSView* s_dividerView = nil;
static NSScrollView* s_imageScrollView = nil;
static bool s_emptyText = false;
static OcrWindowDelegate* s_delegate = nil;
static std::unique_ptr<unsigned char[]> s_rgbaData;
static int s_rgbaWidth = 0;
static int s_rgbaHeight = 0;

// ---- Objective-C 类实现 ----

@implementation OcrWindowDelegate

- (void)windowWillClose:(NSNotification *)notification {
    shotlet::Logger::Info("[OcrWindow] windowWillClose triggered");
    NSWindow* closingWindow = (NSWindow*)notification.object;
    [closingWindow setDelegate:nil];
    if (s_ocrWindow == closingWindow) {
        s_ocrWindow = nil;
    }
    s_delegate = nil;
    s_textView = nil;
    s_dividerView = nil;
    s_imageScrollView = nil;
    s_rgbaData.reset();
    s_rgbaWidth = 0;
    s_rgbaHeight = 0;
}

@end

@implementation OcrImageView

- (BOOL)isFlipped {
    return YES;
}

@end

// ---- C++ 实现 ----

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

NSScreen* FindScreenForQuartzPoint(CGFloat screenX, CGFloat screenY) {
    CGFloat globalMaxY = GetGlobalMaxY();

    for (NSScreen* screen in [NSScreen screens]) {
        NSRect frame = [screen frame];
        CGFloat quartzMinX = frame.origin.x;
        CGFloat quartzMinY = globalMaxY - frame.origin.y - frame.size.height;
        CGFloat quartzMaxX = quartzMinX + frame.size.width;
        CGFloat quartzMaxY = quartzMinY + frame.size.height;

        if (screenX >= quartzMinX && screenX < quartzMaxX &&
            screenY >= quartzMinY && screenY < quartzMaxY) {
            return screen;
        }
    }

    return [NSScreen mainScreen];
}

CGFloat LinearizeColorComponent(CGFloat value) {
    if (value <= 0.03928) {
        return value / 12.92;
    }
    return pow((value + 0.055) / 1.055, 2.4);
}

CGFloat RelativeLuminance(NSColor* color) {
    NSColor* rgbColor = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!rgbColor) {
        rgbColor = [color colorUsingType:NSColorTypeComponentBased];
    }
    if (!rgbColor) {
        return 1.0;
    }

    CGFloat red = 0.0, green = 0.0, blue = 0.0, alpha = 0.0;
    [rgbColor getRed:&red green:&green blue:&blue alpha:&alpha];

    return 0.2126 * LinearizeColorComponent(red) +
           0.7152 * LinearizeColorComponent(green) +
           0.0722 * LinearizeColorComponent(blue);
}

std::string StringPreview(const std::string& value, size_t maxBytes) {
    std::string preview = value.substr(0, std::min(value.size(), maxBytes));
    std::replace(preview.begin(), preview.end(), '\n', ' ');
    std::replace(preview.begin(), preview.end(), '\r', ' ');
    return preview;
}

const char* AppearanceName(NSAppearance* appearance) {
    if (!appearance || ![appearance name]) {
        return "(null)";
    }
    return [[appearance name] UTF8String];
}

std::string ColorDescription(NSColor* color, NSAppearance* appearance) {
    if (!color) {
        return "(null)";
    }

    __block NSColor* resolvedColor = color;
    if (@available(macOS 11.0, *)) {
        [appearance performAsCurrentDrawingAppearance:^{
            resolvedColor = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        }];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSAppearance* previousAppearance = [NSAppearance currentAppearance];
        [NSAppearance setCurrentAppearance:appearance];
        resolvedColor = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        [NSAppearance setCurrentAppearance:previousAppearance];
#pragma clang diagnostic pop
    }

    if (!resolvedColor) {
        resolvedColor = color;
    }

    NSString* description = [resolvedColor description];
    return description ? std::string([description UTF8String]) : std::string("(unavailable)");
}

NSColor* OcrTextColorForBackground(NSColor* backgroundColor, NSAppearance* appearance, CGFloat* outLuminance) {
    __block CGFloat luminance = 1.0;
    if (@available(macOS 11.0, *)) {
        [appearance performAsCurrentDrawingAppearance:^{
            luminance = RelativeLuminance(backgroundColor);
        }];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSAppearance* previousAppearance = [NSAppearance currentAppearance];
        [NSAppearance setCurrentAppearance:appearance];
        luminance = RelativeLuminance(backgroundColor);
        [NSAppearance setCurrentAppearance:previousAppearance];
#pragma clang diagnostic pop
    }

    if (outLuminance) {
        *outLuminance = luminance;
    }

    return luminance > 0.5
        ? [NSColor blackColor]
        : [NSColor whiteColor];
}

} // namespace

bool OcrWindow::Create(const unsigned char* rgbaData, int width, int height,
                       const std::string& text,
                       float screenX, float screenY,
                       float scaleFactor, Language language, Theme theme) {
    @autoreleasepool {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("[OcrWindow] Invalid parameters");
        return false;
    }

    if (s_ocrWindow) {
        Destroy();
    }

    // 复制 RGBA 数据
    int dataSize = width * height * 4;
    s_rgbaData = std::make_unique<unsigned char[]>(dataSize);
    memcpy(s_rgbaData.get(), rgbaData, dataSize);
    s_rgbaWidth = width;
    s_rgbaHeight = height;

    // RGBA → CGImageRef → NSImage
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bitmapInfo = static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
    CGContextRef ctx = CGBitmapContextCreate(
        s_rgbaData.get(), width, height, 8, width * 4, colorSpace, bitmapInfo);

    if (!ctx) {
        Logger::Error("[OcrWindow] Failed to create bitmap context");
        CGColorSpaceRelease(colorSpace);
        s_textView = nil;
    s_dividerView = nil;
    s_imageScrollView = nil;
    s_rgbaData.reset();
        return false;
    }

    CGImageRef cgImage = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(colorSpace);

    if (!cgImage) {
        Logger::Error("[OcrWindow] Failed to create CGImage");
        s_textView = nil;
    s_dividerView = nil;
    s_imageScrollView = nil;
    s_rgbaData.reset();
        return false;
    }

    NSScreen* targetScreen = FindScreenForQuartzPoint(screenX, screenY);
    CGFloat scaleFactor = [targetScreen backingScaleFactor];
    CGFloat imgW = width / scaleFactor;  // 图片逻辑宽度（点）
    CGFloat imgH = height / scaleFactor;  // 图片逻辑高度（点）
    NSImage* nsImage = [[NSImage alloc] initWithCGImage:cgImage size:NSMakeSize(imgW, imgH)];
    CGImageRelease(cgImage);

    // 创建 delegate
    s_delegate = [[OcrWindowDelegate alloc] init];

    // 窗口尺寸：图片 100% 显示 + OCR 区域为图片宽度的一半
    CGFloat dividerWidth = 1.0;
    CGFloat textW = imgW * 0.5;
    CGFloat defaultWidth = imgW + dividerWidth + textW;
    CGFloat defaultHeight = imgH;

    NSUInteger styleMask = NSWindowStyleMaskTitled |
                           NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable;

    s_ocrWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, defaultWidth, defaultHeight)
                  styleMask:styleMask
                    backing:NSBackingStoreBuffered
                      defer:NO];

    [s_ocrWindow setTitle:TrNative(language, TextId::OcrTitle)];
    [s_ocrWindow setLevel:NSStatusWindowLevel];
    [s_ocrWindow setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                      NSWindowCollectionBehaviorFullScreenAuxiliary];
    [s_ocrWindow setDelegate:s_delegate];
    [s_ocrWindow setReleasedWhenClosed:NO];
    [s_ocrWindow setHasShadow:YES];
    [s_ocrWindow setContentMinSize:NSMakeSize(imgW + dividerWidth + 80.0, imgH)];
    [s_ocrWindow center];

    NSView* contentView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, defaultWidth, defaultHeight)];
    [contentView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    // 左侧：滚动容器承载 1:1 图片。窗口被压小时裁剪/滚动，不缩放图片。
    OcrImageView* imageView = [[OcrImageView alloc] initWithFrame:NSMakeRect(0, 0, imgW, imgH)];
    [imageView setImage:nsImage];
    [imageView setImageScaling:NSImageScaleNone];
    [imageView setImageAlignment:NSImageAlignCenter];
    [imageView setWantsLayer:YES];

    NSScrollView* imageScrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, imgW, imgH)];
    [imageScrollView setHasHorizontalScroller:YES];
    [imageScrollView setHasVerticalScroller:YES];
    [imageScrollView setAutohidesScrollers:YES];
    [imageScrollView setBorderType:NSNoBorder];
    [imageScrollView setDocumentView:imageView];
    [imageScrollView setAutoresizingMask:NSViewHeightSizable];
    [imageScrollView setWantsLayer:YES];
    imageScrollView.layer.borderWidth = 1.0;
    imageScrollView.layer.borderColor = [[NSColor separatorColor] CGColor];

    s_imageScrollView = imageScrollView;
    NSView* dividerView = [[NSView alloc] initWithFrame:NSMakeRect(imgW, 0, dividerWidth, defaultHeight)];
    [dividerView setWantsLayer:YES];
    dividerView.layer.backgroundColor = [[NSColor separatorColor] CGColor];
    s_dividerView = dividerView;
    [dividerView setAutoresizingMask:NSViewHeightSizable];

    // 右侧：NSScrollView + NSTextView 显示识别文本
    NSTextView* textView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, textW, defaultHeight)];
    s_textView = textView;
    s_emptyText = text.empty();
    [textView setEditable:NO];
    [textView setSelectable:YES];
    [textView setRichText:NO];
    NSColor* textBackgroundColor = [NSColor textBackgroundColor];
    NSAppearance* textAppearance = [textView effectiveAppearance];
    CGFloat backgroundLuminance = 1.0;
    NSColor* ocrColor = OcrTextColorForBackground(textBackgroundColor, textAppearance, &backgroundLuminance);
    [textView setDrawsBackground:YES];
    [textView setBackgroundColor:textBackgroundColor];
    [textView setTextColor:ocrColor];

    NSFont* ocrFont = [NSFont fontWithName:@"PingFang SC" size:GetOcrFontSize()];

    NSString* nsText = [NSString stringWithUTF8String:text.c_str()];
    if (!nsText) {
        nsText = @"";
    }
    if (nsText.length == 0) {
        nsText = TrNative(language, TextId::OcrEmpty);
        ocrColor = [NSColor placeholderTextColor];
    }

    // 通过 textStorage 的 attribute dict 设置字体和颜色，确保生效
    NSDictionary* attrs = @{NSFontAttributeName: ocrFont, NSForegroundColorAttributeName: ocrColor};
    [[textView textStorage] replaceCharactersInRange:NSMakeRange(0, 0) withString:nsText];
    NSRange fullRange = NSMakeRange(0, [[textView textStorage] length]);
    [[textView textStorage] addAttributes:attrs range:fullRange];

    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(imgW + dividerWidth, 0, textW, defaultHeight)];
    [scrollView setHasVerticalScroller:YES];
    [scrollView setAutohidesScrollers:YES];
    [scrollView setDocumentView:textView];
    [scrollView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [textView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [textView setMinSize:NSMakeSize(textW, defaultHeight)];
    [textView setVerticallyResizable:YES];
    [textView setHorizontallyResizable:NO];
    [[textView textContainer] setContainerSize:NSMakeSize(textW, CGFLOAT_MAX)];
    [[textView textContainer] setWidthTracksTextView:YES];
    [textView setTextContainerInset:NSMakeSize(8.0, 8.0)];

    [contentView addSubview:imageScrollView];
    [contentView addSubview:dividerView];
    [contentView addSubview:scrollView];

    [s_ocrWindow setContentView:contentView];

    SetTheme(theme);
    [s_ocrWindow makeKeyAndOrderFront:nil];
    [s_ocrWindow makeMainWindow];
    [s_ocrWindow invalidateShadow];

    Logger::Info("[OcrWindow] Create end window={} visible={} image={}x{} textBytes={}",
                 (__bridge void*)s_ocrWindow, [s_ocrWindow isVisible], width, height, text.size());

    return true;
    }
}

void OcrWindow::SetTheme(Theme theme) {
    if (!s_ocrWindow) return;
    ApplyNativeWindowTheme((__bridge void*)s_ocrWindow, theme);
    [s_ocrWindow.effectiveAppearance performAsCurrentDrawingAppearance:^{
        NSColor* background = NSColor.textBackgroundColor;
        CGFloat luminance = 0;
        NSColor* foreground = s_emptyText ? NSColor.placeholderTextColor :
            OcrTextColorForBackground(background, s_ocrWindow.effectiveAppearance, &luminance);
        s_textView.backgroundColor = background;
        s_textView.textColor = foreground;
        s_dividerView.layer.backgroundColor = NSColor.separatorColor.CGColor;
        s_imageScrollView.layer.borderColor = NSColor.separatorColor.CGColor;
    }];
    [s_ocrWindow.contentView setNeedsDisplay:YES];
}

void OcrWindow::Destroy() {
    if (s_ocrWindow) {
        if ([s_ocrWindow isVisible]) {
            [s_ocrWindow close];
        } else {
            [s_ocrWindow setDelegate:nil];
            s_ocrWindow = nil;
            s_delegate = nil;
        }
    } else {
        s_delegate = nil;
    }
    s_textView = nil;
    s_dividerView = nil;
    s_imageScrollView = nil;
    s_rgbaData.reset();
    s_rgbaWidth = 0;
    s_rgbaHeight = 0;
}

bool OcrWindow::IsVisible() {
    return s_ocrWindow != nil && [s_ocrWindow isVisible];
}

void OcrWindow::ProcessEvents() {
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
