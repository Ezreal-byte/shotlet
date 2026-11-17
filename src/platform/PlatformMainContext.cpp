#include "PlatformMainContext.h"

#if defined(_WIN32)
#include "PlatformWin32.h"
#include <shellapi.h>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#endif

namespace shotlet {

PlatformAutoreleasePool::PlatformAutoreleasePool() = default;
PlatformAutoreleasePool::~PlatformAutoreleasePool() = default;

#if defined(_WIN32)
namespace {
struct CommandLineDeleter {
    void operator()(wchar_t** arguments) const noexcept { LocalFree(arguments); }
};

std::string ArgumentToUtf8(const wchar_t* argument) {
    // 以 -1 计入结尾 NUL，空参数也能正确转换；拒绝损坏的 UTF-16。
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1,
                                         nullptr, 0, nullptr, nullptr);
    if (bytes == 0) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot encode command-line argument as UTF-8");
    }
    std::string result(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1,
                            result.data(), bytes, nullptr, nullptr) == 0) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot encode command-line argument as UTF-8");
    }
    result.resize(bytes - 1);
    return result;
}
} // namespace

int RunWithWindowsCommandLine(PlatformEntryPoint entryPoint, const wchar_t* commandLine) {
    int argc = 0;
    std::unique_ptr<wchar_t*, CommandLineDeleter> wideArguments(CommandLineToArgvW(commandLine, &argc));
    if (!wideArguments) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot parse Windows command line");
    }
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) arguments.push_back(ArgumentToUtf8(wideArguments.get()[i]));
    std::vector<char*> argv;
    argv.reserve(argc + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    return entryPoint(argc, argv.data());
}
#endif

int RunWithPlatformMainContext(PlatformEntryPoint entryPoint, int argc, char** argv) {
#if defined(_WIN32)
    // mainCRTStartup 提供的窄字符 argv 使用 ANSI 代码页，不能当作 UTF-8。
    (void)argc;
    (void)argv;
    return RunWithWindowsCommandLine(entryPoint, GetCommandLineW());
#else
    return entryPoint(argc, argv);
#endif
}

} // namespace shotlet
