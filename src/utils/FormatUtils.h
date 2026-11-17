#pragma once
#include <array>
#include <cstdint>
#include <chrono>
#include <format>
#include <string>

namespace shotlet::FormatUtils {

inline std::string HoursMinutesSeconds(std::chrono::seconds duration) {
    const auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration - hours);
    const auto seconds = duration - hours - minutes;
    return std::format("{:02}:{:02}:{:02}", hours.count(), minutes.count(), seconds.count());
}

// 使用二进制进位表示容量，非字节单位保留一位小数。
inline std::string ByteSize(uint64_t bytes) {
    constexpr uint64_t bytesPerUnit = 1024;
    constexpr auto units = std::to_array<const char*>({"KB", "MB", "GB", "TB"});
    if (bytes < bytesPerUnit) return std::to_string(bytes) + " B";
    double value = double(bytes) / bytesPerUnit;
    size_t unit = 0;
    while (value >= bytesPerUnit && unit + 1 < units.size()) {
        value /= bytesPerUnit;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

} // namespace shotlet::FormatUtils
