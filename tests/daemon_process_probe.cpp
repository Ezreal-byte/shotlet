#include "platform/DaemonProcess.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include "platform/PlatformWin32.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
int ProbeEntry(int argc, char** argv) {
    std::filesystem::path output;
    int inheritedDescriptor = -1;
    bool waitForRelease = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--probe-output" && i + 1 < argc) {
            const std::string path(argv[++i]);
            output = std::filesystem::path(std::u8string(path.begin(), path.end()));
        } else if (argument.starts_with("--probe-inherited-fd=")) {
            inheritedDescriptor = std::stoi(argument.substr(21));
        } else if (argument == "--probe-wait") {
            waitForRelease = true;
        }
    }
    if (output.empty() || argv[argc] != nullptr) return 1;
    long processId = 0;
    long sessionId = 0;
    bool disconnectedStreams = true;
    bool hasTerminal = false;
    bool inheritedOpen = false;
#if defined(_WIN32)
    processId = GetCurrentProcessId();
    hasTerminal = GetConsoleWindow() != nullptr;
    for (DWORD stream : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        const HANDLE handle = GetStdHandle(stream);
        disconnectedStreams &= handle == nullptr || handle == INVALID_HANDLE_VALUE;
    }
#else
    processId = getpid();
    sessionId = getsid(0);
    struct stat nullInfo{};
    if (stat("/dev/null", &nullInfo) != 0) return 1;
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        struct stat info{};
        disconnectedStreams &= fstat(descriptor, &info) == 0 &&
            S_ISCHR(info.st_mode) && info.st_rdev == nullInfo.st_rdev;
    }
    const int terminal = open("/dev/tty", O_RDONLY);
    hasTerminal = terminal >= 0;
    if (terminal >= 0) close(terminal);
    inheritedOpen = inheritedDescriptor >= 0 && fcntl(inheritedDescriptor, F_GETFD) >= 0;
#endif
    // NUL 分隔保留空参数、换行和中文路径，不依赖 shell 引号解析。
    std::ofstream report(output, std::ios::binary);
    for (long value : {processId, sessionId, long(disconnectedStreams), long(hasTerminal), long(inheritedOpen)}) {
        report << value << '\0';
    }
    for (int i = 0; i < argc; ++i) report << argv[i] << '\0';
    report.close();
    if (!report) return 1;
    // 模拟常驻工作，验证启动父进程返回与终端关闭后仍能完成。
    if (waitForRelease) {
        auto release = output;
        release += ".release";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(release) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    auto finishedPath = output;
    finishedPath += ".finished";
    std::ofstream finished(finishedPath);
    finished << "done";
    return 37;
}

int RunProbe(int argc, char** argv) {
    // 用测试目录名称隔离实例锁，避免影响真实 Shotlet 或并行测试任务。
    std::string instanceName;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--probe-output") {
            const std::string path(argv[i + 1]);
            instanceName = std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path().filename().string();
            break;
        }
    }
    // 探针不读取操作系统语言；默认中文保留已有进程测试的确定性。
    auto preference = shotlet::LanguagePreference::ChineseSimplified;
    std::string error;
    if (!shotlet::ParseLanguageArguments(argc, argv, preference, error)) return 1;
    const auto language = preference == shotlet::LanguagePreference::English
        ? shotlet::Language::English : shotlet::Language::ChineseSimplified;
    return shotlet::RunWithDaemonProcess(ProbeEntry, argc, argv, instanceName, language);
}
} // namespace

int main(int argc, char** argv) {
    return shotlet::RunWithPlatformMainContext(RunProbe, argc, argv);
}
