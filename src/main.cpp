#include "app/Application.h"
#include "platform/PlatformMainContext.h"
#include "platform/DaemonProcess.h"
#include "utils/Logger.h"
#include <exception>
#include <stdexcept>

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
