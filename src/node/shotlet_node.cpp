#include "app/Application.h"
#include "utils/Logger.h"

#include <node_api.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(SHOTLET_PLATFORM_MACOS)
#import <Foundation/Foundation.h>
#endif

#if defined(SHOTLET_PLATFORM_WINDOWS)
#include <winrt/base.h>
#endif

namespace {

void Throw(napi_env env, const char* message) {
    napi_throw_error(env, nullptr, message);
}

napi_value CreateInt32(napi_env env, int value) {
    napi_value result = nullptr;
    napi_create_int32(env, value, &result);
    return result;
}

napi_value CreateDouble(napi_env env, double value) {
    napi_value result = nullptr;
    napi_create_double(env, value, &result);
    return result;
}

napi_value CreateString(napi_env env, const char* value) {
    napi_value result = nullptr;
    napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &result);
    return result;
}

napi_value CreateString(napi_env env, const std::string& value) {
    napi_value result = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &result);
    return result;
}

void SetNamed(napi_env env, napi_value object, const char* name, napi_value value) {
    napi_set_named_property(env, object, name, value);
}

bool ReadStringProperty(napi_env env,
                        napi_value object,
                        const char* name,
                        std::string& value) {
    bool hasProperty = false;
    napi_has_named_property(env, object, name, &hasProperty);
    if (!hasProperty) {
        return false;
    }

    napi_value property = nullptr;
    napi_get_named_property(env, object, name, &property);

    napi_valuetype type = napi_undefined;
    napi_typeof(env, property, &type);
    if (type != napi_string) {
        return false;
    }

    size_t length = 0;
    napi_get_value_string_utf8(env, property, nullptr, 0, &length);
    std::vector<char> buffer(length + 1, '\0');
    napi_get_value_string_utf8(env, property, buffer.data(), buffer.size(), &length);
    value.assign(buffer.data(), length);
    return true;
}

std::filesystem::path PathFromUtf8(std::string_view value) {
    std::u8string utf8Path;
    utf8Path.reserve(value.size());
    for (char ch : value) {
        utf8Path.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(utf8Path);
}

napi_value CreateBounds(napi_env env,
                        float x,
                        float y,
                        float width,
                        float height,
                        float scaleFactor) {
    napi_value result = nullptr;
    napi_create_object(env, &result);
    SetNamed(env, result, "x", CreateDouble(env, x));
    SetNamed(env, result, "y", CreateDouble(env, y));
    SetNamed(env, result, "width", CreateDouble(env, width));
    SetNamed(env, result, "height", CreateDouble(env, height));
    SetNamed(env, result, "scaleFactor", CreateDouble(env, scaleFactor));
    return result;
}

const char* CaptureResultTypeName(shotlet::CaptureResultType type) {
    switch (type) {
        case shotlet::CaptureResultType::Pin:
            return "pin";
        case shotlet::CaptureResultType::Ocr:
            return "ocr";
        case shotlet::CaptureResultType::None:
        default:
            return "none";
    }
}

napi_value CreateHostWindowResult(napi_env env, const shotlet::CaptureResultData& data) {
    napi_value result = nullptr;
    napi_create_object(env, &result);

    napi_value image = nullptr;
    void* copiedImage = nullptr;
    napi_create_buffer_copy(env,
                            data.rgba.size(),
                            data.rgba.empty() ? nullptr : data.rgba.data(),
                            &copiedImage,
                            &image);

    SetNamed(env, result, "type", CreateString(env, CaptureResultTypeName(data.type)));
    SetNamed(env, result, "image", image);
    SetNamed(env, result, "imageWidth", CreateInt32(env, data.imageWidth));
    SetNamed(env, result, "imageHeight", CreateInt32(env, data.imageHeight));
    SetNamed(env, result, "rowPitch", CreateInt32(env, data.rowPitch));
    SetNamed(env, result, "text", CreateString(env, data.text));
    SetNamed(env, result, "selection",
             CreateBounds(env,
                          data.selectionX,
                          data.selectionY,
                          data.selectionWidth,
                          data.selectionHeight,
                          data.scaleFactor));
    SetNamed(env, result, "window",
             CreateBounds(env,
                          data.windowX,
                          data.windowY,
                          data.windowWidth,
                          data.windowHeight,
                          data.scaleFactor));
    return result;
}

napi_value CreateCaptureResult(napi_env env,
                               int exitCode,
                               const shotlet::CaptureResultData* hostWindowResult = nullptr) {
    napi_value result = nullptr;
    napi_create_object(env, &result);

    const char* status = "error";
    if (exitCode == shotlet::ExitSuccess) {
        status = "success";
    } else if (exitCode == shotlet::ExitCancelled) {
        status = "cancelled";
    }

    SetNamed(env, result, "code", CreateInt32(env, exitCode));
    SetNamed(env, result, "status", CreateString(env, status));
    SetNamed(env, result, "ok", CreateInt32(env, exitCode == shotlet::ExitSuccess ? 1 : 0));
    if (hostWindowResult && hostWindowResult->type != shotlet::CaptureResultType::None) {
        SetNamed(env, result, "result", CreateHostWindowResult(env, *hostWindowResult));
    }
    return result;
}

bool HasProperty(napi_env env, napi_value object, const char* name) {
    bool present = false;
    napi_has_named_property(env, object, name, &present);
    return present;
}

// 每个 Node 环境分别保存后续截图配置，活动 Application 使用创建时的副本。
struct AddonConfiguration {
    shotlet::LanguagePreference language = shotlet::LanguagePreference::Auto;
    shotlet::ThemePreference theme = shotlet::ThemePreference::Auto;
};

AddonConfiguration* Configuration(napi_env env) {
    void* data = nullptr;
    napi_get_instance_data(env, &data);
    return static_cast<AddonConfiguration*>(data);
}

napi_value Configure(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        Throw(env, "configure(options) requires an options object");
        return nullptr;
    }
    napi_valuetype type = napi_undefined;
    napi_typeof(env, args[0], &type);
    napi_value nullValue = nullptr;
    napi_get_null(env, &nullValue);
    bool isNull = false;
    bool isArray = false;
    napi_strict_equals(env, args[0], nullValue, &isNull);
    napi_is_array(env, args[0], &isArray);
    if (type != napi_object || isNull || isArray) {
        Throw(env, "configure(options) requires an options object");
        return nullptr;
    }

    try {
        // 先验证全部选项，避免无效配置只应用了一部分。
        const bool hasDirectory = HasProperty(env, args[0], "logDir");
        const bool hasLevel = HasProperty(env, args[0], "logLevel");
        const bool hasConsoleLevel = HasProperty(env, args[0], "consoleLogLevel");
        const bool hasConsole = HasProperty(env, args[0], "consoleLog");
        const bool hasLanguage = HasProperty(env, args[0], "language");
        const bool hasTheme = HasProperty(env, args[0], "theme");
        auto theme = Configuration(env)->theme;
        std::string themeText;
        if (hasTheme && (!ReadStringProperty(env, args[0], "theme", themeText) ||
                         !shotlet::TryParseTheme(themeText, theme))) {
            Throw(env, "theme must be auto, light or dark");
            return nullptr;
        }
        auto language = Configuration(env)->language;
        std::string languageText;
        if (hasLanguage && (!ReadStringProperty(env, args[0], "language", languageText) ||
                            !shotlet::TryParseLanguage(languageText, language))) {
            Throw(env, "language must be auto, zh-CN or en");
            return nullptr;
        }
        std::string directory, levelText, consoleLevelText;
        shotlet::LogLevel level = shotlet::LogLevel::Info;
        shotlet::LogLevel consoleLevel = shotlet::LogLevel::Debug;
        bool console = false;
        if (hasDirectory && (!ReadStringProperty(env, args[0], "logDir", directory) || directory.empty())) {
            Throw(env, "configure(options.logDir) requires a non-empty string");
            return nullptr;
        }
        if (hasLevel && (!ReadStringProperty(env, args[0], "logLevel", levelText) ||
                         !shotlet::Logger::TryParseLevel(levelText, level))) {
            Throw(env, "logLevel must be debug, info, warn or error");
            return nullptr;
        }
        if (hasConsoleLevel && (!ReadStringProperty(env, args[0], "consoleLogLevel", consoleLevelText) ||
                                !shotlet::Logger::TryParseLevel(consoleLevelText, consoleLevel))) {
            Throw(env, "consoleLogLevel must be debug, info, warn or error");
            return nullptr;
        }
        if (hasConsole) {
            napi_value property = nullptr;
            napi_get_named_property(env, args[0], "consoleLog", &property);
            napi_typeof(env, property, &type);
            if (type != napi_boolean) {
                Throw(env, "consoleLog must be a boolean");
                return nullptr;
            }
            napi_get_value_bool(env, property, &console);
        }
        if (hasDirectory) shotlet::Logger::SetLogDirectoryOverride(PathFromUtf8(directory));
        if (hasLevel) shotlet::Logger::SetLevel(level);
        if (hasConsoleLevel) shotlet::Logger::SetConsoleLevel(consoleLevel);
        if (hasConsole) shotlet::Logger::SetConsoleEnabled(console);
        if (hasLanguage) Configuration(env)->language = language;
        if (hasTheme) Configuration(env)->theme = theme;
    } catch (const std::exception& e) {
        Throw(env, e.what());
        return nullptr;
    }
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

napi_value Capture(napi_env env, napi_callback_info info) {
    std::vector<std::string> args{"shotlet", "--language",
                                 shotlet::LanguageOption(Configuration(env)->language),
                                 "--theme", shotlet::ThemeOption(Configuration(env)->theme)};
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }

#if defined(SHOTLET_PLATFORM_MACOS)
    @autoreleasepool {
#endif
    try {
        shotlet::Application app(true);
        if (!app.Initialize(static_cast<int>(argv.size()), argv.data())) {
            return CreateCaptureResult(env, shotlet::ExitError);
        }
        int code = app.Run();
        return CreateCaptureResult(env, code, &app.GetCaptureResultData());
#if defined(SHOTLET_PLATFORM_WINDOWS)
    } catch (const winrt::hresult_error& e) {
        const std::string message = "WinRT error: " + winrt::to_string(e.message());
        shotlet::Logger::Error("event=addon.capture_failed error={}", message);
        Throw(env, message.c_str());
        return nullptr;
#endif
    } catch (const std::exception& e) {
        shotlet::Logger::Error("event=addon.capture_failed error={}", e.what());
        Throw(env, e.what());
        return nullptr;
    } catch (...) {
        shotlet::Logger::Error("event=addon.capture_failed error=unknown");
        Throw(env, "Unknown Shotlet error");
        return nullptr;
    }
#if defined(SHOTLET_PLATFORM_MACOS)
    }
#endif
}

void CleanupLogging(void*) {
    shotlet::Logger::Shutdown();
}

napi_value Init(napi_env env, napi_value exports) {
    std::unique_ptr<AddonConfiguration> configuration(new (std::nothrow) AddonConfiguration);
    if (!configuration || napi_set_instance_data(env, configuration.get(),
            [](napi_env, void* data, void*) { delete static_cast<AddonConfiguration*>(data); }, nullptr) != napi_ok) {
        Throw(env, "Unable to initialize Shotlet configuration");
        return nullptr;
    }
    // 注册成功后，配置的所有权交给 Node 环境的终结回调。
    configuration.release();
    napi_add_env_cleanup_hook(env, CleanupLogging, nullptr);
    napi_value configure = nullptr;
    napi_create_function(env, "configure", NAPI_AUTO_LENGTH, Configure, nullptr, &configure);
    napi_set_named_property(env, exports, "configure", configure);

    napi_value capture = nullptr;
    napi_create_function(env, "capture", NAPI_AUTO_LENGTH, Capture, nullptr, &capture);
    napi_set_named_property(env, exports, "capture", capture);

    napi_value exitCodes = nullptr;
    napi_create_object(env, &exitCodes);
    napi_set_named_property(env, exitCodes, "success", CreateInt32(env, shotlet::ExitSuccess));
    napi_set_named_property(env, exitCodes, "error", CreateInt32(env, shotlet::ExitError));
    napi_set_named_property(env, exitCodes, "cancelled", CreateInt32(env, shotlet::ExitCancelled));
    napi_set_named_property(env, exports, "exitCodes", exitCodes);

    return exports;
}

} // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
