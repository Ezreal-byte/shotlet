#include "FileService.h"
#include <SDL3/SDL_filesystem.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace shotlet {
std::filesystem::path DefaultRecordingDirectory() {
    const char* folder = SDL_GetUserFolder(SDL_FOLDER_VIDEOS);
    if (!folder) return {};
    const std::string value(folder);
    return std::filesystem::path(std::u8string(value.begin(), value.end())) / "Shotlet";
}

std::filesystem::path CreateRecordingSavePath(const std::filesystem::path& directory, const char* extension, std::string& error) {
    error.clear();
    if (directory.empty()) { error = "无法获取默认视频目录"; return {}; }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = ec.message(); return {}; }
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    constexpr int MillisecondsPerSecond = 1000;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % MillisecondsPerSecond;
    std::tm local{};
#if defined(SHOTLET_PLATFORM_WINDOWS)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream name;
    name << "Recording " << std::put_time(&local, "%Y-%m-%d %H.%M.%S") << '-'
         << std::setw(3) << std::setfill('0') << milliseconds << '-' << sequence++;
    auto result = directory / (name.str() + extension);
    // 不覆盖先前录制，即使系统时钟回拨或应用重新启动也能避开已有文件。
    while (std::filesystem::exists(result, ec)) result = directory / (name.str() + '-' + std::to_string(sequence++) + extension);
    if (ec) { error = ec.message(); return {}; }
    return result;
}
}
