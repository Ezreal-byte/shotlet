#include <gtest/gtest.h>
#include "platform/PlatformMainContext.h"
#include "utils/Logger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {
std::vector<std::string> g_arguments;

int CollectArguments(int argc, char** argv) {
    g_arguments.clear();
    for (int i = 0; i < argc; ++i) g_arguments.emplace_back(argv[i]);
    EXPECT_EQ(argv[argc], nullptr);
    return 42;
}

std::filesystem::path Utf8Path(const std::string& argument) {
    return std::filesystem::path(std::u8string(argument.begin(), argument.end()));
}

int WriteUnicodeLog(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--log-dir") return 1;
    shotlet::Logger::SetLogDirectoryOverride(Utf8Path(argv[2]));
    if (!shotlet::Logger::Init(true)) return 1;
    shotlet::Logger::Error("unicode path works");
    shotlet::Logger::Shutdown();
    return 0;
}
} // namespace

TEST(WindowsCommandLineTest, ChinesePathAndSpacesRoundTripToNativePath) {
    EXPECT_EQ(shotlet::RunWithWindowsCommandLine(CollectArguments,
        L"shotlet.exe --log-dir \"C:\\中文目录\\日志 文件\" --log-level=debug"), 42);
    ASSERT_EQ(g_arguments.size(), 4);
    EXPECT_EQ(g_arguments[1], "--log-dir");
    EXPECT_EQ(Utf8Path(g_arguments[2]).native(), L"C:\\中文目录\\日志 文件");
    EXPECT_EQ(g_arguments[3], "--log-level=debug");
}

TEST(WindowsCommandLineTest, EqualsFormPreservesSupplementaryUnicodeCharacters) {
    EXPECT_EQ(shotlet::RunWithWindowsCommandLine(CollectArguments,
        L"shotlet.exe \"--log-dir=C:\\截图目录\\\U0001f600 日志\""), 42);
    ASSERT_EQ(g_arguments.size(), 2);
    ASSERT_TRUE(g_arguments[1].starts_with("--log-dir="));
    EXPECT_EQ(Utf8Path(g_arguments[1].substr(10)).native(), L"C:\\截图目录\\\U0001f600 日志");
}

TEST(WindowsCommandLineTest, EmptyArgumentAndQuotedTrailingBackslashArePreserved) {
    EXPECT_EQ(shotlet::RunWithWindowsCommandLine(CollectArguments,
        L"shotlet.exe \"\" \"C:\\日志 文件\\\\\""), 42);
    ASSERT_EQ(g_arguments.size(), 3);
    EXPECT_TRUE(g_arguments[1].empty());
    EXPECT_EQ(Utf8Path(g_arguments[2]).native(), L"C:\\日志 文件\\");
}

TEST(WindowsCommandLineTest, InvalidUtf16FailsBeforeCallingApplication) {
    g_arguments.clear();
    std::wstring command = L"shotlet.exe --log-dir ";
    command.push_back(static_cast<wchar_t>(0xd800));
    EXPECT_THROW(shotlet::RunWithWindowsCommandLine(CollectArguments, command.c_str()), std::system_error);
    EXPECT_TRUE(g_arguments.empty());
}

TEST(WindowsCommandLineTest, LoggerCreatesFileInExactUnicodeDirectory) {
    const auto root = std::filesystem::temp_directory_path() / ("shotlet_unicode_log_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = root / std::filesystem::path(L"中文 日志\U0001f600");
    const auto command = L"shotlet.exe --log-dir \"" + directory.native() + L"\"";
    EXPECT_EQ(shotlet::RunWithWindowsCommandLine(WriteUnicodeLog, command.c_str()), 0);
    shotlet::Logger::Shutdown();
    shotlet::Logger::ClearLogDirectoryOverride();
    std::ifstream log(directory / "runtime.log", std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_NE(contents.find("unicode path works"), std::string::npos);
    log.close();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
