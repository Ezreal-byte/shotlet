#include "platform/WindowsDpiAwareness.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "WindowsDpiAwareness.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include "utils/Logger.h"

namespace shotlet {

void EnsureWindowsDpiAwareness() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return;

    using SetProcessDpiAwarenessContextFunc = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto setDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFunc>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setDpiAwarenessContext &&
        setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        Logger::Info("[DPI] Awareness set to Per-Monitor V2");
        return;
    }

    auto setProcessDPIAware = reinterpret_cast<BOOL(WINAPI*)()>(
        GetProcAddress(user32, "SetProcessDPIAware"));
    if (setProcessDPIAware && setProcessDPIAware()) {
        Logger::Info("[DPI] Awareness set via SetProcessDPIAware fallback");
        return;
    }

    Logger::Warning("[DPI] Failed to set process DPI awareness, error={}", GetLastError());
}

} // namespace shotlet
