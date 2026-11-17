#pragma once
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <future>

namespace shotlet::TestUtils {
inline constexpr auto NativePollInterval = std::chrono::milliseconds(5);
inline constexpr NSTimeInterval NativeEventIntervalSeconds = 0.005;
inline constexpr NSTimeInterval NativeWaitTimeoutSeconds = 5;
inline void PumpEvents() {
    @autoreleasepool {
        SDL_PumpEvents();
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:NativeEventIntervalSeconds]];
    }
}
template<class Result> Result Await(std::future<Result>& task) {
    while (task.wait_for(NativePollInterval) != std::future_status::ready) PumpEvents();
    return task.get();
}
inline void PumpFor(NSTimeInterval seconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) PumpEvents();
}
template<class Predicate> bool PumpUntil(Predicate ready, NSTimeInterval seconds = NativeWaitTimeoutSeconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        PumpEvents();
    }
    return true;
}
}
