#include <gtest/gtest.h>
#include "utils/Logger.h"
#include <SDL3/SDL_log.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct CountingFormatArg {};
struct ThrowingFormatArg {};
int g_formatCount = 0;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()};
}

class LoggerTest : public ::testing::Test {
protected:
    std::filesystem::path root;

    void SetUp() override {
        shotlet::Logger::Shutdown();
        root = std::filesystem::temp_directory_path() / ("shotlet_logger_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        shotlet::Logger::SetLogDirectoryOverride(root);
        shotlet::Logger::SetLevel(shotlet::LogLevel::Info);
        shotlet::Logger::SetConsoleEnabled(false);
        shotlet::Logger::SetConsoleLevel(shotlet::LogLevel::Debug);
    }

    void TearDown() override {
        // 关闭后台刷新和文件句柄后清理，兼容 Windows 文件共享语义。
        shotlet::Logger::Shutdown();
        shotlet::Logger::ClearLogDirectoryOverride();
        shotlet::Logger::ClearConsoleOverride();
        shotlet::Logger::SetLevel(shotlet::LogLevel::Info);
        shotlet::Logger::SetConsoleLevel(shotlet::LogLevel::Debug);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

void SDLCALL HostSDLCallback(void*, int, SDL_LogPriority, const char*) {}
} // namespace

template<>
struct std::formatter<CountingFormatArg> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const CountingFormatArg&, std::format_context& ctx) const {
        ++g_formatCount;
        return std::format_to(ctx.out(), "counted");
    }
};

template<>
struct std::formatter<ThrowingFormatArg> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(const ThrowingFormatArg&, std::format_context& ctx) const {
        throw std::runtime_error("formatter failed");
        return ctx.out();
    }
};

TEST_F(LoggerTest, DisabledLevelDoesNotFormatArguments) {
    g_formatCount = 0;
    shotlet::Logger::Debug("{}", CountingFormatArg{});
    EXPECT_EQ(g_formatCount, 0);
}

TEST_F(LoggerTest, InfoLevelFiltersDebug) {
    EXPECT_FALSE(shotlet::Logger::IsEnabled(shotlet::LogLevel::Debug));
    EXPECT_TRUE(shotlet::Logger::IsEnabled(shotlet::LogLevel::Info));
}

TEST_F(LoggerTest, InitPreservesExistingRuntimeLogContents) {
    {
        std::ofstream existing(root / "runtime.log");
        existing << "existing log line\n";
    }
    ASSERT_TRUE(shotlet::Logger::Init());
    shotlet::Logger::Info("new log line");
    shotlet::Logger::Flush();
    const auto contents = ReadFile(root / "runtime.log");
    EXPECT_NE(contents.find("existing log line"), std::string::npos);
    EXPECT_NE(contents.find("new log line"), std::string::npos);
}

TEST_F(LoggerTest, InitUsesExplicitLogDirectoryWhenConfigured) {
    const auto directory = root / "host" / "logs";
    shotlet::Logger::SetLogDirectoryOverride(directory);
    ASSERT_TRUE(shotlet::Logger::Init(true));
    shotlet::Logger::Info("host-owned log line");
    shotlet::Logger::Shutdown();
    EXPECT_NE(ReadFile(directory / "runtime.log").find("host-owned log line"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "runtime.log"));
}

TEST_F(LoggerTest, InitReportsUnavailableDirectoryAndRecovers) {
    const auto blocked = root / "file";
    { std::ofstream file(blocked); file << "not a directory"; }
    shotlet::Logger::SetLogDirectoryOverride(blocked / "logs");
    ::testing::internal::CaptureStderr();
    EXPECT_FALSE(shotlet::Logger::Init(true));
    EXPECT_NO_THROW(shotlet::Logger::Error("emergency record"));
    const auto output = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("file logging disabled"), std::string::npos);
    EXPECT_NE(output.find("emergency record"), std::string::npos);
    shotlet::Logger::SetLogDirectoryOverride(root);
    ASSERT_TRUE(shotlet::Logger::Init(true));
    shotlet::Logger::Error("recovered");
    EXPECT_NE(ReadFile(root / "runtime.log").find("recovered"), std::string::npos);
}

TEST_F(LoggerTest, RecordsUtcTimeContextAndEscapesControlCharacters) {
    ASSERT_TRUE(shotlet::Logger::Init());
    const auto id = shotlet::Logger::BeginCapture();
    const auto line = std::source_location::current().line() + 1;
    shotlet::Logger::Error("external={}\n", "line1\r\nline2\033");
    const auto contents = ReadFile(root / "runtime.log");
    EXPECT_TRUE(std::regex_search(contents, std::regex(
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z ERROR module=\w+ pid=\d+ tid=\d+ capture_id=\d+)")));
    EXPECT_NE(contents.find("capture_id=" + std::to_string(id)), std::string::npos);
    EXPECT_NE(contents.find("source=test_logger.cpp:" + std::to_string(line)), std::string::npos);
    EXPECT_NE(contents.find("line1\\r\\nline2\\x1b\\n"), std::string::npos);
    EXPECT_EQ(std::count(contents.begin(), contents.end(), '\n'), 1);
    EXPECT_EQ(contents.find('\033'), std::string::npos);
}

TEST_F(LoggerTest, InitReportsUnavailableFile) {
    std::filesystem::create_directory(root / "runtime.log");
    ::testing::internal::CaptureStderr();
    EXPECT_FALSE(shotlet::Logger::Init(true));
    EXPECT_NE(::testing::internal::GetCapturedStderr().find("cannot open log file"), std::string::npos);
}

TEST_F(LoggerTest, RotationFailureDisablesFileAndReportsOnce) {
    ASSERT_TRUE(shotlet::Logger::Init(true, {512, 2, std::chrono::seconds(2)}));
    std::filesystem::create_directory(root / "runtime.2.log");
    { std::ofstream blocked(root / "runtime.2.log/block"); blocked << "block removal"; }
    ::testing::internal::CaptureStderr();
    for (int i = 0; i < 10; ++i) shotlet::Logger::Info("record={} {}", i, std::string(200, 'x'));
    shotlet::Logger::Error("rotation emergency record");
    const auto output = ::testing::internal::GetCapturedStderr();
    const auto first = output.find("log rotation failed");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(output.find("log rotation failed", first + 1), std::string::npos);
    EXPECT_NE(output.find("rotation emergency record"), std::string::npos);
    EXPECT_NE(ReadFile(root / "runtime.log").find("record=0"), std::string::npos);
}

TEST_F(LoggerTest, RotatesWithinSizeAndHistoryLimits) {
    const shotlet::LogOptions options{768, 2, std::chrono::seconds(2)};
    ASSERT_TRUE(shotlet::Logger::Init(false, options));
    for (int i = 0; i < 40; ++i) shotlet::Logger::Info("record={} {}", i, std::string(100, 'x'));
    shotlet::Logger::Info("oversized={}", std::string(2000, 'x'));
    shotlet::Logger::Shutdown();
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        ++files;
        EXPECT_LE(entry.file_size(), options.maxFileBytes);
        const auto contents = ReadFile(entry.path());
        ASSERT_FALSE(contents.empty());
        EXPECT_EQ(contents.back(), '\n');
    }
    EXPECT_EQ(files, 3);
    EXPECT_TRUE(std::filesystem::exists(root / "runtime.1.log"));
    EXPECT_TRUE(std::filesystem::exists(root / "runtime.2.log"));
    EXPECT_NE(ReadFile(root / "runtime.log").find("[truncated]"), std::string::npos);
}

TEST_F(LoggerTest, PeriodicallyFlushesWithoutAnotherLogCall) {
    ASSERT_TRUE(shotlet::Logger::Init(false, {1024 * 1024, 2, std::chrono::milliseconds(20)}));
    shotlet::Logger::Info("periodic record");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool visible = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ReadFile(root / "runtime.log").find("periodic record") != std::string::npos) {
            visible = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(visible);
}

TEST_F(LoggerTest, TruncationPreservesUtf8Characters) {
    ASSERT_TRUE(shotlet::Logger::Init(true, {512, 2, std::chrono::seconds(2)}));
    std::string message;
    for (int i = 0; i < 1000; ++i) message += "中文";
    shotlet::Logger::Error("message={}", message);
    const auto contents = ReadFile(root / "runtime.log");
    const auto start = contents.find("message=") + std::string("message=").size();
    const auto end = contents.find(" [truncated]");
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ((end - start) % 3, 0);
    EXPECT_LE(contents.size(), 512);
}

TEST_F(LoggerTest, ConcurrentRecordsRemainCompleteAndShutdownFlushes) {
    ASSERT_TRUE(shotlet::Logger::Init());
    const auto id = shotlet::Logger::BeginCapture();
    std::vector<std::thread> workers;
    for (int t = 0; t < 6; ++t) workers.emplace_back([t] {
        for (int i = 0; i < 100; ++i) shotlet::Logger::Info("record={}:{}", t, i);
    });
    for (auto& worker : workers) worker.join();
    shotlet::Logger::Shutdown();
    std::istringstream lines(ReadFile(root / "runtime.log"));
    std::set<std::string> records;
    size_t count = 0;
    std::string line;
    while (std::getline(lines, line)) {
        ++count;
        EXPECT_NE(line.find("capture_id=" + std::to_string(id)), std::string::npos);
        const auto start = line.find("record=");
        ASSERT_NE(start, std::string::npos);
        records.insert(line.substr(start));
    }
    EXPECT_EQ(records.size(), 600);
    EXPECT_EQ(count, 600);
}

TEST_F(LoggerTest, ReinitializationFlushesPreviousDirectory) {
    ASSERT_TRUE(shotlet::Logger::Init());
    shotlet::Logger::Info("first directory");
    shotlet::Logger::SetLogDirectoryOverride(root / "second");
    ASSERT_TRUE(shotlet::Logger::Init());
    shotlet::Logger::Info("second directory");
    shotlet::Logger::Shutdown();
    EXPECT_NE(ReadFile(root / "runtime.log").find("first directory"), std::string::npos);
    EXPECT_NE(ReadFile(root / "second/runtime.log").find("second directory"), std::string::npos);
}

TEST_F(LoggerTest, ChangingDirectoryFlushesOldLogsAndPreservesCaptureContextAndSdlBridge) {
    ASSERT_TRUE(shotlet::Logger::Init(true));
    const auto captureId = shotlet::Logger::BeginCapture();
    shotlet::Logger::Info("before directory change");
    const auto destination = root / std::filesystem::path(u8"运行 日志");
    bool committed = false;
    ASSERT_TRUE(shotlet::Logger::ChangeDirectory(destination, [&] {
        // 提交期间允许日志继续写入旧文件，避免持锁调用保存逻辑导致死锁。
        shotlet::Logger::Info("during commit");
        committed = true;
        return true;
    }));
    EXPECT_TRUE(committed);
    EXPECT_EQ(shotlet::Logger::GetLogPath(), destination / "runtime.log");
    const auto previous = ReadFile(root / "runtime.log");
    EXPECT_NE(previous.find("before directory change"), std::string::npos);
    EXPECT_NE(previous.find("during commit"), std::string::npos);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "after directory change");
    const auto current = ReadFile(destination / "runtime.log");
    EXPECT_NE(current.find("after directory change"), std::string::npos);
    EXPECT_NE(current.find("capture_id=" + std::to_string(captureId)), std::string::npos);
    EXPECT_EQ(current.find("before directory change"), std::string::npos);
}

TEST_F(LoggerTest, UnwritableDestinationAndFailedCommitKeepTheOriginalLogActive) {
    ASSERT_TRUE(shotlet::Logger::Init(true));
    const auto originalPath = shotlet::Logger::GetLogPath();
    const auto blocked = root / "blocked";
    { std::ofstream output(blocked); output << "not a directory"; }
    bool committed = false;
    EXPECT_FALSE(shotlet::Logger::ChangeDirectory(blocked / "logs", [&] { committed = true; return true; }));
    EXPECT_FALSE(committed);
    EXPECT_EQ(shotlet::Logger::GetLogPath(), originalPath);
    EXPECT_FALSE(shotlet::Logger::ChangeDirectory(root / "rejected", [] { return false; }));
    EXPECT_EQ(shotlet::Logger::GetLogPath(), originalPath);
    shotlet::Logger::Error("original file still writable");
    EXPECT_NE(ReadFile(originalPath).find("original file still writable"), std::string::npos);
    ASSERT_TRUE(shotlet::Logger::Init(true));
    EXPECT_EQ(shotlet::Logger::GetLogPath(), originalPath);
}

TEST_F(LoggerTest, ChangingDirectoryRecoversFromUnavailableStartupLog) {
    const auto blocked = root / "blocked";
    { std::ofstream output(blocked); output << "not a directory"; }
    shotlet::Logger::SetLogDirectoryOverride(blocked / "logs");
    ::testing::internal::CaptureStderr();
    EXPECT_FALSE(shotlet::Logger::Init(true));
    ::testing::internal::GetCapturedStderr();
    const auto destination = root / "recovered";
    ASSERT_TRUE(shotlet::Logger::ChangeDirectory(destination));
    shotlet::Logger::Info("recovered file logging");
    shotlet::Logger::Flush();
    EXPECT_NE(ReadFile(destination / "runtime.log").find("recovered file logging"), std::string::npos);
}

TEST_F(LoggerTest, ConsoleThresholdDoesNotSuppressFileRecords) {
    shotlet::Logger::SetLevel(shotlet::LogLevel::Debug);
    shotlet::Logger::SetConsoleEnabled(true);
    shotlet::Logger::SetConsoleLevel(shotlet::LogLevel::Warning);
    ASSERT_TRUE(shotlet::Logger::Init());
    ::testing::internal::CaptureStderr();
    shotlet::Logger::Debug("file-only record");
    shotlet::Logger::Warning("console record");
    shotlet::Logger::Flush();
    const auto output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(output.find("file-only record"), std::string::npos);
    EXPECT_NE(output.find("console record"), std::string::npos);
    EXPECT_EQ(output.find('\033'), std::string::npos);
    EXPECT_NE(ReadFile(root / "runtime.log").find("file-only record"), std::string::npos);
}

TEST_F(LoggerTest, EmbeddedModeDefaultsToNoConsoleOutput) {
    shotlet::Logger::ClearConsoleOverride();
    ASSERT_TRUE(shotlet::Logger::Init(true));
    ::testing::internal::CaptureStderr();
    shotlet::Logger::Error("embedded file record");
    EXPECT_TRUE(::testing::internal::GetCapturedStderr().empty());
    EXPECT_NE(ReadFile(root / "runtime.log").find("embedded file record"), std::string::npos);
}

TEST_F(LoggerTest, SdlRecordsAreWrittenOnceAndHostCallbackIsRestored) {
    SDL_LogOutputFunction previous = nullptr;
    void* previousData = nullptr;
    SDL_GetLogOutputFunction(&previous, &previousData);
    int hostData = 0;
    SDL_SetLogOutputFunction(HostSDLCallback, &hostData);
    ASSERT_TRUE(shotlet::Logger::Init(true));
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "sdl unique record");
    shotlet::Logger::Shutdown();
    const auto contents = ReadFile(root / "runtime.log");
    const auto first = contents.find("sdl unique record");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(contents.find("sdl unique record", first + 1), std::string::npos);
    EXPECT_NE(contents.find("module=sdl"), std::string::npos);
    SDL_LogOutputFunction restored = nullptr;
    void* restoredData = nullptr;
    SDL_GetLogOutputFunction(&restored, &restoredData);
    EXPECT_EQ(restored, HostSDLCallback);
    EXPECT_EQ(restoredData, &hostData);
    SDL_SetLogOutputFunction(previous, previousData);
}

TEST_F(LoggerTest, DoesNotReplaceNewHostCallbackWhenDetaching) {
    SDL_LogOutputFunction previous = nullptr;
    void* previousData = nullptr;
    SDL_GetLogOutputFunction(&previous, &previousData);
    ASSERT_TRUE(shotlet::Logger::Init(true));
    SDL_SetLogOutputFunction(HostSDLCallback, nullptr);
    shotlet::Logger::DetachSDLLogging();
    SDL_LogOutputFunction restored = nullptr;
    SDL_GetLogOutputFunction(&restored, nullptr);
    EXPECT_EQ(restored, HostSDLCallback);
    SDL_SetLogOutputFunction(previous, previousData);
}

TEST_F(LoggerTest, FormattingFailureDoesNotEscapeIntoApplication) {
    ::testing::internal::CaptureStderr();
    EXPECT_NO_THROW(shotlet::Logger::Error("{}", ThrowingFormatArg{}));
    EXPECT_NE(::testing::internal::GetCapturedStderr().find("cannot format"), std::string::npos);
}

TEST_F(LoggerTest, ParsesSupportedLevelsAndRejectsInvalidValues) {
    shotlet::LogLevel level = shotlet::LogLevel::Info;
    EXPECT_TRUE(shotlet::Logger::TryParseLevel("debug", level));
    EXPECT_EQ(level, shotlet::LogLevel::Debug);
    EXPECT_TRUE(shotlet::Logger::TryParseLevel("warning", level));
    EXPECT_EQ(level, shotlet::LogLevel::Warning);
    EXPECT_FALSE(shotlet::Logger::TryParseLevel("verbose", level));
    EXPECT_EQ(level, shotlet::LogLevel::Warning);
}
