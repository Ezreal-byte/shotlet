#include "Logger.h"
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#if defined(SHOTLET_PLATFORM_WINDOWS)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace shotlet {
    std::atomic<LogLevel> Logger::s_currentLevel{LogLevel::Info};

    namespace {
        std::atomic<uint64_t> s_captureSequence{0};
        std::atomic<uint64_t> s_captureId{0};
        constexpr const char* RuntimeLogFilename = "runtime.log";

        const char* LevelName(LogLevel level) {
            switch (level) {
                case LogLevel::Debug: return "DEBUG";
                case LogLevel::Info: return "INFO";
                case LogLevel::Warning: return "WARN";
                case LogLevel::Error: return "ERROR";
            }
            return "UNKNOWN";
        }

        // 保持单条记录只有一行，转义外部错误文本中的控制字符。
        std::string Sanitize(std::string_view message) {
            std::string clean;
            auto end = std::min(message.size(), size_t{64 * 1024});
            while (end > 0 && end < message.size() &&
                   (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80) --end;
            for (unsigned char ch : message.substr(0, end)) {
                if (ch == '\n') clean += "\\n";
                else if (ch == '\r') clean += "\\r";
                else if (ch == '\t') clean += "\\t";
                else if (ch < 0x20 || ch == 0x7f) clean += std::format("\\x{:02x}", ch);
                else clean += static_cast<char>(ch);
            }
            if (message.size() > 64 * 1024) clean += " [truncated]";
            return clean;
        }

        std::filesystem::path DefaultDirectory() {
#if defined(SHOTLET_PLATFORM_MACOS)
            if (const char* home = std::getenv("HOME"))
                return std::filesystem::path(home) / "Library" / "Logs" / "Shotlet";
#elif defined(SHOTLET_PLATFORM_WINDOWS)
            if (const char* dir = std::getenv("LOCALAPPDATA"))
                return std::filesystem::path(dir) / "Shotlet" / "logs";
            if (const char* dir = std::getenv("APPDATA"))
                return std::filesystem::path(dir) / "Shotlet" / "logs";
#else
            if (const char* dir = std::getenv("XDG_STATE_HOME"); dir && *dir)
                return std::filesystem::path(dir) / "shotlet";
            if (const char* home = std::getenv("HOME"))
                return std::filesystem::path(home) / ".local" / "state" / "shotlet";
#endif
            std::error_code ec;
            auto temp = std::filesystem::temp_directory_path(ec);
            return ec ? std::filesystem::path{} : temp / "shotlet";
        }

        struct LogState {
            std::mutex mutex;
            std::mutex lifecycleMutex;
            std::condition_variable wakeup;
            std::thread worker;
            bool stopping = false;
            std::ofstream file;
            std::filesystem::path directoryOverride;
            bool hasOverride = false;
            std::filesystem::path logPath;
            size_t fileBytes = 0;
            LogOptions options;
            std::optional<bool> consoleOverride;
            bool consoleEnabled = true;
            LogLevel consoleLevel = LogLevel::Debug;
            bool failureReported = false;

            ~LogState() {
                Logger::DetachSDLLogging();
                StopWorker();
            }

            void DisableFile(const char* reason) {
                if (file.is_open()) file.close();
                if (!failureReported) {
                    failureReported = true;
                    std::fprintf(stderr, "[Shotlet logger] %s; file logging disabled\n", reason);
                }
            }

            void FlushFile() {
                if (!file.is_open()) return;
                file.flush();
                if (!file) DisableFile("log flush failed");
            }

            void StopWorker() {
                {
                    std::lock_guard lock(mutex);
                    stopping = true;
                }
                wakeup.notify_all();
                if (worker.joinable()) worker.join();
                std::lock_guard lock(mutex);
                FlushFile();
            }

            void Run() {
                std::unique_lock lock(mutex);
                while (!wakeup.wait_for(lock, options.flushInterval, [this] { return stopping; })) {
                    FlushFile();
                }
            }

            std::filesystem::path HistoryPath(size_t index) const {
                return logPath.parent_path() / ("runtime." + std::to_string(index) + ".log");
            }

            bool Rotate() {
                FlushFile();
                if (!file.is_open()) return false;
                file.close();
                std::error_code ec;
                std::filesystem::remove(HistoryPath(options.maxFiles), ec);
                if (!ec) {
                    for (size_t i = options.maxFiles; i > 0; --i) {
                        const auto from = i == 1 ? logPath : HistoryPath(i - 1);
                        const bool exists = std::filesystem::exists(from, ec);
                        if (ec) break;
                        if (exists) std::filesystem::rename(from, HistoryPath(i), ec);
                        if (ec) break;
                    }
                }
                if (ec) {
                    DisableFile("log rotation failed");
                    return false;
                }
                file.clear();
                file.open(logPath, std::ios::out | std::ios::binary | std::ios::app);
                fileBytes = 0;
                if (!file) DisableFile("cannot open rotated log file");
                return file.is_open();
            }
        };

        LogState& State() {
            static LogState state;
            return state;
        }

        // SDL 的锁与日志文件锁独立，安装/归还回调时不持有文件锁。
        std::mutex s_sdlBridgeMutex;
        SDL_LogOutputFunction s_previousOutput = nullptr;
        void* s_previousUserdata = nullptr;
        bool s_bridgeInstalled = false;

        void SDLCALL SDLLogOutput(void*, int category, SDL_LogPriority priority, const char* message) {
            const char* text = message ? message : "";
            if (priority >= SDL_LOG_PRIORITY_ERROR) Logger::Error("[SDL] category={} {}", category, text);
            else if (priority >= SDL_LOG_PRIORITY_WARN) Logger::Warning("[SDL] category={} {}", category, text);
            else if (priority >= SDL_LOG_PRIORITY_INFO) Logger::Info("[SDL] category={} {}", category, text);
            else Logger::Debug("[SDL] category={} {}", category, text);
        }

        void AttachSDLLogging() {
            std::lock_guard lock(s_sdlBridgeMutex);
            if (s_bridgeInstalled) return;
            SDL_GetLogOutputFunction(&s_previousOutput, &s_previousUserdata);
            SDL_SetLogOutputFunction(SDLLogOutput, nullptr);
            s_bridgeInstalled = true;
        }

        bool UseColor() {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            return false; // 不向未确认支持 ANSI 的 Windows 控制台写入转义序列
#else
            const char* term = std::getenv("TERM");
            return isatty(fileno(stderr)) && !std::getenv("NO_COLOR") &&
                   (!term || std::string_view(term) != "dumb");
#endif
        }

        std::string MakeLine(LogLevel level, std::string_view message, const std::source_location& source) {
            const auto now = std::chrono::system_clock::now();
            const auto seconds = std::chrono::system_clock::to_time_t(now);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            std::tm utc{};
#if defined(SHOTLET_PLATFORM_WINDOWS)
            gmtime_s(&utc, &seconds);
            const auto pid = _getpid();
#else
            gmtime_r(&seconds, &utc);
            const auto pid = getpid();
#endif
            std::string_view path = source.file_name();
            const auto src = path.rfind("src/");
            const auto winSrc = path.rfind("src\\");
            const auto start = src != path.npos ? src : winSrc;
            std::string_view module = "app";
            if (start != path.npos) {
                const auto relative = path.substr(start + 4);
                const auto end = relative.find_first_of("/\\");
                if (end != relative.npos) module = relative.substr(0, end);
            }
            if (message.starts_with("[SDL]")) module = "sdl";
            const auto basename = path.substr(path.find_last_of("/\\") + 1);
            std::ostringstream line;
            line.imbue(std::locale::classic());
            line << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
                 << std::setfill('0') << std::setw(3) << ms.count() << "Z " << LevelName(level)
                 << " module=" << module << " pid=" << pid
                 << " tid=" << std::hash<std::thread::id>{}(std::this_thread::get_id())
                 << " capture_id=" << s_captureId.load(std::memory_order_relaxed);
            if (level == LogLevel::Debug || level == LogLevel::Error)
                line << " source=" << basename << ':' << source.line();
            line << ' ' << Sanitize(message);
            return line.str();
        }
    } // namespace

    bool Logger::Init(bool embeddedMode, const LogOptions& options) noexcept {
        try {
            auto& state = State();
            std::lock_guard lifecycleLock(state.lifecycleMutex);
            state.StopWorker();
            bool available = false;
            {
                std::lock_guard lock(state.mutex);
                if (state.file.is_open()) state.file.close();
                state.file.clear();
                state.failureReported = false;
                state.consoleEnabled = state.consoleOverride.value_or(!embeddedMode);
                state.options = options;
                state.fileBytes = 0;
                s_captureId.store(0, std::memory_order_relaxed);
                if (options.maxFileBytes < 256 || options.maxFiles == 0 || options.flushInterval.count() <= 0) {
                    state.options = {};
                    state.DisableFile("invalid log options");
                    return false;
                }
                const auto directory = state.hasOverride ? state.directoryOverride : DefaultDirectory();
                state.logPath = directory.empty() ? std::filesystem::path{} : directory / RuntimeLogFilename;
                std::error_code ec;
                if (!directory.empty()) std::filesystem::create_directories(directory, ec);
                if (directory.empty() || ec) {
                    state.DisableFile("cannot create log directory");
                } else {
                    state.file.open(state.logPath, std::ios::out | std::ios::binary | std::ios::app);
                    if (!state.file) state.DisableFile("cannot open log file");
                    else {
                        state.fileBytes = std::filesystem::file_size(state.logPath, ec);
                        if (ec) state.DisableFile("cannot determine log file size");
                    }
                }
                available = state.file.is_open();
                state.stopping = false;
            }
            if (available) state.worker = std::thread([&state] { state.Run(); });
            AttachSDLLogging();
            return available;
        } catch (...) {
            std::fputs("[Shotlet logger] initialization failed\n", stderr);
            return false;
        }
    }

    void Logger::Flush() noexcept {
        try {
            auto& state = State();
            std::lock_guard lock(state.mutex);
            state.FlushFile();
            std::fflush(stderr);
        } catch (...) {
            std::fputs("[Shotlet logger] flush failed\n", stderr);
        }
    }

    void Logger::DetachSDLLogging() noexcept {
        std::lock_guard lock(s_sdlBridgeMutex);
        if (!s_bridgeInstalled) return;
        SDL_LogOutputFunction current = nullptr;
        void* userdata = nullptr;
        SDL_GetLogOutputFunction(&current, &userdata);
        // 宿主已接管输出时，不覆盖其新回调。
        if (current == SDLLogOutput) SDL_SetLogOutputFunction(s_previousOutput, s_previousUserdata);
        s_bridgeInstalled = false;
    }

    void Logger::Shutdown() noexcept {
        try {
            auto& state = State();
            std::lock_guard lifecycleLock(state.lifecycleMutex);
            DetachSDLLogging();
            state.StopWorker();
            std::lock_guard lock(state.mutex);
            if (state.file.is_open()) state.file.close();
            s_captureId.store(0, std::memory_order_relaxed);
        } catch (...) {
            std::fputs("[Shotlet logger] shutdown failed\n", stderr);
        }
    }

    void Logger::SetLogDirectoryOverride(const std::filesystem::path& directory) {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        state.directoryOverride = directory;
        state.hasOverride = true;
    }

    void Logger::ClearLogDirectoryOverride() {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        state.directoryOverride.clear();
        state.hasOverride = false;
    }

    std::filesystem::path Logger::GetLogPath() {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        return state.logPath;
    }

    bool Logger::ChangeDirectory(const std::filesystem::path& directory,
                                 const std::function<bool()>& commit) noexcept {
        try {
            auto& state = State();
            std::lock_guard lifecycleLock(state.lifecycleMutex);
            auto nextDirectory = directory.empty() ? DefaultDirectory() : directory;
            if (nextDirectory.empty()) return false;
            nextDirectory = std::filesystem::absolute(nextDirectory);
            auto nextPath = nextDirectory / RuntimeLogFilename;
            std::filesystem::create_directories(nextDirectory);
            std::ofstream nextFile(nextPath, std::ios::out | std::ios::binary | std::ios::app);
            if (!nextFile) return false;
            nextFile.seekp(0, std::ios::end);
            const auto nextFileBytes = nextFile.tellp();
            if (!nextFile || nextFileBytes < 0) return false;
            bool sameFile = false;
            {
                std::lock_guard lock(state.mutex);
                std::error_code ec;
                sameFile = std::filesystem::equivalent(nextPath, state.logPath, ec);
                // 启动时文件不可用可能没有刷新线程；更改目录后也应恢复定时刷新。
                if (!state.worker.joinable()) {
                    state.stopping = false;
                    state.worker = std::thread([&state] { state.Run(); });
                }
            }
            // 不持有文件锁：保存期间后台线程仍可写入旧文件，失败无需重新打开旧文件。
            if (commit && !commit()) return false;
            std::lock_guard lock(state.mutex);
            state.FlushFile();
            // 重选当前目录时，计入保存配置期间仍写入旧流的日志。
            const auto fileBytes = sameFile ? state.fileBytes : static_cast<size_t>(nextFileBytes);
            state.file.swap(nextFile);
            state.logPath.swap(nextPath);
            state.directoryOverride.swap(nextDirectory);
            state.hasOverride = true;
            state.fileBytes = fileBytes;
            state.failureReported = false;
            return true;
        } catch (...) {
            return false;
        }
    }

    void Logger::SetLevel(LogLevel level) { s_currentLevel.store(level, std::memory_order_relaxed); }

    bool Logger::TryParseLevel(std::string_view value, LogLevel& level) {
        if (value == "debug") level = LogLevel::Debug;
        else if (value == "info") level = LogLevel::Info;
        else if (value == "warn" || value == "warning") level = LogLevel::Warning;
        else if (value == "error") level = LogLevel::Error;
        else return false;
        return true;
    }

    bool Logger::IsEnabled(LogLevel level) { return level >= s_currentLevel.load(std::memory_order_relaxed); }

    void Logger::SetConsoleEnabled(bool enabled) {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        state.consoleOverride = enabled;
        state.consoleEnabled = enabled;
    }

    void Logger::SetConsoleLevel(LogLevel level) {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        state.consoleLevel = level;
    }

    void Logger::ClearConsoleOverride() {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        state.consoleOverride.reset();
    }

    uint64_t Logger::BeginCapture() {
        const auto id = s_captureSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        s_captureId.store(id, std::memory_order_relaxed);
        return id;
    }

    void Logger::Log(LogLevel level, const std::string& message, const std::source_location& source) noexcept {
        try {
            if (!IsEnabled(level)) return;
            // 时间与消息格式化不占用文件锁；写入与轮转仍串行，保证记录完整。
            auto line = MakeLine(level, message, source);
            auto& state = State();
            std::lock_guard lock(state.mutex);
            if (line.size() + 1 > state.options.maxFileBytes) {
                auto end = state.options.maxFileBytes - 13;
                // 不截断 UTF-8 多字节字符；避免轮转文件出现无效编码。
                while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) --end;
                line.resize(end);
                line += " [truncated]";
            }
            if (state.file.is_open()) {
                const auto bytes = line.size() + 1;
                if (state.fileBytes > 0 && (state.fileBytes >= state.options.maxFileBytes ||
                    bytes > state.options.maxFileBytes - state.fileBytes)) state.Rotate();
                if (state.file.is_open()) {
                    state.file << line << '\n';
                    if (!state.file) state.DisableFile("log write failed");
                    else state.fileBytes += bytes;
                    if (level == LogLevel::Error) state.FlushFile();
                }
            }
            const bool console = state.consoleEnabled && level >= state.consoleLevel;
            if (console || (!state.file.is_open() && level == LogLevel::Error)) {
                if (console && UseColor()) {
                    const char* color = level == LogLevel::Error ? "\033[31m" :
                        level == LogLevel::Warning ? "\033[33m" :
                        level == LogLevel::Debug ? "\033[36m" : "\033[32m";
                    std::fputs(color, stderr);
                    std::fwrite(line.data(), 1, line.size(), stderr);
                    std::fputs("\033[0m\n", stderr);
                } else {
                    std::fwrite(line.data(), 1, line.size(), stderr);
                    std::fputc('\n', stderr);
                }
                if (level == LogLevel::Error) std::fflush(stderr);
            }
        } catch (...) {
            ReportFormattingFailure();
        }
    }

    void Logger::ReportFormattingFailure() noexcept {
        std::fputs("[Shotlet logger] cannot format or write log record\n", stderr);
    }
} // namespace shotlet
