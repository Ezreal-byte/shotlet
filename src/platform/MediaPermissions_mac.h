#pragma once
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#include <atomic>
#include <format>
#include <memory>
#include <string>

namespace shotlet {
// 必须在主线程调用；用途说明来自宿主 bundle，缺失时不能触发会终止进程的 TCC 请求。
inline bool RequestMediaDevicePermission(AVMediaType type, NSString* usageKey, const char* device, std::string& error) {
    // 已授权时无需用途说明：TCC 只在真正弹窗请求时读取宿主 bundle 的 Info.plist。
    // Node addon 宿主（如非 bundle 的 node 进程）已授权后仍可能取不到该键。
    auto status = [AVCaptureDevice authorizationStatusForMediaType:type];
    if (status == AVAuthorizationStatusAuthorized) return true;
    if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
        error = std::format("请在系统设置的隐私与安全性中允许{}访问", device); return false;
    }
    // 仅在需要触发授权弹窗时检查用途说明；缺失时会触发会终止进程的 TCC 请求。
    id usage = [NSBundle.mainBundle objectForInfoDictionaryKey:usageKey];
    if (![usage isKindOfClass:NSString.class] || [usage length] == 0) {
        error = std::format("宿主应用需要在 Info.plist 声明 {} 才能访问{}", usageKey.UTF8String, device); return false;
    }
    if (status == AVAuthorizationStatusNotDetermined) {
        auto completed = std::make_shared<std::atomic_bool>(false);
        [AVCaptureDevice requestAccessForMediaType:type completionHandler:^(BOOL) { completed->store(true); }];
        constexpr NSTimeInterval PermissionEventIntervalSeconds = 0.01;
        while (!completed->load()) [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:PermissionEventIntervalSeconds]];
        status = [AVCaptureDevice authorizationStatusForMediaType:type];
    }
    if (status == AVAuthorizationStatusAuthorized) return true;
    error = std::format("请在系统设置的隐私与安全性中允许{}访问", device); return false;
}
}
