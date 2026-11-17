#pragma once
#include "i18n/Localization.h"

#if defined(_WIN32)
#include "PlatformWin32.h"
namespace shotlet {
inline std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size);
    return result;
}
inline std::wstring TrWide(Language language, TextId id) {
    return Utf8ToWide(Tr(language, id));
}
} // namespace shotlet
#endif

#if defined(__OBJC__) && defined(__APPLE__)
#import <Foundation/Foundation.h>
namespace shotlet {
inline NSString* TrNative(Language language, TextId id) {
    return [NSString stringWithUTF8String:Tr(language, id)];
}
} // namespace shotlet
#endif
