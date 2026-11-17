#pragma once
// PlatformWin32.h - Safe Windows header inclusion
// Include this INSTEAD of directly including <windows.h> in any .cpp file.
// It handles macro pollution that conflicts with our class method names.

#if defined(SHOTLET_PLATFORM_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Undefine Windows macros that conflict with our class method/function names
#ifdef GetLastError
#undef GetLastError
#endif
#ifdef CopyFile
#undef CopyFile
#endif
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef GetObject
#undef GetObject
#endif
#ifdef DrawText
#undef DrawText
#endif
#ifdef GetClassName
#undef GetClassName
#endif
#ifdef GetClassInfo
#undef GetClassInfo
#endif
#ifdef SetCursor
#undef SetCursor
#endif
#ifdef ShowCursor
#undef ShowCursor
#endif
#ifdef GetMonitorInfo
#undef GetMonitorInfo
#endif

namespace shotlet {
// 统一应用 Win11 圆角偏好（DWMWCP_ROUND）；Win10 无此属性时调用失败，自动保持直角。
// 无边框窗口共用，避免各窗口圆角设置漂移。实现见 Appearance_win.cpp。
void ApplyWindowCornerPreference(HWND hwnd);
} // namespace shotlet

#endif // SHOTLET_PLATFORM_WINDOWS
