#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

bool HasCommentedDeclaration(const std::string& header, const std::string& declaration) {
    const auto declarationPos = header.find(declaration);
    if (declarationPos == std::string::npos) {
        return false;
    }

    const auto lineStart = header.rfind('\n', declarationPos);
    const auto commentEnd = lineStart == std::string::npos ? 0 : lineStart;
    const auto commentStart = header.rfind("//", commentEnd);
    if (commentStart == std::string::npos) {
        return false;
    }

    const auto textBetween = header.substr(commentStart, declarationPos - commentStart);
    return textBetween.find('\n') != std::string::npos &&
           textBetween.find_first_not_of(" /\r\n\t") != std::string::npos;
}
} // namespace

TEST(ApplicationStructureTest, ExtractedHelpersHaveCommentsAndDefinitions) {
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto header = ReadFile(projectRoot / "src/app/Application.h");
    const auto source = ReadFile(projectRoot / "src/app/Application.cpp") +
                        ReadFile(projectRoot / "src/app/ApplicationCapture.cpp");
    // 声明和定义可以使用不同的换行及指针空格格式，结构约束只检查签名。
    const auto whitespace = std::regex(R"(\s+)");
    const auto normalizedSource = std::regex_replace(source, whitespace, "");
    const std::vector<std::string> helpers = {
        "void ProcessCaptureFrame();",
        "void ProcessAuxiliaryWindowsFrame();",
        "void ProcessIdleFrame();",
        "FBORenderer::RenderResult RenderCurrentSelection(const ScreenOverlay::Config*& config, Canvas*& canvas, const char* action);",
        "void CleanupAfterCapture(const char* reason);",
    };

    for (const auto& helper : helpers) {
        EXPECT_TRUE(HasCommentedDeclaration(header, helper)) << helper;

        auto implementationName = helper.substr(0, helper.find(';'));
        implementationName = std::regex_replace(
            implementationName,
            std::regex(R"((.*\s)([A-Za-z0-9_]+\())"),
            "$1Application::$2");
        EXPECT_NE(normalizedSource.find(std::regex_replace(implementationName, whitespace, "")),
                  std::string::npos) << helper;
    }
}

TEST(ApplicationStructureTest, PlatformCleanupAndHintsAreNotInApplication) {
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto source = ReadFile(projectRoot / "src/app/Application.cpp") +
                        ReadFile(projectRoot / "src/app/ApplicationInitialization.cpp") +
                        ReadFile(projectRoot / "src/app/ApplicationCapture.cpp");

    EXPECT_EQ(source.find("platform/PlatformWin32.h"), std::string::npos);
    EXPECT_EQ(source.find("platform/WindowsDpiAwareness.h"), std::string::npos);
    EXPECT_EQ(source.find("SDL_HINT_WINDOWS_RAW_KEYBOARD"), std::string::npos);
    EXPECT_EQ(source.find("SetCursor"), std::string::npos);
    EXPECT_EQ(source.find("SDL_WaitForGPUIdle"), std::string::npos);
}

TEST(ApplicationStructureTest, MacGpuCreationAllowsMetalMacFamily1Hardware) {
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto platformSource = ReadFile(projectRoot / "src/platform/ApplicationPlatform_mac.cpp");

    EXPECT_NE(platformSource.find("ApplicationPlatform_mac.cpp should only be built for macOS"), std::string::npos);
    EXPECT_NE(platformSource.find("SDL_CreateGPUDeviceWithProperties"), std::string::npos);
    EXPECT_NE(platformSource.find("SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN"), std::string::npos);
    EXPECT_NE(platformSource.find("SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN"), std::string::npos);
    EXPECT_NE(platformSource.find("SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN"), std::string::npos);
}

TEST(ApplicationStructureTest, MacImGuiSdlGpuBackendUsesMslOnlyOnOlderMacOS) {
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto rootCmake = ReadFile(projectRoot / "CMakeLists.txt");
    const auto wrapperSource = ReadFile(projectRoot / "src/third_party/gpu_msl_backend.cpp");

    EXPECT_NE(rootCmake.find("gpu_msl_backend.cpp"), std::string::npos);
    EXPECT_NE(wrapperSource.find("#define SDL_GetGPUShaderFormats"), std::string::npos);
    EXPECT_EQ(wrapperSource.find("__builtin_available(macOS 14.0, *)"), std::string::npos);
    EXPECT_NE(wrapperSource.find("kern.osproductversion"), std::string::npos);
    EXPECT_NE(wrapperSource.find("ShotletImGuiIsAtLeastMacOS14"), std::string::npos);
    EXPECT_NE(wrapperSource.find("return formats;"), std::string::npos);
    EXPECT_NE(wrapperSource.find("SDL_GPU_SHADERFORMAT_MSL"), std::string::npos);
    EXPECT_NE(wrapperSource.find("#define SDL_CreateGPUShader"), std::string::npos);
    EXPECT_NE(wrapperSource.find("#define SDL_CreateGPUGraphicsPipeline"), std::string::npos);
    EXPECT_NE(wrapperSource.find("#define SDL_BindGPUGraphicsPipeline"), std::string::npos);
    EXPECT_NE(wrapperSource.find("#define SDL_DrawGPUIndexedPrimitives"), std::string::npos);
    EXPECT_NE(wrapperSource.find("SDL_GetError()"), std::string::npos);
    EXPECT_NE(wrapperSource.find("imgui_impl_sdlgpu3.cpp"), std::string::npos);
}

TEST(ApplicationStructureTest, ResultWindowPlumbingIsRemoved) {
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto mainSource = ReadFile(projectRoot / "src/main.cpp");
    const auto appSource = ReadFile(projectRoot / "src/app/Application.cpp") +
                           ReadFile(projectRoot / "src/app/ApplicationOutput.cpp");
    const auto rootCmake = ReadFile(projectRoot / "CMakeLists.txt");
    const auto testsCmake = ReadFile(projectRoot / "tests/CMakeLists.txt");

    EXPECT_FALSE(std::filesystem::exists(projectRoot / "src/platform/ResultWindowLauncher.cpp"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "src/platform/ResultWindowLauncher.h"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "src/platform/ResultWindowMain.cpp"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "src/platform/ResultWindowMain.h"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "src/platform/ResultWindowPayload.cpp"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "src/platform/ResultWindowPayload.h"));
    EXPECT_FALSE(std::filesystem::exists(projectRoot / "tests/test_result_window_payload.cpp"));

    EXPECT_EQ(mainSource.find("ResultWindow"), std::string::npos);
    EXPECT_EQ(mainSource.find("--result-window"), std::string::npos);
    EXPECT_EQ(appSource.find("ResultWindowLauncher"), std::string::npos);
    EXPECT_EQ(appSource.find("LaunchPinResultWindow"), std::string::npos);
    EXPECT_EQ(appSource.find("LaunchOcrResultWindow"), std::string::npos);
    EXPECT_EQ(rootCmake.find("ResultWindow"), std::string::npos);
    EXPECT_EQ(testsCmake.find("ResultWindow"), std::string::npos);
}

TEST(ApplicationStructureTest, WindowsBuildScriptHandlesGeneratorPlatformCache) {
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto script = ReadFile(projectRoot / "build.ps1");

    EXPECT_NE(script.find("CMAKE_GENERATOR_PLATFORM"), std::string::npos);
    EXPECT_NE(script.find("Clear-CMakeConfigurationCache"), std::string::npos);
    EXPECT_NE(script.find("Existing build directory uses CMake generator platform"), std::string::npos);
}

TEST(ApplicationStructureTest, EditorResultAnchorIsQueriedViaOverlayManager) {
    // Pin/OCR 结果窗口定位必须经 GetResultSelectionScreenOrigin 取锚点：
    // 编辑器选区是图片坐标，直接 config.windowX + selection.Min.x 会失配
    // （长图 y 可达数千，结果窗口弹到屏幕外）。此断言防止回退为手算。
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto appSource = ReadFile(projectRoot / "src/app/ApplicationOutput.cpp") +
                           ReadFile(projectRoot / "src/app/ApplicationOcr.cpp");

    EXPECT_NE(appSource.find("GetResultSelectionScreenOrigin()"), std::string::npos);
    EXPECT_EQ(appSource.find("config.windowX) + selection.Min.x"), std::string::npos);
    EXPECT_EQ(appSource.find("config.windowY) + selection.Min.y"), std::string::npos);
    EXPECT_EQ(appSource.find("m_ocrConfig.windowX + m_ocrSelection.Min.x"), std::string::npos);
    EXPECT_EQ(appSource.find("m_ocrConfig.windowY + m_ocrSelection.Min.y"), std::string::npos);
}

TEST(ApplicationStructureTest, PlatformLayerDoesNotDependOnApplication) {
    // platform 层不得反向依赖 app 层（分层：app → ui → platform）。
    // ApplyEditorGpuBufferCount 曾在 .mm 内直接调 Application::Instance()，
    // 修复后改为 ui 层调用方传值；此断言防止回退。
    const auto projectRoot = std::filesystem::path(SHOTLET_SOURCE_DIR);
    const auto header = ReadFile(projectRoot / "src/platform/ImageEditorWindow.h");
    const auto macSource = ReadFile(projectRoot / "src/platform/ImageEditorWindow_mac.mm");
    const auto editorSource = ReadFile(projectRoot / "src/ui/ImageEditor.cpp");

    EXPECT_EQ(macSource.find("app/Application.h"), std::string::npos);
    EXPECT_EQ(macSource.find("Application::Instance"), std::string::npos);

    // 签名带 count 参数，数值由调用方负责
    EXPECT_NE(header.find("void ApplyEditorGpuBufferCount(SDL_Window* window, int count);"),
              std::string::npos);

    // ui 层调用方从 Application 取值传入（依赖方向 ui → app 合法）
    EXPECT_NE(editorSource.find("ApplyEditorGpuBufferCount(m_sdlWindow, gpuBufferCount)"),
              std::string::npos);
    EXPECT_NE(editorSource.find("GetGpuBufferCount()"), std::string::npos);
}
