#pragma once

namespace shotlet {

// 限定当前线程的临时原生对象生命周期；必须在创建它的线程上按嵌套顺序析构。
// macOS 使用 autorelease pool，其他平台为空操作，显式持有的窗口不受影响。
class PlatformAutoreleasePool {
public:
    PlatformAutoreleasePool();
    ~PlatformAutoreleasePool();
    PlatformAutoreleasePool(const PlatformAutoreleasePool&) = delete;
    PlatformAutoreleasePool& operator=(const PlatformAutoreleasePool&) = delete;

private:
    void* m_pool = nullptr;
};

using PlatformEntryPoint = int (*)(int argc, char** argv);

int RunWithPlatformMainContext(PlatformEntryPoint entryPoint, int argc, char** argv);

#if defined(_WIN32)
// 从 Unicode 命令行构造 UTF-8 参数，存储在入口函数返回前始终有效。
int RunWithWindowsCommandLine(PlatformEntryPoint entryPoint, const wchar_t* commandLine);
#endif

} // namespace shotlet
