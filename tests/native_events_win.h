#pragma once
#include "platform/PlatformWin32.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <future>
#include <stdexcept>

namespace shotlet::TestUtils {
inline constexpr auto NativePollInterval = std::chrono::milliseconds(5);
inline constexpr auto NativeWaitTimeout = std::chrono::seconds(10);

inline void DrainWindowMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

inline void PumpFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        SDL_PumpEvents();
        SDL_Delay(static_cast<Uint32>(NativePollInterval.count()));
    }
}
template<class Predicate>
bool PumpUntil(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + NativeWaitTimeout;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        PumpFor(NativePollInterval);
    }
    return true;
}
template<class Result>
Result Await(std::future<Result>& task) {
    // 超时以异常退出测试，先由 future 回收任务，再销毁它借用的错误信息和录制会话。
    if (!PumpUntil([&] { return task.wait_for(std::chrono::seconds::zero()) == std::future_status::ready; }))
        throw std::runtime_error("等待后台测试任务超时");
    return task.get();
}
} // namespace shotlet::TestUtils
