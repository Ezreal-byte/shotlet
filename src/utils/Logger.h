#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace shotlet {
    enum class LogLevel { Debug, Info, Warning, Error };

    struct LogOptions {
        std::size_t maxFileBytes = 10 * 1024 * 1024;
        std::size_t maxFiles = 5; // 历史文件数，另有一个当前文件
        std::chrono::milliseconds flushInterval{2000};
    };

    // 隐式转换时保留调用位置，同时继续在编译期检查格式字符串。
    template<typename... Args>
    struct LogFormat {
        std::format_string<Args...> format;
        std::source_location location;

        template<typename T>
        consteval LogFormat(const T& value,
                           std::source_location source = std::source_location::current())
            : format(value), location(source) {}
    };

    class Logger {
    public:
        // 文件不可用时返回 false，仍允许控制台和错误应急输出。
        static bool Init(bool embeddedMode = false, const LogOptions& options = {}) noexcept;
        static void Flush() noexcept;
        static void Shutdown() noexcept;
        // 归还宿主的 SDL 回调，文件日志继续可用，便于记录顶层异常。
        static void DetachSDLLogging() noexcept;
        static void SetLogDirectoryOverride(const std::filesystem::path& directory);
        static void ClearLogDirectoryOverride();
        static std::filesystem::path GetLogPath();
        // 先验证新文件，再提交配置；提交失败时保留原文件和目录。
        // commit 在文件锁外执行，可继续记日志，但不能重入日志生命周期操作。
        static bool ChangeDirectory(const std::filesystem::path& directory,
                                    const std::function<bool()>& commit = {}) noexcept;
        static void SetLevel(LogLevel level);
        static bool TryParseLevel(std::string_view value, LogLevel& level);
        static bool IsEnabled(LogLevel level);
        static void SetConsoleEnabled(bool enabled);
        static void ClearConsoleOverride();
        static void SetConsoleLevel(LogLevel level);
        // 应用一次只处理一个截图任务，工作线程共享当前任务标识。
        static uint64_t BeginCapture();

        template<typename... Args>
        static void Debug(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
            Write(LogLevel::Debug, fmt, std::forward<Args>(args)...);
        }
        template<typename... Args>
        static void Info(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
            Write(LogLevel::Info, fmt, std::forward<Args>(args)...);
        }
        template<typename... Args>
        static void Warning(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
            Write(LogLevel::Warning, fmt, std::forward<Args>(args)...);
        }
        template<typename... Args>
        static void Error(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args) noexcept {
            Write(LogLevel::Error, fmt, std::forward<Args>(args)...);
        }

    private:
        template<typename... Args>
        static void Write(LogLevel level, const LogFormat<std::type_identity_t<Args>...>& fmt,
                          Args&&... args) noexcept {
            if (!IsEnabled(level)) return;
            try {
                Log(level, std::format(fmt.format, std::forward<Args>(args)...), fmt.location);
            } catch (...) {
                ReportFormattingFailure();
            }
        }
        static void Log(LogLevel level, const std::string& message,
                        const std::source_location& source) noexcept;
        static void ReportFormattingFailure() noexcept;
        static std::atomic<LogLevel> s_currentLevel;
    };
} // namespace shotlet
