#include "DaemonProcess.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include "PlatformWin32.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#else
extern char** environ;
#endif
#endif

namespace shotlet {
namespace {
constexpr std::string_view ChildArgument = "--shotlet-daemon-child";
constexpr std::string_view LockArgument = "--shotlet-daemon-lock=";

void PrintAlreadyRunning(Language language) {
#if defined(_WIN32)
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        // 控制台使用 Unicode 输出，不依赖代码页；管道和文件仍输出 UTF-8。
        const char* utf8 = Tr(language, TextId::DaemonAlreadyRunning);
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (length <= 0) {
            std::fputs(utf8, stdout);
            return;
        }
        std::wstring message(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, message.data(), length);
        DWORD written = 0;
        if (WriteConsoleW(output, message.data(), static_cast<DWORD>(message.size() - 1),
                          &written, nullptr)) {
            return;
        }
    }
#endif
    std::fputs(Tr(language, TextId::DaemonAlreadyRunning), stdout);
}

// 父进程创建时便持有锁，子进程继承同一个内核对象；交接期间没有重新抢锁的空隙。
class DaemonInstance {
public:
    DaemonInstance(std::string_view name, uintptr_t inherited) {
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
                                != std::string_view::npos) {
            throw std::invalid_argument("Invalid daemon instance name");
        }
#if defined(_WIN32)
        if (inherited) {
            handle = reinterpret_cast<HANDLE>(inherited);
            DWORD flags = 0;
            if (!GetHandleInformation(handle, &flags) ||
                !SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
                const DWORD error = GetLastError();
                CloseHandle(handle);
                handle = nullptr;
                throw std::system_error(error, std::system_category(), "Invalid inherited daemon lock");
            }
            acquired = true;
            return;
        }
        std::wstring mutexName = L"Local\\Shotlet.Daemon.";
        mutexName.append(name.begin(), name.end());
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        // 以命名对象的存在表示占用，无需跨线程转移互斥体所有权。
        handle = CreateMutexW(&security, FALSE, mutexName.c_str());
        const DWORD error = GetLastError();
        if (!handle) throw std::system_error(error, std::system_category(), "Cannot create daemon instance lock");
        acquired = error != ERROR_ALREADY_EXISTS;
#else
        const std::string directory = "/tmp/shotlet-daemon-" + std::to_string(getuid());
        if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) {
            throw std::system_error(errno, std::generic_category(), "Cannot create daemon lock directory");
        }
        const int directoryFd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directoryFd < 0) throw std::system_error(errno, std::generic_category(), "Cannot open daemon lock directory");
        struct stat directoryInfo{};
        const bool safeDirectory = fstat(directoryFd, &directoryInfo) == 0 &&
            directoryInfo.st_uid == getuid() && (directoryInfo.st_mode & 0077) == 0;
        const std::string file = std::string(name) + ".lock";
        descriptor = safeDirectory ? openat(directoryFd, file.c_str(),
            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
        const int error = safeDirectory ? errno : EACCES;
        close(directoryFd);
        if (descriptor < 0) throw std::system_error(error, std::generic_category(), "Cannot open daemon instance lock");
        // 从没有标准输入输出的环境启动时，锁不能占用即将重定向的 0、1、2。
        if (descriptor < 3) {
            const int original = descriptor;
            descriptor = fcntl(original, F_DUPFD_CLOEXEC, 3);
            const int duplicateError = errno;
            close(original);
            if (descriptor < 0) {
                throw std::system_error(duplicateError, std::generic_category(), "Cannot preserve daemon instance lock");
            }
        }
        struct stat info{};
        if (fstat(descriptor, &info) < 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
            info.st_nlink != 1 || (info.st_mode & 0077) != 0) {
            close(descriptor);
            descriptor = -1;
            throw std::runtime_error("Invalid daemon instance lock file");
        }
        if (inherited) {
            struct stat inheritedInfo{};
            const int inheritedFd = static_cast<int>(inherited);
            const bool valid = inheritedFd >= 3 && uintptr_t(inheritedFd) == inherited &&
                fstat(inheritedFd, &inheritedInfo) == 0 && inheritedInfo.st_dev == info.st_dev &&
                inheritedInfo.st_ino == info.st_ino;
            close(descriptor);
            descriptor = -1;
            if (!valid) throw std::runtime_error("Invalid inherited daemon instance lock");
            descriptor = inheritedFd;
        }
        if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
            acquired = true;
        } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
            const int lockError = errno;
            close(descriptor);
            descriptor = -1;
            throw std::system_error(lockError, std::generic_category(), "Cannot acquire daemon instance lock");
        }
        if (inherited && fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
            const int flagError = errno;
            close(descriptor);
            descriptor = -1;
            throw std::system_error(flagError, std::generic_category(), "Cannot protect daemon instance lock");
        }
#endif
    }
    ~DaemonInstance() {
        // 只关闭引用，不主动解锁：父进程退出时子进程仍持有同一个锁。
#if defined(_WIN32)
        if (handle) CloseHandle(handle);
#else
        if (descriptor >= 0) close(descriptor);
        // 不删除锁文件，避免新旧进程锁定不同 inode 而同时运行。
#endif
    }
    DaemonInstance(const DaemonInstance&) = delete;
    DaemonInstance& operator=(const DaemonInstance&) = delete;
    bool acquired = false;
#if defined(_WIN32)
    HANDLE handle = nullptr;
    uintptr_t NativeLock() const { return reinterpret_cast<uintptr_t>(handle); }
#else
    int descriptor = -1;
    uintptr_t NativeLock() const { return static_cast<uintptr_t>(descriptor); }
#endif
};

#if defined(_WIN32)
std::wstring WideArgument(const char* argument) {
    if (!*argument) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument, -1, nullptr, 0);
    if (!size) throw std::system_error(GetLastError(), std::system_category(), "Cannot decode daemon argument");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument, -1, result.data(), size)) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot decode daemon argument");
    }
    result.resize(size - 1);
    return result;
}

std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(ch == L'\"' ? 2 * backslashes + 1 : backslashes, L'\\');
        result.push_back(ch);
        backslashes = 0;
    }
    result.append(2 * backslashes, L'\\');
    result.push_back(L'\"');
    return result;
}

struct ProcessAttributes {
    std::vector<char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ProcessAttributes() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
            throw std::system_error(GetLastError(), std::system_category(), "Cannot initialize process attributes");
        }
        value = attributes;
    }
    ~ProcessAttributes() { if (value) DeleteProcThreadAttributeList(value); }
};

void StartDetachedProcess(int argc, char** argv, uintptr_t nativeLock) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot locate Shotlet executable");
    }
    // 按已解析的参数重新转义；原始命令行中未闭合的引号不能吞掉内部标记，
    // 否则子进程可能再次启动自身。空参数、中文和结尾反斜杠也必须保留。
    std::wstring commandLine = QuoteArgument(std::wstring(path.data(), length));
    for (int i = 1; i < argc; ++i) commandLine += L" " + QuoteArgument(WideArgument(argv[i]));
    commandLine += L" " + QuoteArgument(L"--shotlet-daemon-child");
    commandLine += L" " + QuoteArgument(WideArgument((std::string(LockArgument) + std::to_string(nativeLock)).c_str()));
    ProcessAttributes attributes;
    HANDLE lockHandle = reinterpret_cast<HANDLE>(nativeLock);
    // 仅继承实例锁，避免其他文件、管道或终端句柄泄漏到常驻进程。
    if (!UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &lockHandle, sizeof(lockHandle), nullptr, nullptr)) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot inherit daemon instance lock");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.data(), commandLine.data(), nullptr, nullptr, TRUE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &startup.StartupInfo, &process)) {
        throw std::system_error(GetLastError(), std::system_category(), "Cannot start Shotlet daemon");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
#else
void CheckSpawnResult(int error, const char* operation) {
    if (error) throw std::system_error(error, std::generic_category(), operation);
}

std::string ExecutablePath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("Cannot locate Shotlet executable");
    }
    return path.data();
#else
    std::vector<char> path(4096);
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size());
    if (length < 0) throw std::system_error(errno, std::generic_category(), "Cannot locate Shotlet executable");
    if (static_cast<size_t>(length) == path.size()) throw std::runtime_error("Shotlet executable path is too long");
    return std::string(path.data(), static_cast<size_t>(length));
#endif
}

// 初始化失败时也要释放已创建的 spawn 配置。
struct SpawnAttributes {
    posix_spawnattr_t value{};
    SpawnAttributes() { CheckSpawnResult(posix_spawnattr_init(&value), "Cannot initialize spawn attributes"); }
    ~SpawnAttributes() { posix_spawnattr_destroy(&value); }
};
struct SpawnFileActions {
    posix_spawn_file_actions_t value{};
    SpawnFileActions() { CheckSpawnResult(posix_spawn_file_actions_init(&value), "Cannot initialize spawn file actions"); }
    ~SpawnFileActions() { posix_spawn_file_actions_destroy(&value); }
};

void StartDetachedProcess(int argc, char** argv, uintptr_t nativeLock) {
    std::string executable = ExecutablePath();
    std::vector<char*> arguments{executable.data()};
    for (int i = 1; i < argc; ++i) arguments.push_back(argv[i]);
    std::string childArgument(ChildArgument);
    arguments.push_back(childArgument.data());
    std::string lockArgument = std::string(LockArgument) + std::to_string(nativeLock);
    arguments.push_back(lockArgument.data());
    arguments.push_back(nullptr);

    SpawnAttributes attributes;
    SpawnFileActions files;
    // 创建新会话，使关闭终端或终端的 Ctrl+C 不再影响常驻进程。
    short flags = POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
    CheckSpawnResult(posix_spawn_file_actions_addinherit_np(&files.value, static_cast<int>(nativeLock)),
                     "Cannot inherit daemon instance lock");
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
    for (int descriptor = 3; descriptor < static_cast<int>(nativeLock); ++descriptor) {
        CheckSpawnResult(posix_spawn_file_actions_addclose(&files.value, descriptor), "Cannot close inherited descriptor");
    }
    CheckSpawnResult(posix_spawn_file_actions_addclosefrom_np(&files.value, static_cast<int>(nativeLock) + 1),
                     "Cannot close inherited descriptors");
#endif
#endif
    // dup2 到自身会清除子进程中的 FD_CLOEXEC，父进程仍保留该标志。
    CheckSpawnResult(posix_spawn_file_actions_adddup2(&files.value, static_cast<int>(nativeLock),
                                                     static_cast<int>(nativeLock)),
                     "Cannot preserve daemon instance lock");
    CheckSpawnResult(posix_spawnattr_setflags(&attributes.value, flags), "Cannot detach daemon session");
    sigset_t mask;
    sigemptyset(&mask);
    CheckSpawnResult(posix_spawnattr_setsigmask(&attributes.value, &mask), "Cannot reset daemon signal mask");
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);
    CheckSpawnResult(posix_spawnattr_setsigdefault(&attributes.value, &mask), "Cannot reset daemon signal handlers");
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        CheckSpawnResult(posix_spawn_file_actions_addopen(&files.value, descriptor, "/dev/null",
                                                        descriptor == 0 ? O_RDONLY : O_WRONLY, 0),
                         "Cannot disconnect daemon terminal streams");
    }
    pid_t child = 0;
#if defined(__APPLE__)
    char** environment = *_NSGetEnviron();
#else
    char** environment = environ;
#endif
    CheckSpawnResult(posix_spawn(&child, executable.c_str(), &files.value, &attributes.value,
                                arguments.data(), environment), "Cannot start Shotlet daemon");
}
#endif
} // namespace

int RunWithDaemonProcess(PlatformEntryPoint entryPoint, int argc, char** argv, std::string_view instanceName, Language language) {
    bool daemon = false;
    bool foreground = false;
    bool child = false;
    uintptr_t inheritedLock = 0;
    std::vector<char*> arguments;
    arguments.reserve(static_cast<size_t>(argc) + 1);
    for (int i = 0; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (i > 0 && argument == ChildArgument) {
            if (child) throw std::invalid_argument("Duplicate daemon child argument");
            child = true;
            continue;
        }
        if (i > 0 && argument.starts_with(LockArgument)) {
            if (inheritedLock) throw std::invalid_argument("Duplicate daemon lock argument");
            const auto value = argument.substr(LockArgument.size());
            const auto result = std::from_chars(value.data(), value.data() + value.size(), inheritedLock);
            if (inheritedLock == 0 || result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
                throw std::invalid_argument("Invalid daemon lock argument");
            }
            continue;
        }
        if (i > 0 && (argument == "--daemon" || argument == "-d")) daemon = true;
        if (i > 0 && argument == "--foreground") foreground = true;
        arguments.push_back(argv[i]);
    }
    if (child != (inheritedLock != 0) || (child && (!daemon || foreground))) {
        throw std::invalid_argument("Invalid daemon child invocation");
    }
    const int forwardedCount = static_cast<int>(arguments.size());
    arguments.push_back(nullptr);
    if (daemon) {
        DaemonInstance instance(instanceName, inheritedLock);
        if (!instance.acquired) {
            PrintAlreadyRunning(language);
            return 0;
        }
        if (!foreground && !child) {
            // 重新执行程序，让 SDL/AppKit 在新进程的主线程上从头初始化。
            StartDetachedProcess(argc, argv, instance.NativeLock());
            return 0;
        }
        return entryPoint(forwardedCount, arguments.data());
    }
    return entryPoint(forwardedCount, arguments.data());
}

} // namespace shotlet
