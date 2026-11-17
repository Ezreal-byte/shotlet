#pragma once
#include <filesystem>
#include <string>

namespace shotlet::FilePathUtils {
// filesystem::path::string 在 Windows 使用本地代码页；界面和日志统一使用 UTF-8。
inline std::string ToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
}
