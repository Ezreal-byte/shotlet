#include "app/Application.h"
#include "platform/PlatformMainContext.h"
#include "platform/DaemonProcess.h"
#include "utils/Logger.h"
#include <exception>
#include <stdexcept>
#include <cstdio>
#include <string_view>

namespace {

int RunApplication(int argc, char** argv) {
    try {
        shotlet::Application app;
        
        if (!app.Initialize(argc, argv)) {
            shotlet::Logger::Error("Failed to initialize application");
            return 1;
        }
        
        return app.Run();
    } catch (const std::exception& e) {
        shotlet::Logger::Error("event=app.fatal error={}", e.what());
        return 1;
    } catch (...) {
        shotlet::Logger::Error("event=app.fatal error=unknown");
        return 1;
    }
}

int Run(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--version") {
            std::printf("Shotlet %s\n", SHOTLET_VERSION);
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            std::puts("Shotlet - screenshot and annotation tool\n"
                      "Usage: shotlet [--language auto|zh-CN|en] [--theme auto|light|dark]\n"
                      "               [--log-dir PATH] [--log-level debug|info|warn|error]\n"
                      "       shotlet --version\n"
                      "Select a region, annotate it, then use Save to export a PNG.\n"
                      "Linux: X11 and a CJK font are required. Vulkan falls back to CPU rendering.\n"
                      "Set SHOTLET_RENDERER=auto|software|vulkan to choose the Linux renderer.");
            return 0;
        }
    }
    shotlet::LanguagePreference preference = shotlet::LanguagePreference::Auto;
    std::string error;
    if (!shotlet::ParseLanguageArguments(argc, argv, preference, error))
        throw std::invalid_argument(error);
    shotlet::ThemePreference theme = shotlet::ThemePreference::Auto;
    if (!shotlet::ParseThemeArguments(argc, argv, theme, error))
        throw std::invalid_argument(error);
    return shotlet::RunWithDaemonProcess(RunApplication, argc, argv, "shotlet",
                                       shotlet::ResolveLanguage(preference));
}

} // namespace

int main(int argc, char** argv) {
    int result = 1;
    try {
        result = shotlet::RunWithPlatformMainContext(Run, argc, argv);
    } catch (const std::exception& e) {
        // 平台入口的参数转换早于 Application 初始化，也需要可靠报错。
        shotlet::Logger::Error("event=app.entry_failed error={}", e.what());
    } catch (...) {
        shotlet::Logger::Error("event=app.entry_failed error=unknown");
    }
    shotlet::Logger::Shutdown();
    return result;
}
