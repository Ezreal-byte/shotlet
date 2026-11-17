#include "ClipboardService.h"
#include "MacImageOutput.h"
#include "utils/Logger.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreServices/CoreServices.h>

@interface ShotletPngPasteboardProvider : NSObject <NSPasteboardItemDataProvider> {
@public
    std::shared_future<shotlet::ClipboardPng> pending;
    std::shared_ptr<std::atomic<bool>> published;
    int expectedVersion;
}
@end

@implementation ShotletPngPasteboardProvider
- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
    if (![type isEqualToString:NSPasteboardTypePNG] || !pending.valid()) return;
    const auto png = pending.get();
    if (png && int(pasteboard.changeCount) == expectedVersion && [item setData:(__bridge NSData*)png.get() forType:type] && published) published->store(true);
}
@end

namespace shotlet {

ClipboardPng EncodeClipboardPng(const unsigned char* rgbaData, int width, int height) {
    @autoreleasepool {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("CopyImageToClipboard: invalid parameters");
        return {};
    }

    // 创建 PNG 数据
    CFMutableDataRef pngData = CFDataCreateMutable(nullptr, 0);
    if (!pngData) {
        Logger::Error("Failed to create PNG data");
        return {};
    }

    CFStringRef utiType = CFSTR("public.png");
    CGImageDestinationRef destination = CGImageDestinationCreateWithData(
        pngData, utiType, 1, nullptr);

    if (!destination) {
        Logger::Error("Failed to create image destination");
        CFRelease(pngData);
        return {};
    }

    // 借用原始像素，避免大图再创建完整 bitmap context 副本。
    CGImageRef cgImage = CreateImageFromRGBA(rgbaData, width, height);

    if (!cgImage) {
        Logger::Error("Failed to create CGImage");
        CFRelease(destination);
        CFRelease(pngData);
        return {};
    }

    CGImageDestinationAddImage(destination, cgImage, nullptr);
    CGImageRelease(cgImage);

    bool finalized = CGImageDestinationFinalize(destination);
    CFRelease(destination);

    if (!finalized) {
        Logger::Error("Failed to finalize PNG");
        CFRelease(pngData);
        return {};
    }

    Logger::Debug("PNG size: {} bytes", CFDataGetLength(pngData));

    return ClipboardPng(pngData, [](CFDataRef data) { CFRelease(data); });
    }
}

int DeclareClipboardPng(std::shared_future<ClipboardPng> png, std::shared_ptr<std::atomic<bool>> published) {
    @autoreleasepool {
    if (!png.valid()) return -1;
    ShotletPngPasteboardProvider* provider = [ShotletPngPasteboardProvider new];
    provider->pending = std::move(png);
    provider->published = std::move(published);
    NSPasteboardItem* item = [NSPasteboardItem new];
    [item setDataProvider:provider forTypes:@[NSPasteboardTypePNG]];
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    if (![pasteboard writeObjects:@[item]]) return -1;
    provider->expectedVersion = int(pasteboard.changeCount);
    return provider->expectedVersion;
    }
}

bool IsClipboardPngCurrent(int version) {
    return version >= 0 && [NSPasteboard generalPasteboard].changeCount == version;
}

bool PublishClipboardPng(const ClipboardPng& png, int expectedVersion) {
    @autoreleasepool {
    if (!png) return false;
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    if (expectedVersion >= 0) {
        if (pasteboard.changeCount != expectedVersion) return false;
    } else {
        [pasteboard clearContents];
    }
    NSData* nsData = (__bridge NSData*)png.get();
    BOOL success = [pasteboard setData:nsData forType:NSPasteboardTypePNG];

    return success == YES;
    }
}

bool CopyImageToClipboard(const unsigned char* rgbaData, int width, int height) {
    return PublishClipboardPng(EncodeClipboardPng(rgbaData, width, height));
}

} // namespace shotlet
