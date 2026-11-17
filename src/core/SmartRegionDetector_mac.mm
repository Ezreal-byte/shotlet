#include "SmartRegionDetector.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <algorithm>

namespace shotlet {

// 辅助：CFStringRef 转 std::string
static std::string CFStringToStd(CFStringRef ref) {
    if (!ref) return "";
    const char* cstr = CFStringGetCStringPtr(ref, kCFStringEncodingUTF8);
    if (cstr) return std::string(cstr);
    CFIndex length = CFStringGetLength(ref);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(maxSize);
    if (CFStringGetCString(ref, buffer.data(), maxSize, kCFStringEncodingUTF8))
        return std::string(buffer.data());
    return "";
}

void SmartRegionDetector::BuildCache(float screenOffsetX, float screenOffsetY,
                                     float screenW, float screenH) {
    m_cache.clear();

    pid_t selfPid = getpid();

    CFArrayRef windowList = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) return;

    CFIndex count = CFArrayGetCount(windowList);

    std::vector<RawWindow> rawWindows;

    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windowList, i));

        // 排除自身进程
        CFNumberRef pidRef = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowOwnerPID));
        if (!pidRef) continue;
        int pid = 0;
        CFNumberGetValue(pidRef, kCFNumberSInt32Type, &pid);
        if (pid == selfPid) continue;

        // 只保留普通窗口层(0)和菜单栏层(24)
        CFNumberRef layerRef = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowLayer));
        if (!layerRef) continue;
        int layer = 0;
        CFNumberGetValue(layerRef, kCFNumberSInt32Type, &layer);
        if (layer != 0 && layer != 24) continue;

        // 获取 bounds
        CFDictionaryRef boundsRef = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
        if (!boundsRef) continue;
        CGRect rect;
        if (!CGRectMakeWithDictionaryRepresentation(boundsRef, &rect)) continue;

        // 排除极小窗口
        if (rect.size.width < 10 || rect.size.height < 10) continue;

        // 构建标签
        std::string ownerName = CFStringToStd(static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowOwnerName)));
        std::string windowName = CFStringToStd(static_cast<CFStringRef>(CFDictionaryGetValue(info, kCGWindowName)));
        std::string label;
        if (!ownerName.empty() && !windowName.empty())
            label = ownerName + " - " + windowName;
        else if (!ownerName.empty())
            label = ownerName;
        else
            label = windowName;

        ImRect global(ImVec2(rect.origin.x, rect.origin.y),
                       ImVec2(rect.origin.x + rect.size.width, rect.origin.y + rect.size.height));

        rawWindows.push_back({global, label});
    }

    CFRelease(windowList);

    BuildCacheFromWindows(rawWindows, screenOffsetX, screenOffsetY, screenW, screenH);
}

} // namespace shotlet
