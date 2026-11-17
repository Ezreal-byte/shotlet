#pragma once
#include <string>
#include <string_view>

namespace shotlet::StringUtils {
    // 返回有效 UTF-8 文本前若干字符的字节长度，不切断中文或其他多字节字符。
    inline size_t Utf8PrefixBytes(std::string_view text, size_t maxCharacters) {
        size_t end = 0;
        for (size_t count = 0; end < text.size() && count < maxCharacters; ++count) {
            ++end;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
        }
        return end;
    }

    // 只去掉两端的 ASCII 空白，保留正文内部的空白和 Unicode 字符。
    inline std::string Trim(std::string_view value) {
        constexpr std::string_view whitespace = " \t\r\n";
        const auto first = value.find_first_not_of(whitespace);
        if (first == std::string_view::npos)
            return {};
        return std::string(value.substr(first, value.find_last_not_of(whitespace) - first + 1));
    }
} // namespace shotlet::StringUtils
