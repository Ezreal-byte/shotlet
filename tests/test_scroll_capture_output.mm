#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cstring>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#import <objc/runtime.h>
#include <vector>
#include "core/Canvas.h"
#include "output/FBORenderer.h"
#include "platform/ImageEditorWindow.h"
#include "platform/MacImageOutput.h"
#include "platform/OcrService.h"
#include "platform/OcrWindow.h"
#include "platform/PinWindow.h"
#include "platform/TransparentWindow.h"
#import "scrollcapture/MacScrollMaskView.h"
#include "ui/HistoryWindow.h"
#include "ui/RecentCapturesWindow.h"
#include "ui/RecentCapturesLayout.h"
#include "app/Application.h"
#include "ui/ImageEditor.h"
#include "ui/ScreenOverlay.h"
#include "ui/SubToolbar.h"
#include "ui/Toolbar.h"
#include "ui/OverlayManager.h"
#include "utils/ImGuiUtils.h"
#include "gpu_preview.h"
#include <thread>

using namespace shotlet;

TEST(MacImageOutputTest, OwnedRgbaImageCopiesPaddedRowsAndPreservesStraightAlpha) {
    constexpr int width = 1, height = 2, rowPitchBytes = 8;
    const std::array<unsigned char, rowPitchBytes * height> expected{
        243, 111, 35, 128, 123, 123, 123, 123, 21, 99, 177, 64, 123, 123, 123, 123};
    auto source = expected;
    CGImageRef image = CreateOwnedImageFromRGBA(source.data(), width, height, rowPitchBytes);
    ASSERT_NE(image, nullptr);
    source.fill(0);
    EXPECT_EQ(CGImageGetWidth(image), width); EXPECT_EQ(CGImageGetHeight(image), height);
    EXPECT_EQ(CGImageGetBytesPerRow(image), rowPitchBytes);
    EXPECT_EQ(CGImageGetAlphaInfo(image), kCGImageAlphaLast);
    NSData* bytes = CFBridgingRelease(CGDataProviderCopyData(CGImageGetDataProvider(image)));
    CGImageRelease(image);
    ASSERT_EQ(bytes.length, expected.size());
    EXPECT_EQ(std::memcmp(bytes.bytes, expected.data(), expected.size()), 0);
    EXPECT_EQ(CreateOwnedImageFromRGBA(nullptr, width, height, rowPitchBytes), nullptr);
    EXPECT_EQ(CreateOwnedImageFromRGBA(source.data(), width, height, width), nullptr);
}

TEST(ScrollCaptureOutputTest, MaximumHeightPngRoundTripsWithoutCropping) {
    @autoreleasepool {
        constexpr int width = 40, height = 16384;
        std::vector<uint8_t> pixels(size_t(width) * height * 4, 255);
        pixels[0] = 17;
        pixels[(size_t(height - 1) * width) * 4 + 2] = 91;
        NSString* path = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-long-%@.png", NSUUID.UUID.UUIDString];
        ASSERT_TRUE(WritePngToFile(pixels.data(), width, height, path.UTF8String));
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr);
        ASSERT_NE(source, nullptr);
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        ASSERT_NE(image, nullptr);
        EXPECT_EQ(CGImageGetWidth(image), width);
        EXPECT_EQ(CGImageGetHeight(image), height);
        std::vector<uint8_t> decoded(pixels.size());
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(decoded.data(), width, height, 8, width * 4, color,
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
        ASSERT_NE(context, nullptr);
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
        EXPECT_EQ(decoded, pixels);
        CGContextRelease(context); CGColorSpaceRelease(color);
        CGImageRelease(image); CFRelease(source);
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
}

TEST(ScrollCaptureOutputTest, OcrReadsBothEndsAndDeduplicatesOverlap) {
    @autoreleasepool {
        constexpr int width = 800, height = 5000;
        std::vector<uint8_t> pixels(size_t(width) * height * 4, 255);
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, width * 4, color,
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
        ASSERT_NE(context, nullptr);
        CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), 48, nullptr);
        auto text = [&](NSString* value, int y) {
            NSDictionary* attributes = @{(__bridge NSString*)kCTFontAttributeName: (__bridge id)font,
                (__bridge NSString*)kCTForegroundColorAttributeName: (__bridge id)NSColor.blackColor.CGColor};
            NSAttributedString* string = [[NSAttributedString alloc] initWithString:value attributes:attributes];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
            CGContextSetTextPosition(context, 24, height - y);
            CTLineDraw(line, context); CFRelease(line);
        };
        text(@"FIRST SECTION", 100);
        text(@"OVERLAP SECTION", 4030);
        text(@"LAST SECTION", 4900);
        CFRelease(font); CGContextRelease(context); CGColorSpaceRelease(color);
        std::string result = OcrService::Recognize(pixels.data(), width, height);
        EXPECT_NE(result.find("FIRST SECTION"), std::string::npos) << result;
        EXPECT_NE(result.find("LAST SECTION"), std::string::npos) << result;
        auto overlap = result.find("OVERLAP SECTION");
        ASSERT_NE(overlap, std::string::npos) << result;
        EXPECT_EQ(result.find("OVERLAP SECTION", overlap + 1), std::string::npos) << result;
    }
}

TEST(ScrollCaptureOutputTest, EnglishOcrWindowPreservesChineseResultAndLocalizesEmptyText) {
    @autoreleasepool {
        if (NSScreen.screens.count == 0) GTEST_SKIP() << "当前会话没有显示器";
        std::vector<uint8_t> pixels(200 * 100 * 4, 255);
        struct CloseOcrWindow { ~CloseOcrWindow() { OcrWindow::Destroy(); } } close;
        for (const std::string text : {std::string("中文识别结果"), std::string()}) {
            ASSERT_TRUE(OcrWindow::Create(pixels.data(), 200, 100, text, 10, 10,
                                          1.0f, Language::English));
            NSWindow* resultWindow = nil;
            for (NSWindow* candidate in NSApp.windows) {
                if ([candidate.title isEqualToString:@"Extract text"]) resultWindow = candidate;
            }
            ASSERT_NE(resultWindow, nil);
            NSTextView* resultView = nil;
            for (NSView* candidate in resultWindow.contentView.subviews) {
                if ([candidate isKindOfClass:NSScrollView.class]) {
                    NSView* document = [(NSScrollView*)candidate documentView];
                    if ([document isKindOfClass:NSTextView.class]) resultView = (NSTextView*)document;
                }
            }
            ASSERT_NE(resultView, nil);
            EXPECT_TRUE([resultView.string isEqualToString:text.empty() ? @"No text recognized" : @"中文识别结果"]);
            OcrWindow::Destroy();
        }
    }
}

TEST(ScrollCaptureOutputTest, OcrThemeSwitchPreservesTextAndHostAppearance) {
    @autoreleasepool {
        if (NSScreen.screens.count == 0) GTEST_SKIP() << "当前会话没有显示器";
        std::vector<uint8_t> pixels(200 * 100 * 4, 255);
        struct CloseWindow { ~CloseWindow() { OcrWindow::Destroy(); } } close;
        NSAppearance* hostAppearance = NSApp.appearance;
        ASSERT_TRUE(OcrWindow::Create(pixels.data(), 200, 100, "中文结果", 10, 10,
                                     1, Language::English, Theme::Light));
        NSWindow* result = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"Extract text"]) result = candidate;
        ASSERT_NE(result, nil);
        NSTextView* text = nil;
        for (NSView* candidate in result.contentView.subviews) {
            if ([candidate isKindOfClass:NSScrollView.class]) {
                NSView* document = [(NSScrollView*)candidate documentView];
                if ([document isKindOfClass:NSTextView.class]) text = (NSTextView*)document;
            }
        }
        ASSERT_NE(text, nil);
        const NSRect frame = result.frame;
        for (const auto theme : {Theme::Dark, Theme::Light, Theme::Dark}) {
            OcrWindow::SetTheme(theme);
            EXPECT_TRUE([result.appearance.name isEqualToString:theme == Theme::Dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua]);
            EXPECT_TRUE([text.string isEqualToString:@"中文结果"]);
            EXPECT_TRUE(NSEqualRects(result.frame, frame));
            [result.effectiveAppearance performAsCurrentDrawingAppearance:^{
                const auto foreground = [text.textColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace].redComponent;
                const auto background = [text.backgroundColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace].redComponent;
                EXPECT_GT(std::abs(foreground - background), 0.5);
            }];
            EXPECT_EQ(NSApp.appearance, hostAppearance);
        }
    }
}

class ScrollCaptureGpuOutputTest : public testing::Test {
protected:
    SDL_GPUDevice* device = nullptr;
    SDL_Window* window = nullptr;
    ImGuiContext* context = nullptr;
    bool initialized = false;
    void SetUp() override {
        if (!SDL_Init(SDL_INIT_VIDEO)) GTEST_SKIP() << "SDL 视频不可用: " << SDL_GetError();
        window = SDL_CreateWindow("Shotlet 长图输出测试", 800, 600, SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, "metal");
        if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window))
            GTEST_SKIP() << "Metal 输出不可用: " << SDL_GetError();
        context = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().Fonts->AddFontDefault();
        ImGui_ImplSDL3_InitForOther(window);
        ImGui_ImplSDLGPU3_InitInfo info = {};
        info.Device = device;
        info.ColorTargetFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
        info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
        ImGui_ImplSDLGPU3_Init(&info);
        initialized = true;
    }
    void TearDown() override {
        if (device) SDL_WaitForGPUIdle(device);
        if (initialized) { ImGui_ImplSDLGPU3_Shutdown(); ImGui_ImplSDL3_Shutdown(); }
        if (context) ImGui::DestroyContext(context);
        if (device && window) SDL_ReleaseWindowFromGPUDevice(device, window);
        if (window) SDL_DestroyWindow(window);
        if (device) SDL_DestroyGPUDevice(device);
        SDL_Quit();
    }
};

TEST_F(ScrollCaptureGpuOutputTest, EditorThemeSwitchPreservesGeometryAnnotationsAndExportPixels) {
    auto image = std::make_shared<ScrollImage>();
    image->width = 320;
    image->height = 240;
    image->rgba.assign(size_t(image->width) * image->height * 4, 255);
    image->rgba[0] = 17;
    ScreenOverlay::Config config;
    config.language = Language::English;
    struct RestoreContext {
        ImGuiContext* previous;
        ~RestoreContext() { ImGui::SetCurrentContext(previous); }
    } restore{context};
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    Canvas* canvas = editor.GetCanvas();
    canvas->SetTool(ToolType::Rectangle);
    canvas->SetToolColor(IM_COL32(200, 30, 50, 255));
    canvas->SetToolFill(true);
    canvas->OnMouseDown(ImVec2(20, 20));
    canvas->OnMouseMove(ImVec2(80, 80));
    canvas->OnMouseUp(ImVec2(80, 80));
    const auto selection = canvas->GetSelection();
    SDL_Window* editorWindow = editor.GetSDLWindow();
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(editorWindow), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    ASSERT_NE(native, nil);
    const NSRect frame = native.frame;
    std::vector<uint8_t> baseline;
    for (const auto theme : {Theme::Light, Theme::Dark, Theme::Light}) {
        ImGui::SetCurrentContext(context);
        editor.SetTheme(theme);
        EXPECT_EQ(ImGui::GetCurrentContext(), context);
        EXPECT_TRUE(NSEqualRects(native.frame, frame));
        EXPECT_TRUE([native.appearance.name isEqualToString:theme == Theme::Dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua]);
        editor.RenderFrame();
        EXPECT_EQ(canvas->GetSelection().Min.x, selection.Min.x);
        EXPECT_EQ(canvas->GetSelection().Max.y, selection.Max.y);
        ImGui::SetCurrentContext(editor.GetImGuiContext());
        EXPECT_EQ(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Text]), GetThemePalette(theme).text);
        auto output = FBORenderer::RenderToOffscreen(device, editorWindow, editor.GetImGuiContext(), canvas, canvas->GetSelection(), 1);
        ASSERT_NE(output.data, nullptr);
        std::vector<uint8_t> pixels(output.data.get(), output.data.get() + size_t(output.width) * output.height * 4);
        if (baseline.empty()) baseline = pixels;
        else EXPECT_EQ(pixels, baseline);
    }
}

TEST_F(ScrollCaptureGpuOutputTest, CaptureDocumentRestoresSevenEditableLayersInRealEditorAtEveryRotation) {
    struct RestoreContext {
        ImGuiContext* previous;
        ~RestoreContext() { ImGui::SetCurrentContext(previous); }
    } restore{context};
    auto image = std::make_shared<ScrollImage>();
    image->width = 320; image->height = 240;
    image->rgba.resize(size_t(image->width) * image->height * 4);
    for (int y = 0; y < image->height; ++y) for (int x = 0; x < image->width; ++x) {
        auto* p = image->rgba.data() + (size_t(y) * image->width + x) * 4;
        p[0] = x % 256; p[1] = y; p[2] = (x + y) % 256; p[3] = 255;
    }
    ScreenOverlay::Config config;
    ImageEditor source(config, image, device);
    ASSERT_TRUE(source.Create());
    auto* canvas = source.GetCanvas();
    canvas->SetUIScale(1.5f);
    canvas->SetTool(ToolType::Mosaic);
    canvas->OnMouseDown(ImVec2(4, 4)); canvas->OnMouseMove(ImVec2(55, 50)); canvas->OnMouseUp(ImVec2(55, 50));
    canvas->SetTool(ToolType::None);
    auto& layers = canvas->GetLayerManager();
    auto rectangle = std::make_unique<RectangleLayer>(ImVec2(70, 20), ImVec2(140, 80), IM_COL32(200, 20, 50, 200), 4);
    rectangle->SetShowBorder(false); layers.AddLayer(std::move(rectangle));
    auto ellipse = std::make_unique<EllipseLayer>(ImVec2(160, 20), ImVec2(230, 80), IM_COL32(20, 180, 70, 255), 3);
    ellipse->SetShowBorder(false); ellipse->SetFilled(true); layers.AddLayer(std::move(ellipse));
    auto arrow = std::make_unique<ArrowLayer>(ImVec2(40, 140), ImVec2(140, 110), IM_COL32(40, 90, 200, 255), 4);
    arrow->SetShowHandles(false); layers.AddLayer(std::move(arrow));
    layers.AddLayer(std::make_unique<BrushLayer>(std::vector<ImVec2>{{170, 110}, {190, 125}, {220, 115}}, IM_COL32(120, 30, 150, 160), 6));
    layers.AddLayer(std::make_unique<TextLayer>(ImVec2(25, 170), "History\neditable", IM_COL32(10, 10, 10, 255), 19, 120));
    layers.AddLayer(std::make_unique<NumberLayer>(ImVec2(260, 180), 1, IM_COL32(200, 70, 10, 255), 36));
    std::string error;
    for (int turns = 0; turns < 4; ++turns) {
        ImGui::SetCurrentContext(source.GetImGuiContext());
        source.RenderFrame(); // 与真实输出一致，先上传此前马赛克操作累积的脏区
        auto document = canvas->ExportDocument(error);
        ASSERT_TRUE(document) << error;
        ASSERT_EQ(document->layers.size(), 7u);
        auto expected = FBORenderer::RenderToOffscreen(device, source.GetSDLWindow(), source.GetImGuiContext(),
            canvas, canvas->GetSelection(), 1);
        ASSERT_NE(expected.data, nullptr);
        auto snapshot = std::make_shared<CaptureDocument>(std::move(*document));
        auto targetConfig = config;
        targetConfig.scaleFactor = 2;
        ImageEditor target(snapshot, targetConfig, device);
        ASSERT_TRUE(target.Create());
        auto* restoredCanvas = target.GetCanvas();
        EXPECT_EQ(restoredCanvas->GetImageQuarterTurns(), turns);
        EXPECT_FLOAT_EQ(restoredCanvas->GetDisplaySize().x, 320);
        for (float uiScale : {1.0f, 1.5f, 2.0f}) {
            restoredCanvas->SetUIScale(uiScale);
            target.RenderFrame();
            auto actual = FBORenderer::RenderToOffscreen(device, target.GetSDLWindow(), target.GetImGuiContext(),
                restoredCanvas, restoredCanvas->GetSelection(), 2);
            ASSERT_NE(actual.data, nullptr);
            ASSERT_EQ(actual.width, expected.width); ASSERT_EQ(actual.height, expected.height);
            EXPECT_EQ(std::memcmp(actual.data.get(), expected.data.get(), size_t(expected.width) * expected.height * 4), 0)
                << "旋转=" << turns << ", UI 缩放=" << uiScale;
        }
        ImGui::SetCurrentContext(target.GetImGuiContext());
        // 旧文字经真实输入队列追加文字，提交后继续沿用同一图层标识。
        restoredCanvas->OnMouseDoubleClick(ImVec2(30, 175));
        ASSERT_TRUE(restoredCanvas->IsTextInputActive());
        ImGui::GetIO().AddInputCharactersUTF8(" edited");
        target.RenderFrame();
        restoredCanvas->CommitTextInput();
        auto edited = restoredCanvas->ExportDocument(error);
        ASSERT_TRUE(edited) << error;
        bool foundText = false;
        for (const auto& layer : edited->layers) if (const auto* text = std::get_if<DocumentText>(&layer.data)) {
            foundText = true;
            EXPECT_NE(text->text.find(" edited"), std::string::npos);
            EXPECT_EQ(layer.id, snapshot->layers[5].id);
        }
        EXPECT_TRUE(foundText);
        ASSERT_TRUE(restoredCanvas->DeleteLayerById(snapshot->layers[0].id));
        EXPECT_EQ(std::memcmp(restoredCanvas->GetCurrentImageData(), image->rgba.data(), image->rgba.size()), 0);
        ImGui::SetCurrentContext(source.GetImGuiContext());
        canvas->RotateImageCounterClockwise();
    }
}

TEST_F(ScrollCaptureGpuOutputTest, CaptureDocumentCroppedRetinaMosaicMatchesOriginalGpuExport) {
    struct RestoreContext {
        ImGuiContext* previous;
        ~RestoreContext() { ImGui::SetCurrentContext(previous); }
    } restore{context};
    constexpr int size = 200;
    auto pixels = std::make_unique<uint8_t[]>(size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        auto* p = pixels.get() + (y * size + x) * 4;
        p[0] = x % 2 ? 255 : 0; p[1] = y % 2 ? 255 : 0; p[2] = (x + y) % 256; p[3] = 255;
    }
    Canvas canvas;
    canvas.SetUIScale(1.5f);
    canvas.SetScreenshotOwned(1, ImVec2(size, size), ImVec2(100, 100), std::move(pixels), size * 4);
    canvas.OnMouseDown(ImVec2(10.5f, 11.5f)); canvas.OnMouseMove(ImVec2(90.5f, 91.5f)); canvas.OnMouseUp(ImVec2(90.5f, 91.5f));
    canvas.SetImageEditing(true);
    canvas.SetTool(ToolType::Mosaic);
    canvas.OnMouseDown(ImVec2(11, 12)); canvas.OnMouseMove(ImVec2(45, 50)); canvas.OnMouseUp(ImVec2(45, 50));
    canvas.SetTool(ToolType::None);
    auto rectangle = std::make_unique<RectangleLayer>(ImVec2(30, 55), ImVec2(80, 85), IM_COL32(200, 20, 40, 255), 3);
    rectangle->SetShowBorder(false); canvas.GetLayerManager().AddLayer(std::move(rectangle));
    auto texture = CreateScreenshotTexture(device, size, size, canvas.GetCurrentImageData(), size * 4);
    ASSERT_NE(texture, nullptr);
    struct ReleaseTexture {
        SDL_GPUDevice* device; SDL_GPUTexture* texture;
        ~ReleaseTexture() { SDL_ReleaseGPUTexture(device, texture); }
    } release{device, texture};
    canvas.SetScreenshot((ImTextureID)(intptr_t)texture, ImVec2(size, size), ImVec2(100, 100));
    std::string error;
    for (int turns = 0; turns < 4; ++turns) {
        ImGui::SetCurrentContext(context);
        auto document = canvas.ExportDocument(error);
        ASSERT_TRUE(document) << error;
        ASSERT_EQ(document->width, 160); ASSERT_EQ(document->height, 160);
        EXPECT_EQ(document->mosaicGridOffsetX, 21 % 18);
        EXPECT_EQ(document->mosaicGridOffsetY, 23 % 18);
        auto expected = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), 2);
        ASSERT_NE(expected.data, nullptr);
        ScreenOverlay::Config config;
        config.scaleFactor = 3;
        ImageEditor editor(std::make_shared<CaptureDocument>(std::move(*document)), config, device);
        ASSERT_TRUE(editor.Create());
        editor.RenderFrame();
        auto* restoredCanvas = editor.GetCanvas();
        auto actual = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
            restoredCanvas, restoredCanvas->GetSelection(), 3);
        ASSERT_NE(actual.data, nullptr);
        ASSERT_EQ(actual.width, expected.width); ASSERT_EQ(actual.height, expected.height);
        EXPECT_EQ(std::memcmp(actual.data.get(), expected.data.get(), size_t(expected.width) * expected.height * 4), 0)
            << "裁剪后旋转=" << turns;
        canvas.RotateImageCounterClockwise();
    }
}
TEST_F(ScrollCaptureGpuOutputTest, CaptureDocumentFractionalSamplesKeepGeometryAndMosaicWithoutOutsidePixels) {
    struct RestoreContext {
        ImGuiContext* previous;
        ~RestoreContext() { ImGui::SetCurrentContext(previous); }
    } restore{context};
    constexpr int size = 200;
    auto pixels = std::make_unique<uint8_t[]>(size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        auto* p = pixels.get() + (y * size + x) * 4;
        p[0] = x % 2 ? 255 : 0; p[1] = y % 2 ? 255 : 0; p[2] = (x + y) % 256; p[3] = 255;
    }
    Canvas canvas;
    canvas.SetUIScale(1.5f);
    canvas.SetScreenshotOwned(1, ImVec2(size, size), ImVec2(100, 100), std::move(pixels), size * 4);
    canvas.OnMouseDown(ImVec2(10.375f, 11.625f)); canvas.OnMouseMove(ImVec2(90.625f, 91.875f)); canvas.OnMouseUp(ImVec2(90.625f, 91.875f));
    canvas.SetImageEditing(true);
    canvas.SetTool(ToolType::Mosaic);
    canvas.OnMouseDown(ImVec2(11, 12)); canvas.OnMouseMove(ImVec2(45, 50)); canvas.OnMouseUp(ImVec2(45, 50));
    canvas.SetTool(ToolType::None);
    auto rectangle = std::make_unique<RectangleLayer>(ImVec2(30, 55), ImVec2(80, 85), IM_COL32(200, 20, 40, 255), 3);
    rectangle->SetShowBorder(false); canvas.GetLayerManager().AddLayer(std::move(rectangle));
    auto texture = CreateScreenshotTexture(device, size, size, canvas.GetCurrentImageData(), size * 4);
    ASSERT_NE(texture, nullptr);
    struct ReleaseTexture {
        SDL_GPUDevice* device; SDL_GPUTexture* texture;
        ~ReleaseTexture() { SDL_ReleaseGPUTexture(device, texture); }
    } release{device, texture};
    canvas.SetScreenshot((ImTextureID)(intptr_t)texture, ImVec2(size, size), ImVec2(100, 100));
    std::string error;
    for (int turns = 0; turns < 4; ++turns) {
        ImGui::SetCurrentContext(context);
        auto document = canvas.ExportDocument(error);
        ASSERT_TRUE(document) << error;
        ASSERT_EQ(document->width, 160); ASSERT_EQ(document->height, 161);
        EXPECT_EQ(document->mosaicGridOffsetX, 21 % 18);
        EXPECT_EQ(document->mosaicGridOffsetY, 23 % 18);
        auto expected = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), 2);
        ASSERT_NE(expected.data, nullptr);
        ScreenOverlay::Config config;
        config.scaleFactor = 3;
        ImageEditor editor(std::make_shared<CaptureDocument>(std::move(*document)), config, device);
        ASSERT_TRUE(editor.Create());
        editor.RenderFrame();
        auto* restoredCanvas = editor.GetCanvas();
        auto actual = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
            restoredCanvas, restoredCanvas->GetSelection(), 3);
        ASSERT_NE(actual.data, nullptr);
        ASSERT_EQ(actual.width, expected.width); ASSERT_EQ(actual.height, expected.height);
        // 不同尺寸纹理的归一化 UV 插值允许一个通道量化级差，禁止几何偏移。
        int maximumDifference = 0;
        for (size_t i = 0; i < size_t(expected.width) * expected.height * 4; ++i)
            maximumDifference = std::max(maximumDifference, std::abs(int(actual.data[i]) - int(expected.data[i])));
        EXPECT_LE(maximumDifference, 1) << "小数裁剪后旋转=" << turns;
        for (int row = 0; row < 161; ++row)
            EXPECT_EQ(std::memcmp(restoredCanvas->GetCurrentImageData() + row * 160 * 4,
                canvas.GetCurrentImageData() + (23 + row) * size * 4 + 21 * 4, 160 * 4), 0);
        canvas.RotateImageCounterClockwise();
    }
}

namespace {
template <typename Window>
void ClickWindow(Window& window,float x,float y,int button=ImGuiMouseButton_Left) {
    ImGui::SetCurrentContext(window.GetImGuiContext());
    SDL_Event event{};event.type=SDL_EVENT_MOUSE_MOTION;event.motion.windowID=SDL_GetWindowID(window.GetSDLWindow());event.motion.x=x;event.motion.y=y;window.ProcessEvent(event);
    event={};event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;event.button.windowID=SDL_GetWindowID(window.GetSDLWindow());event.button.button=button==ImGuiMouseButton_Right?SDL_BUTTON_RIGHT:SDL_BUTTON_LEFT;
    event.button.x=x;event.button.y=y;window.ProcessEvent(event);
    auto& io=ImGui::GetIO();io.ConfigInputTrickleEventQueue=false;io.AddMouseButtonEvent(button,false);window.RenderFrame();
    io.AddMousePosEvent(x,y);io.AddMouseButtonEvent(button,true);window.RenderFrame();
    io.AddMouseButtonEvent(button,false);window.RenderFrame();
    event.type=SDL_EVENT_MOUSE_BUTTON_UP;window.ProcessEvent(event);
}
void ClickHistory(HistoryWindow& history,float x,float y,int button=ImGuiMouseButton_Left) {
    ClickWindow(history,x,y,button);
}
void SaveWindowFrame(SDL_GPUDevice* device, SDL_Window* window, ImGuiContext* context, const std::string& path) {
    const auto output = std::filesystem::path(SHOTLET_HISTORY_PREVIEW_DIR) / std::filesystem::path(path).filename();
    std::string error;
    EXPECT_TRUE(shotlet::TestUtils::SaveGpuPreview(device, window, context, output, error)) << error;
}
void SaveHistoryFrame(SDL_GPUDevice* device, HistoryWindow& history, const std::string& path) {
    // 既有历史界面测试在保存预览后继续查询控件，保留其切换到历史上下文的约定。
    ImGui::SetCurrentContext(history.GetImGuiContext());
    SaveWindowFrame(device, history.GetSDLWindow(), history.GetImGuiContext(), path);
}
}

namespace shotlet {
NSPasteboard* RecentTestPasteboard(id, SEL) {
    return [NSPasteboard pasteboardWithName:@"org.shotlet.recent-captures-tests"];
}
class RecentCapturesWindowTest : public ::ScrollCaptureGpuOutputTest {
protected:
    void SetUp() override {
        ScrollCaptureGpuOutputTest::SetUp();
        if (!initialized) return;
        SDL_GetGlobalMouseState(&originalMouseX, &originalMouseY);
        root = std::filesystem::temp_directory_path() / ("shotlet-recent-" + NewHistoryId());
        service = std::make_unique<HistoryService>(root);
        recent = std::make_unique<RecentCapturesWindow>(device, *service, Language::ChineseSimplified, Theme::Dark);
        pasteboardMethod = class_getClassMethod(NSPasteboard.class, @selector(generalPasteboard));
        originalPasteboard = method_setImplementation(pasteboardMethod, reinterpret_cast<IMP>(RecentTestPasteboard));
    }
    void TearDown() override {
        if (application) { application->m_gpuDevice = nullptr; application.reset(); }
        recent.reset(); service.reset();
        if (mouseWarped) SDL_WarpMouseGlobal(originalMouseX, originalMouseY);
        if (originalPasteboard) {
            [[NSPasteboard generalPasteboard] releaseGlobally];
            method_setImplementation(pasteboardMethod, originalPasteboard);
        }
        if (!root.empty()) { std::error_code error; std::filesystem::remove_all(root, error); }
        ScrollCaptureGpuOutputTest::TearDown();
    }
    void Seed(int count) {
        constexpr int width = 640, height = 400, channels = 4;
        for (int index = 0; index < count; ++index) {
            CaptureDocument document; document.width = width; document.height = height;
            document.logicalSize = {width, height}; document.createdAtUnixMs = HistoryNow() + index;
            document.originalRgba.assign(width * height * channels, 255);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                auto* pixel = document.originalRgba.data() + (y * width + x) * channels;
                pixel[0] = uint8_t(30 + (index * 17) % 120);
                pixel[1] = uint8_t(60 + x * 110 / width);
                pixel[2] = uint8_t(80 + y * 100 / height);
            }
            auto pixels = std::shared_ptr<unsigned char>(new unsigned char[document.originalRgba.size()], std::default_delete<unsigned char[]>());
            std::memcpy(pixels.get(), document.originalRgba.data(), document.originalRgba.size());
            pixels.get()[0] = static_cast<unsigned char>(index + 10); // 最终结果与可编辑底图不同，验证复制选择正确资产。
            HistoryImage image{pixels, width, height};
            HistoryDetails details; details.title = "项目截图 " + std::to_string(index + 1);
            details.sourceApplication = index % 2 ? "Chrome" : "Terminal";
            std::string error, id;
            auto generation = service->Store().RegisterCandidate(error); ASSERT_TRUE(generation);
            ASSERT_TRUE(service->Store().Write(*generation, document, image, "copy", error, details, &id)) << error;
            ids.push_back(id);
        }
    }
    bool Open() {
        SDL_Rect work{}; SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &work);
        if (!recent->Show({work.x + work.w - 32, work.y - 24, 24, 24})) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (recent->m_loading.valid() && std::chrono::steady_clock::now() < deadline) {
            recent->RenderFrame(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !recent->m_loading.valid();
    }
    size_t Count() const { return recent->m_snapshot.entries.size(); }
    std::string Id(size_t index) const { return recent->m_snapshot.entries[index].metadata.id; }
    void Key(SDL_Keycode key) {
        SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(recent->GetSDLWindow()); event.key.key = key;
        EXPECT_TRUE(recent->ProcessEvent(event));
        recent->RenderFrame();
    }
    void ClickFirst() {
        ImGuiUtils::ContextScope current(recent->GetImGuiContext());
        auto& io = ImGui::GetIO(); io.ConfigInputTrickleEventQueue = false;
        const ImVec2 point(150, 95);
        SDL_WarpMouseInWindow(recent->GetSDLWindow(), point.x, point.y);
        SDL_PumpEvents(); mouseWarped = true;
        io.AddMousePosEvent(point.x, point.y); io.AddMouseButtonEvent(0, false); recent->RenderFrame();
        io.AddMouseButtonEvent(0, true); recent->RenderFrame();
        io.AddMouseButtonEvent(0, false); recent->RenderFrame();
    }
    void ReadyPreview() {
        recent->m_previewDue = std::chrono::steady_clock::now();
        recent->RenderFrame();
        if (recent->m_loadingPreview.valid()) recent->m_loadingPreview.wait();
        recent->RenderFrame();
    }
    SDL_Window* PreviewWindow() { return recent->m_previewSurface.Window(); }
    ImGuiContext* PreviewContext() { return recent->m_previewSurface.Context(); }
    bool Busy() const { return recent->m_busy; }
    void ExpectRoundedWindow(SDL_Window* window, ImGuiContext* gui, const char* previewName) {
        SaveWindowFrame(device, window, gui, previewName);
        std::string error;
        const auto path = std::filesystem::path(SHOTLET_HISTORY_PREVIEW_DIR) / previewName;
        const auto image = ReadHistoryPng(path, error); ASSERT_TRUE(image) << error;
        const auto alpha = [&](int x, int y) { return image.pixels.get()[(size_t(y) * image.width + x) * 4 + 3]; };
        // 检查实际 GPU 输出的四角透明，而内部保持不透明，防止只改了样式却仍显示矩形背景。
        for (const int x : {1, image.width - 2})
            for (const int y : {1, image.height - 2}) EXPECT_EQ(alpha(x, y), 0);
        EXPECT_EQ(alpha(image.width / 2, image.height / 2), 255);
        const float density = SDL_GetWindowPixelDensity(window);
        const int radiusPixels = int(std::lround(PopupSurface::WindowCornerRadiusLogical * density));
        EXPECT_GT(alpha(radiusPixels, 1), 0); // Retina 半径只乘一次像素密度。
        NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        ASSERT_NE(native, nil);
        EXPECT_FALSE(native.opaque); EXPECT_TRUE(native.contentView.layer.masksToBounds);
        EXPECT_GT(native.contentView.layer.cornerRadius, 0);
    }
    size_t Selected() const { return size_t(recent->m_selected); }
    void Hover(size_t index) {
        ImGuiUtils::ContextScope current(recent->GetImGuiContext());
        const ImVec2 point(150, 95 + index * RecentCapturesLayout::RowHeight);
        SDL_WarpMouseInWindow(recent->GetSDLWindow(), point.x, point.y);
        SDL_PumpEvents(); mouseWarped = true;
        SDL_Event event{}; event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(recent->GetSDLWindow()); event.motion.x = point.x; event.motion.y = point.y;
        recent->ProcessEvent(event);
        ImGui::GetIO().AddMousePosEvent(point.x, point.y); recent->RenderFrame();
    }
    bool PreviewVisible() const { return recent->m_previewSurface.IsVisible(); }
    void ExpectAllRowsVisible() {
        recent->RenderFrame(); recent->RenderFrame();
        ImGuiUtils::ContextScope current(recent->GetImGuiContext());
        for (const auto* child : ImGui::GetCurrentContext()->Windows)
            if (std::strstr(child->Name, "recent-entries")) EXPECT_LE(child->ScrollMax.y, 1);
    }
    void BeginApplication() {
        recent.reset();
        application = std::make_unique<Application>();
        application->m_gpuDevice = device;
        application->m_history = std::move(service);
        application->m_language = Language::ChineseSimplified;
        application->m_running = true;
    }
    void WaitForApplicationCopy() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((application->m_recentCopy.valid() || application->ClipboardCopiesBusy()) &&
               std::chrono::steady_clock::now() < deadline) {
            application->ProcessRecentCaptures(); application->ProcessClipboardCopies();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_FALSE(application->m_recentCopy.valid()); EXPECT_FALSE(application->ClipboardCopiesBusy());
        EXPECT_EQ(application->m_historyWindow, nullptr);
    }
    void CopyLatest() {
        application->RequestRecentCopy({}, true);
        WaitForApplicationCopy();
    }
    void CopyFirstInApplication() {
        ASSERT_TRUE(application->EnsureRecentCaptures());
        auto& popup = *application->m_recentCaptures;
        ASSERT_TRUE(popup.Show({100, 60, 24, 24}));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (popup.m_loading.valid() && std::chrono::steady_clock::now() < deadline) {
            application->ProcessRecentCaptures(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN; event.key.key = SDLK_RETURN;
        event.key.windowID = SDL_GetWindowID(popup.GetSDLWindow()); popup.ProcessEvent(event);
        application->ProcessRecentCaptures(); WaitForApplicationCopy();
        popup.m_hideAfter = std::chrono::steady_clock::now(); popup.RenderFrame();
        EXPECT_FALSE(popup.IsVisible()); EXPECT_EQ(popup.m_focus, nullptr);
        popup.Close();
    }
    void ExpectCopiedPixel(unsigned char red) {
        NSData* data = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypePNG];
        ASSERT_NE(data, nil);
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr); ASSERT_NE(source, nullptr);
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr); ASSERT_NE(image, nullptr);
        EXPECT_EQ(CGImageGetWidth(image), 640u); EXPECT_EQ(CGImageGetHeight(image), 400u);
        std::vector<unsigned char> decoded(640 * 400 * 4);
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef bitmap = CGBitmapContextCreate(decoded.data(), 640, 400, 8, 640 * 4, color,
            static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
        ASSERT_NE(bitmap, nullptr); CGContextDrawImage(bitmap, CGRectMake(0, 0, 640, 400), image);
        EXPECT_EQ(decoded[0], red);
        CGContextRelease(bitmap); CGColorSpaceRelease(color); CGImageRelease(image); CFRelease(source);
    }
    size_t ApplicationRecordCount() { return application->m_history->Store().List().size(); }
    size_t ApplicationReservedBytes() { return application->m_history->ReservedBytes(); }
    bool ApplicationFeedbackVisible() { return application->m_recentCaptures && application->m_recentCaptures->IsVisible(); }
    NSStatusBarButton* FindTrayButton() {
        NSStatusBarButton* button = nil;
        std::function<void(NSView*)> findButton = [&](NSView* view) {
            if ([view isKindOfClass:NSStatusBarButton.class]) {
                auto* candidate = static_cast<NSStatusBarButton*>(view);
                if ([candidate.toolTip isEqualToString:@"Shotlet - 后台运行"]) button = candidate;
            }
            for (NSView* child in view.subviews) findButton(child);
        };
        for (NSWindow* candidate in NSApp.windows) findButton(candidate.contentView);
        return button;
    }
    void ExpectTrayPixel(NSStatusBarButton* button, const std::array<Uint8, 4>& expected) {
        CGImageRef image = [button.image CGImageForProposedRect:nullptr context:nil hints:nil];
        ASSERT_NE(image, nullptr);
        NSData* bytes = CFBridgingRelease(CGDataProviderCopyData(CGImageGetDataProvider(image)));
        ASSERT_GE(bytes.length, expected.size());
        const auto* pixel = static_cast<const Uint8*>(bytes.bytes);
        for (size_t channel = 0; channel < expected.size(); ++channel) EXPECT_EQ(pixel[channel], expected[channel]);
    }
    std::filesystem::path root;
    std::unique_ptr<HistoryService> service;
    std::unique_ptr<RecentCapturesWindow> recent;
    std::vector<std::string> ids;
    std::unique_ptr<Application> application;
    Method pasteboardMethod = nullptr;
    IMP originalPasteboard = nullptr;
    float originalMouseX = 0, originalMouseY = 0;
    bool mouseWarped = false;
};

TEST_F(RecentCapturesWindowTest, ShowsOnlyEightNewestActiveRecordsAndKeepsCopyOrderStable) {
    Seed(11);
    std::string error;
    ASSERT_TRUE(service->Store().Trash(ids.back(), error));
    ASSERT_TRUE(Open()); ASSERT_EQ(Count(), RecentCapturesWindow::MaxRecords);
    EXPECT_EQ(Id(0), ids[9]); EXPECT_EQ(Id(7), ids[2]);
    ExpectAllRowsVisible();
    ClickFirst();
    const auto requests = recent->TakeRequests(); ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].action, RecentCapturesWindow::Action::Copy);
    EXPECT_EQ(requests[0].id, ids[9]); EXPECT_TRUE(Busy());
    recent->SetCopyResult(true);
    EXPECT_EQ(Id(0), ids[9]);
}

TEST_F(RecentCapturesWindowTest, KeyboardPreviewStaysOnScreenAndRetinaSizeIsNotDoubled) {
    Seed(3); ASSERT_TRUE(Open());
    Key(SDLK_DOWN); EXPECT_EQ(Selected(), 1u);
    ReadyPreview(); ASSERT_NE(PreviewWindow(), nullptr);
    int width = 0, height = 0, pixelWidth = 0;
    SDL_GetWindowSize(recent->GetSDLWindow(), &width, &height);
    SDL_GetWindowSizeInPixels(recent->GetSDLWindow(), &pixelWidth, nullptr);
    EXPECT_EQ(width, RecentCapturesLayout::Width);
    EXPECT_EQ(pixelWidth, std::lround(width * SDL_GetWindowPixelDensity(recent->GetSDLWindow())));
    int x = 0, y = 0, relativeX = 0, relativeY = 0, previewWidth = 0;
    SDL_GetWindowPosition(recent->GetSDLWindow(), &x, &y);
    SDL_GetWindowPosition(PreviewWindow(), &relativeX, &relativeY);
    SDL_GetWindowSize(PreviewWindow(), &previewWidth, nullptr);
    EXPECT_LE(relativeX + previewWidth, 0); // 右侧空间不足，预览翻转到左侧。
    ExpectRoundedWindow(recent->GetSDLWindow(), recent->GetImGuiContext(), "recent-captures-dark.png");
    ExpectRoundedWindow(PreviewWindow(), PreviewContext(), "recent-preview-dark.png");
    recent->SetTheme(Theme::Light); recent->SetLanguage(Language::English); recent->RenderFrame();
    ExpectRoundedWindow(recent->GetSDLWindow(), recent->GetImGuiContext(), "recent-captures-light.png");
    ExpectRoundedWindow(PreviewWindow(), PreviewContext(), "recent-preview-light.png");
    Key(SDLK_RETURN);
    const auto requests = recent->TakeRequests(); ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].id, ids[1]);
    recent->SetCopyResult(false); EXPECT_FALSE(Busy()); EXPECT_TRUE(recent->IsVisible());
    Key(SDLK_ESCAPE); EXPECT_FALSE(recent->IsVisible());
}

TEST_F(RecentCapturesWindowTest, EmptyFeedbackAndClosingReleaseAllHistoryBudgets) {
    ASSERT_TRUE(Open()); EXPECT_EQ(Count(), 0u);
    recent->ShowFeedback({100, 100, 24, 24}, "已复制到剪贴板");
    EXPECT_TRUE(recent->IsVisible());
    EXPECT_TRUE(SDL_GetWindowFlags(recent->GetSDLWindow()) & SDL_WINDOW_NOT_FOCUSABLE);
    recent->Close(); EXPECT_EQ(service->ReservedBytes(), 0u);
    EXPECT_EQ(ImGui::GetCurrentContext(), context);
}

TEST_F(RecentCapturesWindowTest, HoverSelectsAnItemAndShowsPreviewAfterTheDelay) {
    Seed(3); ASSERT_TRUE(Open()); Hover(2);
    EXPECT_EQ(Selected(), 2u); EXPECT_FALSE(PreviewVisible());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!PreviewVisible() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); recent->RenderFrame();
    }
    EXPECT_TRUE(PreviewVisible());
    EXPECT_TRUE(SDL_GetWindowFlags(PreviewWindow()) & SDL_WINDOW_TOOLTIP);
}

TEST_F(RecentCapturesWindowTest, CopyingFromThePanelWorksWithoutOpeningTheHistoryWindowAndClosesOnSuccess) {
    Seed(3); BeginApplication(); CopyFirstInApplication(); ExpectCopiedPixel(12);
    EXPECT_EQ(ApplicationRecordCount(), 3u); EXPECT_EQ(ApplicationReservedBytes(), 0u);
}

TEST_F(RecentCapturesWindowTest, CopyLatestUsesFinalPngSkipsTrashAndDoesNotCreateDuplicateHistory) {
    Seed(3);
    std::string error; ASSERT_TRUE(service->Store().Trash(ids.back(), error));
    BeginApplication();
    CopyLatest(); ExpectCopiedPixel(11);
    EXPECT_EQ(ApplicationRecordCount(), 2u); EXPECT_EQ(ApplicationReservedBytes(), 0u);
    EXPECT_TRUE(ApplicationFeedbackVisible());
}

TEST_F(RecentCapturesWindowTest, CopyLatestWithNoHistoryPreservesTheExistingClipboard) {
    [[NSPasteboard generalPasteboard] clearContents];
    ASSERT_TRUE([[NSPasteboard generalPasteboard] setString:@"保留原有内容" forType:NSPasteboardTypeString]);
    BeginApplication(); CopyLatest();
    EXPECT_TRUE([[[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString] isEqualToString:@"保留原有内容"]);
    EXPECT_EQ(ApplicationReservedBytes(), 0u); EXPECT_TRUE(ApplicationFeedbackVisible());
}

TEST_F(RecentCapturesWindowTest, NativeTrayLeftClickAndMenuActionsAreDeferredAndCleanedUp) {
    SystemTray tray; ASSERT_TRUE(tray.Create(Language::ChineseSimplified, Theme::Dark));
    const auto anchor = tray.GetAnchor(); EXPECT_GT(anchor.w, 0); EXPECT_GT(anchor.h, 0);
    NSStatusBarButton* button = FindTrayButton();
    ASSERT_NE(button, nil);
    NSBitmapImageRep* bitmap = [button bitmapImageRepForCachingDisplayInRect:button.bounds];
    ASSERT_NE(bitmap, nil);
    [button cacheDisplayInRect:button.bounds toBitmapImageRep:bitmap];
    EXPECT_EQ(bitmap.pixelsWide, std::lround(button.bounds.size.width * button.window.backingScaleFactor));
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    const std::string iconPreview = std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/tray-icon-fixed.png";
    ASSERT_TRUE([png writeToFile:[NSString stringWithUTF8String:iconPreview.c_str()] atomically:YES]);
    [button performClick:nil];
    EXPECT_TRUE(tray.ConsumeRecentRequest()); EXPECT_FALSE(tray.ConsumeRecentRequest());
    // 检查本应用创建的原生菜单动作，不向桌面注入点击，也不进入阻塞的菜单追踪循环。
    NSMenu* menu = [button.target valueForKey:@"menu"];
    ASSERT_NE(menu, nil);
    for (NSMenuItem* item in menu.itemArray) {
        if (item.tag != static_cast<NSInteger>(TrayAction::CopyLatest)) continue;
        [menu performActionForItemAtIndex:[menu indexOfItem:item]];
    }
    EXPECT_TRUE(tray.ConsumeCopyLatestRequest()); EXPECT_FALSE(tray.IsQuitRequested());
    tray.UpdateIcon(Theme::Light);
    EXPECT_EQ(button.image.size.width, 22);
    tray.Destroy(); EXPECT_EQ(button.target, nil);
}

TEST_F(RecentCapturesWindowTest, NativeTrayImageOwnsPixelsAfterSourceReuseAndRepeatedClicks) {
    constexpr int iconPixels = 30;
    constexpr int clickCount = 20;
    const std::array<Uint8, 4> initialColor{241, 193, 83, 255};
    const std::array<Uint8, 4> updatedColor{93, 163, 231, 255};
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurface(iconPixels, iconPixels, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    ASSERT_NE(surface, nullptr);
    const auto fill = [&](const auto& color) {
        ASSERT_TRUE(SDL_FillSurfaceRect(surface.get(), nullptr,
            SDL_MapSurfaceRGBA(surface.get(), color[0], color[1], color[2], color[3])));
    };
    fill(initialColor);
    int clicks = 0;
    const std::unique_ptr<NativeSystemTray, decltype(&DestroyNativeSystemTray)> tray(
        CreateNativeSystemTray(surface.get(), Language::ChineseSimplified,
            [](void* data, TrayAction action) { if (action == TrayAction::Recent) ++*static_cast<int*>(data); }, &clicks),
        DestroyNativeSystemTray);
    ASSERT_NE(tray, nullptr);
    NSStatusBarButton* button = FindTrayButton(); ASSERT_NE(button, nil);
    // 确定性改写仍然有效的源内存，验证原生图像没有借用它；避免靠访问已释放内存来复现。
    fill((std::array<Uint8, 4>{0, 0, 0, 0}));
    ExpectTrayPixel(button, initialColor);
    fill(updatedColor);
    UpdateNativeSystemTrayIcon(tray.get(), surface.get());
    fill((std::array<Uint8, 4>{0, 0, 0, 0}));
    // 转换失败必须保留上一次有效图像，避免状态栏只剩空白按钮。
    UpdateNativeSystemTrayIcon(tray.get(), nullptr);
    ExpectTrayPixel(button, updatedColor);
    for (int click = 0; click < clickCount; ++click) {
        [button performClick:nil];
        ExpectTrayPixel(button, updatedColor);
    }
    EXPECT_EQ(clicks, clickCount);
}
} // namespace shotlet

TEST_F(ScrollCaptureGpuOutputTest, EditorTitlebarSharesNativeButtonsAndKeepsZoomClickableAtDifferentWidths) {
    ImGuiUtils::ContextScope restore(context);
    constexpr int imageWidth = 320, imageHeight = 240, rgbaChannels = 4;
    auto image = std::make_shared<ScrollImage>();
    image->width = imageWidth; image->height = imageHeight;
    image->rgba.assign(size_t(imageWidth) * imageHeight * rgbaChannels, 255);
    auto document = std::make_shared<CaptureDocument>();
    document->width = imageWidth; document->height = imageHeight;
    document->logicalSize = {imageWidth, imageHeight};
    document->originalRgba = image->rgba;
    ScreenOverlay::Config config;
    config.language = Language::ChineseSimplified;
    // 同时覆盖滚动截图结果和历史文档恢复两个真实入口。
    for (bool fromHistory : {false, true}) {
        auto editor = fromHistory ? std::make_unique<ImageEditor>(document, config, device)
                                  : std::make_unique<ImageEditor>(config, image, device);
        ASSERT_TRUE(editor->Create());
        SDL_Window* editorWindow = editor->GetSDLWindow();
        NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(editorWindow),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        ASSERT_NE(native, nil);
        EXPECT_TRUE(native.styleMask & NSWindowStyleMaskFullSizeContentView);
        EXPECT_EQ(native.titleVisibility, NSWindowTitleHidden);
        constexpr int editorHeight = 500;
        for (int width : {760, 1024}) {
            ASSERT_TRUE(SDL_SetWindowSize(editorWindow, width, editorHeight));
            SDL_SyncWindow(editorWindow);
            for (Theme theme : {Theme::Dark, Theme::Light}) {
                editor->SetTheme(theme);
                editor->RenderFrame(); editor->RenderFrame();
                ImGui::SetCurrentContext(editor->GetImGuiContext());
                auto* toolbar = ImGui::FindWindowByName("Toolbar");
                auto* viewport = ImGui::FindWindowByName("image_editor_viewport");
                ASSERT_NE(toolbar, nullptr); ASSERT_NE(viewport, nullptr);
                EXPECT_FLOAT_EQ(toolbar->Pos.y, 0);
                EXPECT_FLOAT_EQ(viewport->Pos.y, Toolbar::EditorHeight);
                EXPECT_LE(toolbar->DC.CursorMaxPos.x, width);
                EXPECT_NEAR(NSHeight(native.contentView.bounds), NSHeight(native.frame), 1);
                const float leadingInset = LayoutImageEditorTitlebar(editorWindow, Toolbar::EditorHeight);
                ASSERT_GT(leadingInset, 0);
                NSView* root = native.contentView.superview;
                for (NSWindowButton type : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
                    NSButton* button = [native standardWindowButton:type];
                    ASSERT_NE(button, nil); EXPECT_FALSE(button.hidden);
                    const NSRect rect = [button convertRect:button.bounds toView:native.contentView];
                    const double centerY = native.contentView.isFlipped ? NSMidY(rect)
                        : NSHeight(native.contentView.bounds) - NSMidY(rect);
                    EXPECT_NEAR(centerY, Toolbar::EditorHeight / 2, 1);
                    EXPECT_LT(NSMaxX(rect), leadingInset);
                    const NSPoint center = [button convertPoint:NSMakePoint(NSMidX(button.bounds), NSMidY(button.bounds)) toView:root];
                    NSView* hit = [root hitTest:center];
                    EXPECT_TRUE(hit == button || [hit isDescendantOf:button]);
                }
                const ImVec2 zoomOut(toolbar->WorkRect.Min.x + leadingInset + Toolbar::IconSize / 2,
                                     Toolbar::EditorHeight / 2);
                const NSPoint nativePoint = NSMakePoint(zoomOut.x, native.contentView.isFlipped ? zoomOut.y
                    : NSHeight(native.contentView.bounds) - zoomOut.y);
                NSView* hit = [root hitTest:[native.contentView convertPoint:nativePoint toView:root]];
                EXPECT_TRUE(hit == native.contentView || [hit isDescendantOf:native.contentView]);
                const float zoomBeforeClick = editor->GetViewTransform().zoom;
                ClickWindow(*editor, zoomOut.x, zoomOut.y);
                editor->RenderFrame(); // 缩放回调在工具栏绘制后执行，下一帧才应用到视图。
                EXPECT_LT(editor->GetViewTransform().zoom, zoomBeforeClick);
                const std::string preview = std::string(fromHistory ? "history-editor" : "scroll-editor") +
                    (theme == Theme::Dark ? "-dark-" : "-light-") + std::to_string(width) + ".png";
                SaveWindowFrame(device, editorWindow, editor->GetImGuiContext(), preview);
            }
        }
    }
}

TEST_F(ScrollCaptureGpuOutputTest, EditorViewActionIconsStayStableWhileDrawingAnnotations) {
    ImGuiUtils::ContextScope restore(context);
    constexpr int imageWidth = 320, imageHeight = 240, rgbaChannels = 4;
    auto image = std::make_shared<ScrollImage>();
    image->width = imageWidth; image->height = imageHeight;
    image->rgba.assign(size_t(imageWidth) * imageHeight * rgbaChannels, 255);
    ScreenOverlay::Config config;
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    ImGui::SetCurrentContext(editor.GetImGuiContext());

    const auto viewActionColors = [&]() {
        const auto* toolbar = ImGui::FindWindowByName("Toolbar");
        std::vector<ImU32> colors;
        if (!toolbar) return colors;
        const auto selectedColor = GetThemePalette(config.theme).selectedBackground;
        float annotationLeft = toolbar->Pos.x + toolbar->Size.x;
        // 选中的标注工具位于视图操作组之后，以实际绘制位置截取前方的图标。
        for (const auto& vertex : toolbar->DrawList->VtxBuffer)
            if (vertex.col == selectedColor) annotationLeft = std::min(annotationLeft, vertex.pos.x);
        const float nativeButtonsRight = LayoutImageEditorTitlebar(editor.GetSDLWindow(), Toolbar::EditorHeight);
        for (const auto& vertex : toolbar->DrawList->VtxBuffer)
            if (vertex.pos.x >= nativeButtonsRight && vertex.pos.x < annotationLeft) colors.push_back(vertex.col);
        return colors;
    };
    const auto pointerEvent = [&](Uint32 type, ImVec2 imagePoint) {
        const auto position = editor.GetViewTransform().ToScreen(imagePoint);
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(editor.GetSDLWindow());
            event.motion.x = position.x; event.motion.y = position.y;
        } else {
            event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow());
            event.button.button = SDL_BUTTON_LEFT;
            event.button.x = position.x; event.button.y = position.y;
        }
        editor.ProcessEvent(event);
        editor.RenderFrame();
    };
    for (Theme theme : {Theme::Dark, Theme::Light}) {
        config.theme = theme;
        editor.SetTheme(theme);
        for (ToolType tool : {ToolType::Rectangle, ToolType::Brush}) {
            SCOPED_TRACE(testing::Message() << "theme=" << static_cast<int>(theme) << ", tool=" << static_cast<int>(tool));
            auto* canvas = editor.GetCanvas();
            canvas->SetTool(tool);
            editor.RenderFrame(); editor.RenderFrame();
            const auto idleColors = viewActionColors();
            ASSERT_FALSE(idleColors.empty());
            const auto layersBefore = canvas->GetLayerManager().GetLayers().size();
            pointerEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, ImVec2(120, 100));
            EXPECT_EQ(viewActionColors(), idleColors) << "落笔时图标变色";
            pointerEvent(SDL_EVENT_MOUSE_MOTION, ImVec2(160, 130));
            EXPECT_EQ(viewActionColors(), idleColors) << "绘制中图标变色";
            if (tool == ToolType::Rectangle)
                SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
                    theme == Theme::Dark ? "editor-drawing-dark.png" : "editor-drawing-light.png");
            pointerEvent(SDL_EVENT_MOUSE_BUTTON_UP, ImVec2(160, 130));
            EXPECT_EQ(viewActionColors(), idleColors) << "收笔时图标变色";
            ASSERT_GT(canvas->GetLayerManager().GetLayers().size(), layersBefore);
            // 每轮删除本次笔迹，避免下一工具的落点命中已有图层而进入拖动。
            EXPECT_TRUE(canvas->DeleteLayerById(canvas->GetLayerManager().GetLayers().back()->GetId()));
        }
    }
}

TEST_F(ScrollCaptureGpuOutputTest, WindowsTitlebarControlsShareOneRowWithEditorTools) {
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(320, 240), ImVec2(320, 240), nullptr, 1280);
    Toolbar toolbar;
    toolbar.SetCanvas(&canvas);
    toolbar.SetEditorMode(true);
    toolbar.SetWindowActionCallback([](Toolbar::WindowAction) {});
    constexpr float windowWidth = 760;
    for (Theme theme : {Theme::Dark, Theme::Light}) {
        toolbar.SetTheme(theme);
        for (int frame = 0; frame < 2; ++frame) {
            ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame();
            ImGui::GetIO().DisplaySize = ImVec2(windowWidth, Toolbar::EditorHeight);
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            ImGui::NewFrame();
            toolbar.Render(); toolbar.RenderWindowControls();
            ImGui::Render();
        }
        const auto* tools = ImGui::FindWindowByName("Toolbar");
        const auto* controls = ImGui::FindWindowByName("EditorWindowControls");
        ASSERT_NE(tools, nullptr); ASSERT_NE(controls, nullptr);
        EXPECT_FLOAT_EQ(controls->Pos.x, tools->Size.x);
        EXPECT_FLOAT_EQ(controls->Size.y, Toolbar::EditorHeight);
        EXPECT_FLOAT_EQ(toolbar.GetSelectedIconSize(), Toolbar::IconSize);
        SaveWindowFrame(device, window, context, theme == Theme::Dark ? "windows-titlebar-dark.png" : "windows-titlebar-light.png");
    }
}

TEST(HistoryIndexTest, WorkerRecognizesExistingScreenshotsAndMakesTextSearchable) {
    auto root=std::filesystem::temp_directory_path()/("shotlet-history-ocr-"+NewHistoryId());
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}}cleanup{root};
    HistoryService service(root);CaptureDocument document;document.width=640;document.height=160;document.logicalSize={640,160};
    document.createdAtUnixMs=HistoryNow();document.originalRgba.assign(640*160*4,255);
    CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
    CGContextRef bitmap=CGBitmapContextCreate(document.originalRgba.data(),640,160,8,640*4,color,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast)|kCGBitmapByteOrder32Big);
    ASSERT_NE(bitmap,nullptr);CTFontRef font=CTFontCreateWithName(CFSTR("Helvetica"),40,nullptr);
    NSDictionary* attributes=@{(__bridge NSString*)kCTFontAttributeName:(__bridge id)font,
        (__bridge NSString*)kCTForegroundColorAttributeName:(__bridge id)NSColor.blackColor.CGColor};
    NSAttributedString* text=[[NSAttributedString alloc]initWithString:@"SEARCHABLE HISTORY" attributes:attributes];
    CTLineRef line=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text);
    CGContextSetTextPosition(bitmap,24,70);CTLineDraw(line,bitmap);
    CFRelease(line);CFRelease(font);CGContextRelease(bitmap);CGColorSpaceRelease(color);
    auto pixels=std::shared_ptr<const unsigned char>(document.originalRgba.data(),[](const unsigned char*){});
    std::string error;auto generation=service.Store().RegisterCandidate(error);ASSERT_TRUE(generation);
    ASSERT_TRUE(service.Store().Write(*generation,document,{pixels,640,160},"save",error));
    const auto id=service.Store().List()[0].id;service.RequestIndex(id,true);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    std::optional<HistoryMetadata> indexed;
    while(std::chrono::steady_clock::now()<deadline){
        indexed=service.Store().Get(id,error);if(indexed&&indexed->details.ocrIndexed)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(indexed);ASSERT_TRUE(indexed->details.ocrIndexed);EXPECT_TRUE(indexed->details.ocrError.empty())<<indexed->details.ocrError;
    EXPECT_NE(indexed->details.ocrText.find("HISTORY"),std::string::npos)<<indexed->details.ocrText;
    HistoryQuery query;query.search="history";EXPECT_EQ(QueryHistory(service.Store().List(),query,HistoryNow()).size(),1u);
    service.Drain();EXPECT_EQ(service.ReservedBytes(),0u);
}

TEST_F(ScrollCaptureGpuOutputTest, HistoryBrowserGridListMultiSelectSearchAndMenusUseRealEvents) {
    struct RestoreContext{ImGuiContext* value;~RestoreContext(){ImGui::SetCurrentContext(value);}}restore{context};
    auto root=std::filesystem::temp_directory_path()/("shotlet-history-browser-"+NewHistoryId());
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}}cleanup{root};
    HistoryService service(root);std::string error;
    const char* types[]={"region","window","scroll","fullscreen"};
    for(int i=0;i<8;++i){
        CaptureDocument d;d.width=320;d.height=240;d.logicalSize={320,240};d.createdAtUnixMs=HistoryDayStart(HistoryNow(),i<4?0:-1)+(10*60+i)*60*1000;
        d.originalRgba.resize(320*240*4);
        for(int y=0;y<240;++y)for(int x=0;x<320;++x){
            auto* p=d.originalRgba.data()+(y*320+x)*4;p[0]=30+i*4;p[1]=35+i*3;p[2]=40+i*6;p[3]=255;
            if(x>28&&x<260&&((y>65&&y<85)||(y>105&&y<125))){p[0]=i%2?40:20;p[1]=i%2?65:55;p[2]=i%2?110:170;}
        }
        HistoryDetails details;details.captureType=types[i%4];details.sourceApplication="Chrome";details.sourceWindow="需求评审.pdf";
        details.title="截图 "+HistoryTime(d.createdAtUnixMs,"%Y-%m-%d %H.%M.%S");details.tags={i%2?"评审":"设计稿"};
        details.favorite=i==0;details.ocrIndexed=true;details.ocrText=i==0?"支持时间、类型、标签筛选历史记录":"截图中的其他文字";
        auto generation=service.Store().RegisterCandidate(error);ASSERT_TRUE(generation);
        auto pixels=std::shared_ptr<const unsigned char>(d.originalRgba.data(),[](const unsigned char*){});
        ASSERT_TRUE(service.Store().Write(*generation,d,{pixels,320,240},"copy",error,details))<<error;
    }
    HistoryWindow history(device,service,Language::ChineseSimplified,Theme::Dark);ASSERT_TRUE(history.Show());
    history.RenderFrame();history.RenderFrame();ImGui::SetCurrentContext(history.GetImGuiContext());
    auto child=[&](const char* name)->ImGuiWindow*{for(auto* w:ImGui::GetCurrentContext()->Windows)if(std::strstr(w->Name,name))return w;return nullptr;};
    auto* sidebar=child("/history_sidebar_");auto* browser=child("/history_browser_");auto* details=child("/history_detail_");
    ASSERT_NE(sidebar,nullptr);ASSERT_NE(browser,nullptr);ASSERT_NE(details,nullptr);
    EXPECT_NEAR(sidebar->Size.x,208,1);EXPECT_NEAR(browser->Pos.x,208,1);EXPECT_NEAR(details->Pos.x,1000,1);
    EXPECT_LE(browser->ContentSize.x,browser->InnerRect.GetWidth()+1);
    SaveHistoryFrame(device,history,"build/history/tests/history-grid-preview.png");
    auto click=[&](float x,float y,int button=ImGuiMouseButton_Left){
        ClickHistory(history,x,y,button);
    };
    auto hoverCards=[&](bool list){
        auto* ctx=ImGui::GetCurrentContext();auto& io=ImGui::GetIO();
        // 保持后端的鼠标捕获状态，避免系统鼠标位置覆盖测试输入；ImGui 内保持未按下。
        SDL_Event capture{};capture.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
        capture.button.windowID=SDL_GetWindowID(history.GetSDLWindow());capture.button.button=SDL_BUTTON_LEFT;
        history.ProcessEvent(capture);io.ConfigInputTrickleEventQueue=false;
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);io.AddMousePosEvent(900,25);
        history.RenderFrame();history.RenderFrame();
        const auto content=browser->ContentSize;const float scrollMax=browser->ScrollMax.y;
        const auto records=service.Store().List();std::vector<ImGuiID> cardIds;
        for(const auto& record:records){
            const auto seed=ImHashStr(record.id.c_str(),0,browser->IDStack.back());
            cardIds.push_back(ImHashStr("##history_card",0,seed));
        }
        int hovered=0;ImVec2 lastCard;float lastWidth=0;std::string lastId;
        for(int page=0;page<(list?2:1);++page){
            const int columns=list?1:4;const float height=list?86:148;
            const float line=ImGui::GetTextLineHeightWithSpacing(),spacing=ImGui::GetStyle().ItemSpacing.y;
            const float cardWidth=(browser->WorkRect.GetWidth()-14*(columns-1))/columns;
            for(int i=0;i<8;++i){
                const float x=browser->DC.CursorStartPos.x+(i%4%columns)*(cardWidth+14)+30;
                const float y=browser->DC.CursorStartPos.y+line+(i/4)*((4/columns)*(height+spacing)+10+spacing+line)
                    +(i%4/columns)*(height+spacing)+30;
                if(!browser->InnerClipRect.Contains(ImVec2(x,y)))continue;
                io.AddMousePosEvent(x,y);
                for(int frame=0;frame<3;++frame){
                    history.RenderFrame();
                    ASSERT_EQ(ctx->ErrorCountCurrentFrame,0)<<"列表="<<list<<"，卡片="<<i;
                    EXPECT_NEAR(browser->ContentSize.x,content.x,.01f);
                    EXPECT_NEAR(browser->ContentSize.y,content.y,.01f);
                    EXPECT_NEAR(browser->ScrollMax.y,scrollMax,.01f);
                }
                EXPECT_EQ(ctx->HoveredWindow,browser);
                const auto found=std::find(cardIds.begin(),cardIds.end(),ctx->HoveredId);
                ASSERT_NE(found,cardIds.end());
                lastCard=ImVec2(x-30,y-30);lastWidth=cardWidth;lastId=records[found-cardIds.begin()].id;
                ++hovered;
            }
            if(list&&page==0){
                EXPECT_GT(browser->ScrollMax.y,0);
                io.AddMouseWheelEvent(0,-20);history.RenderFrame();history.RenderFrame();
                EXPECT_GT(browser->Scroll.y,0);
            }
        }
        EXPECT_GE(hovered,8);EXPECT_TRUE(history.TakeRequests().empty());
        SaveHistoryFrame(device,history,list?"history-list-hover-preview.png":"history-grid-hover-preview.png");
        capture.type=SDL_EVENT_MOUSE_BUTTON_UP;history.ProcessEvent(capture);
        // 缩略图收藏按钮独立响应；收藏切换不改变当前选中的截图。
        auto selectedId=[&](){
            SDL_Event event{};event.type=SDL_EVENT_KEY_DOWN;event.key.windowID=SDL_GetWindowID(history.GetSDLWindow());event.key.key=SDLK_C;event.key.mod=SDL_KMOD_CTRL;
            history.ProcessEvent(event);event.type=SDL_EVENT_KEY_UP;event.key.mod=0;history.ProcessEvent(event);
            const auto requests=history.TakeRequests();EXPECT_EQ(requests.size(),1u);
            if(requests.size()!=1)return std::string();
            EXPECT_EQ(requests[0].action,HistoryWindow::Action::Copy);return requests[0].id;
        };
        const auto selectedBefore=selectedId();
        const ImVec2 favorite(lastCard.x+(list?84:lastWidth-18),lastCard.y+18);
        auto original=service.Store().Get(lastId,error);ASSERT_TRUE(original);
        for(int toggle=0;toggle<2;++toggle){
            click(favorite.x,favorite.y);
            const auto requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
            EXPECT_EQ(requests[0].action,HistoryWindow::Action::Favorite);EXPECT_EQ(requests[0].id,lastId);
            EXPECT_EQ(requests[0].enabled,toggle?original->details.favorite:!original->details.favorite);
            ASSERT_TRUE(service.Store().UpdateDetails(lastId,[&](HistoryDetails& d){d.favorite=requests[0].enabled;},error))<<error;
            history.Invalidate();history.RenderFrame();
            EXPECT_EQ(selectedId(),selectedBefore);
            click(900,25);
            SaveHistoryFrame(device,history,list?(toggle?"history-list-favorite-hidden.png":"history-list-favorite-filled.png"):
                (toggle?"history-grid-favorite-hidden.png":"history-grid-favorite-filled.png"));
            EXPECT_TRUE(history.TakeRequests().empty());
        }
        // 悬停按钮仍可点击，且每次只对当前卡片生成一个操作请求。
        const TextId labels[]={TextId::HistoryQuickCopy,TextId::HistoryQuickAnnotate,TextId::HistoryFavorite,TextId::HistoryDelete};
        const HistoryWindow::Action actions[]={HistoryWindow::Action::Copy,HistoryWindow::Action::Edit,HistoryWindow::Action::Favorite,HistoryWindow::Action::Delete};
        float rowWidth=30;float widths[4];
        for(int i=0;i<4;++i){widths[i]=ImGui::CalcTextSize(Tr(Language::ChineseSimplified,labels[i])).x+4;rowWidth+=widths[i];}
        float buttonX=lastCard.x+(lastWidth-rowWidth)*.5f;
        const float buttonY=lastCard.y+(list?86-2:148-32)-17;
        for(int i=0;i<4;++i){
            const ImVec2 point(buttonX+widths[i]*.5f,buttonY);
            click(point.x,point.y);
            EXPECT_EQ(ctx->ErrorCountCurrentFrame,0);
            const auto requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
            EXPECT_EQ(requests[0].action,actions[i]);EXPECT_EQ(requests[0].id,lastId);
            if(i==2){
                auto record=service.Store().Get(lastId,error);ASSERT_TRUE(record);
                const bool original=record->details.favorite;
                EXPECT_EQ(requests[0].enabled,!original);
                ASSERT_TRUE(service.Store().UpdateDetails(lastId,[&](HistoryDetails& d){d.favorite=requests[0].enabled;},error))<<error;
                history.Invalidate();history.RenderFrame();
                click(point.x,point.y);
                const auto undo=history.TakeRequests();ASSERT_EQ(undo.size(),1u);
                EXPECT_EQ(undo[0].action,HistoryWindow::Action::Favorite);EXPECT_EQ(undo[0].id,lastId);EXPECT_EQ(undo[0].enabled,original);
                ASSERT_TRUE(service.Store().UpdateDetails(lastId,[&](HistoryDetails& d){d.favorite=undo[0].enabled;},error))<<error;
                history.Invalidate();history.RenderFrame();
            }
            buttonX+=widths[i]+10;
        }
    };
    hoverCards(false);
    // 用真实鼠标输入切换列表和网格；不移动系统鼠标。
    click(1254,25);history.RenderFrame();EXPECT_LE(browser->ContentSize.x,browser->InnerRect.GetWidth()+1);
    hoverCards(true);
    SaveHistoryFrame(device,history,"build/history/tests/history-list-preview.png");
    click(1220,25);history.RenderFrame();
    SDL_Event key{};key.type=SDL_EVENT_KEY_DOWN;key.key.windowID=SDL_GetWindowID(history.GetSDLWindow());key.key.key=SDLK_A;key.key.mod=SDL_KMOD_CTRL;
    click(80,112);history.ProcessEvent(key);history.RenderFrame();
    key.key.key=SDLK_DELETE;key.key.mod=0;history.ProcessEvent(key);
    auto favoriteRequests=history.TakeRequests();ASSERT_EQ(favoriteRequests.size(),1u);EXPECT_EQ(favoriteRequests[0].ids.size(),1u);
    key.key.key=SDLK_ESCAPE;history.ProcessEvent(key);history.RenderFrame();click(80,80);
    key.key.key=SDLK_A;key.key.mod=SDL_KMOD_CTRL;
    ASSERT_TRUE(history.ProcessEvent(key));history.RenderFrame();EXPECT_NE(child("/history_selection_header_"),nullptr);
    EXPECT_EQ(child("/history_selection_bar_"),nullptr);
    // 松开全选快捷键，macOS 上仍按住 Ctrl 的左键会被 ImGui 解释为右键。
    SDL_Event release=key;release.type=SDL_EVENT_KEY_UP;release.key.mod=0;history.ProcessEvent(release);
    history.RenderFrame();history.RenderFrame();
    SaveHistoryFrame(device,history,"build/history/tests/history-multi-preview.png");
    const float multiCardWidth=(browser->WorkRect.GetWidth()-42)/4;
    click(browser->DC.CursorStartPos.x+3*(multiCardWidth+14)+multiCardWidth-18,
        browser->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+18);
    auto multiFavorite=history.TakeRequests();ASSERT_EQ(multiFavorite.size(),1u)
        <<"卡片宽度="<<multiCardWidth<<" 起点="<<browser->DC.CursorStartPos.x<<","<<browser->DC.CursorStartPos.y
        <<" 鼠标="<<ImGui::GetIO().MousePos.x<<","<<ImGui::GetIO().MousePos.y<<" hovered="<<ImGui::GetCurrentContext()->HoveredId;
    EXPECT_EQ(multiFavorite[0].action,HistoryWindow::Action::Favorite);EXPECT_FALSE(multiFavorite[0].enabled);
    SaveHistoryFrame(device,history,"build/history/tests/history-multi-preview.png");
    key.key.key=SDLK_DELETE;key.key.mod=0;history.ProcessEvent(key);
    auto requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);EXPECT_EQ(requests[0].action,HistoryWindow::Action::Delete);EXPECT_EQ(requests[0].ids.size(),8u);
    key.key.key=SDLK_ESCAPE;history.ProcessEvent(key);history.RenderFrame();EXPECT_TRUE(history.IsVisible());
    click(400,25);ImGui::GetIO().AddInputCharactersUTF8("标签筛选");history.RenderFrame();
    // 输入框中的复制/删除应交由文字编辑处理，不生成截图操作。
    key.key.key=SDLK_C;key.key.mod=SDL_KMOD_CTRL;history.ProcessEvent(key);
    key.key.key=SDLK_DELETE;key.key.mod=0;history.ProcessEvent(key);EXPECT_TRUE(history.TakeRequests().empty());
    click(900,500);key.key.key=SDLK_A;key.key.mod=SDL_KMOD_CTRL;history.ProcessEvent(key);history.RenderFrame();
    key.key.key=SDLK_DELETE;key.key.mod=0;history.ProcessEvent(key);
    auto searchRequests=history.TakeRequests();ASSERT_EQ(searchRequests.size(),1u);EXPECT_EQ(searchRequests[0].ids.size(),1u);
    key.key.key=SDLK_ESCAPE;history.ProcessEvent(key);history.RenderFrame();
    click(300,220,ImGuiMouseButton_Right);history.RenderFrame();
    ImGui::SetCurrentContext(history.GetImGuiContext());
    EXPECT_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    SaveHistoryFrame(device,history,"build/history/tests/history-context-preview.png");
    history.Close();ASSERT_TRUE(service.Store().Clear(error));ASSERT_TRUE(history.Show());history.RenderFrame();history.RenderFrame();
    SaveHistoryFrame(device,history,"build/history/tests/history-empty-preview.png");
    history.SetTheme(Theme::Light);history.RenderFrame();
    SaveHistoryFrame(device,history,"build/history/tests/history-empty-light-preview.png");
    browser=child("/history_browser_");ASSERT_NE(browser,nullptr);
    click(browser->Pos.x+browser->Size.x*.5f,browser->Pos.y+std::max(80.f,browser->Size.y*.25f)+192);
    EXPECT_TRUE(history.TakeRequests().empty());
}

TEST_F(ScrollCaptureGpuOutputTest, HistorySelectionHeaderSupportsEmptyPartialAllAndFilteredSelection) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root=std::filesystem::temp_directory_path()/("shotlet-history-selection-"+NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec;std::filesystem::remove_all(path,ec); } } cleanup{root};
    HistoryService service(root);std::string error;
    for(int i=0;i<6;++i){
        CaptureDocument document;document.width=320;document.height=240;document.logicalSize={320,240};document.createdAtUnixMs=HistoryNow()-i*1000;
        document.originalRgba.resize(320*240*4);
        for(size_t pixel=0;pixel<document.originalRgba.size();pixel+=4){
            document.originalRgba[pixel]=30+i*15;document.originalRgba[pixel+1]=45+i*8;document.originalRgba[pixel+2]=55+i*20;document.originalRgba[pixel+3]=255;
        }
        HistoryDetails details;details.ocrIndexed=true;details.favorite=i%2;details.captureType="region";
        auto generation=service.Store().RegisterCandidate(error);ASSERT_TRUE(generation);
        auto pixels=std::shared_ptr<const unsigned char>(document.originalRgba.data(),[](const unsigned char*){});
        ASSERT_TRUE(service.Store().Write(*generation,document,{pixels,320,240},"copy",error,details))<<error;
    }
    HistoryWindow history(device,service,Language::ChineseSimplified,Theme::Dark);ASSERT_TRUE(history.Show());
    history.RenderFrame();history.RenderFrame();ImGui::SetCurrentContext(history.GetImGuiContext());
    auto* ctx=ImGui::GetCurrentContext();
    auto child=[&](const char* name)->ImGuiWindow*{for(auto* w:ctx->Windows)if(std::strstr(w->Name,name))return w;return nullptr;};
    auto* header=child("/history_selection_header_");auto* browser=child("/history_browser_");
    ASSERT_NE(header,nullptr);ASSERT_NE(browser,nullptr);
    auto frames=[&]{history.RenderFrame();history.RenderFrame();EXPECT_EQ(ctx->ErrorCountCurrentFrame,0);};
    auto all=[&]{ClickHistory(history,header->Pos.x+20,header->Pos.y+19);frames();};
    auto button=[&](TextId label){ImGui::ActivateItemByID(header->GetID(Tr(Language::ChineseSimplified,label)));frames();};
    auto point=[&](int index){
        const float width=(browser->WorkRect.GetWidth()-42)/4;
        return ImVec2(browser->DC.CursorStartPos.x+(index%4)*(width+14)+30,
            browser->DC.CursorStartPos.y+ImGui::GetTextLineHeightWithSpacing()+(index/4)*(148+ImGui::GetStyle().ItemSpacing.y)+40);
    };
    auto modifiedClick=[&](int index,SDL_Keycode key,SDL_Keymod mod){
        SDL_Event event{};event.type=SDL_EVENT_KEY_DOWN;event.key.windowID=SDL_GetWindowID(history.GetSDLWindow());event.key.key=key;event.key.mod=mod;
        history.ProcessEvent(event);const auto p=point(index);ClickHistory(history,p.x,p.y);
        event.type=SDL_EVENT_KEY_UP;event.key.mod=0;history.ProcessEvent(event);frames();
    };
    std::vector<std::string> selectedIds;
    auto count=[&](){
        SDL_Event event{};event.type=SDL_EVENT_KEY_DOWN;event.key.windowID=SDL_GetWindowID(history.GetSDLWindow());event.key.key=SDLK_DELETE;
        history.ProcessEvent(event);event.type=SDL_EVENT_KEY_UP;history.ProcessEvent(event);frames();
        const auto requests=history.TakeRequests();selectedIds.clear();
        if(requests.empty())return size_t(0);
        EXPECT_EQ(requests.size(),1u);EXPECT_EQ(requests[0].action,HistoryWindow::Action::Delete);
        selectedIds=requests[0].ids;return selectedIds.size();
    };
    SaveHistoryFrame(device,history,"history-selection-default.png");
    // 默认只有顶部全选栏，普通焦点仍支持单张操作；进入选择模式后全选当前结果。
    EXPECT_EQ(count(),1u);all();EXPECT_EQ(count(),6u);EXPECT_EQ(browser->ScrollMax.y,0);
    EXPECT_FLOAT_EQ(header->Size.y,38);EXPECT_EQ(child("/history_selection_bar_"),nullptr);
    // 原底部的导出、删除和拼接入口均迁入顶部，目标仍是全部勾选项。
    button(TextId::HistoryExport);auto requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::Export);EXPECT_EQ(requests[0].ids.size(),6u);
    button(TextId::HistoryDelete);requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::Delete);EXPECT_EQ(requests[0].ids.size(),6u);
    history.SetBusy(true);button(TextId::HistoryExport);EXPECT_TRUE(history.TakeRequests().empty());history.SetBusy(false);
    button(TextId::HistoryStitch);ASSERT_FALSE(ctx->OpenPopupStack.empty());
    auto* dialog=ctx->OpenPopupStack.back().Window;ASSERT_NE(dialog,nullptr);
    EXPECT_NE(std::strstr(dialog->Name,"history_stitch_dialog"),nullptr);
    ImGui::ActivateItemByID(dialog->GetID(Tr(Language::ChineseSimplified,TextId::HistoryCancel)));frames();
    EXPECT_TRUE(ctx->OpenPopupStack.empty());
    SaveHistoryFrame(device,history,"history-selection-all.png");
    button(TextId::HistoryClearSelection);EXPECT_EQ(count(),0u);
    SaveHistoryFrame(device,history,"history-selection-none.png");
    button(TextId::HistoryDone);EXPECT_EQ(count(),1u);
    // 完成后没有历史范围锚点，首次 Shift 点击仍应从当前焦点延伸选择。
    modifiedClick(2,SDLK_LSHIFT,SDL_KMOD_SHIFT);EXPECT_EQ(count(),3u);
    SaveHistoryFrame(device,history,"history-selection-partial.png");
    all();EXPECT_EQ(count(),6u);all();EXPECT_EQ(count(),0u);
    button(TextId::HistoryDone);
    const auto first=point(0);ClickHistory(history,first.x,first.y);
    modifiedClick(1,SDLK_LGUI,SDL_KMOD_GUI);EXPECT_EQ(count(),2u);
    // 切换筛选后清空勾选，全选只针对筛选出的 3 条收藏记录。
    ClickHistory(history,80,112);frames();EXPECT_EQ(count(),0u);all();EXPECT_EQ(count(),3u);
    for(const auto& id:selectedIds){auto record=service.Store().Get(id,error);ASSERT_TRUE(record);EXPECT_TRUE(record->details.favorite);}
    SaveHistoryFrame(device,history,"history-selection-filtered.png");
    button(TextId::HistoryDone);ClickHistory(history,1220,25);frames();all();EXPECT_EQ(count(),3u);
    SaveHistoryFrame(device,history,"history-selection-list.png");
    button(TextId::HistoryDone);EXPECT_EQ(count(),1u);
    history.SetTheme(Theme::Light);all();EXPECT_EQ(count(),3u);
    SaveHistoryFrame(device,history,"history-selection-light.png");
    // 最小窗口宽度下，所有操作仍位于选择栏内部，不覆盖列表或互相重叠。
    ASSERT_TRUE(SDL_SetWindowSize(history.GetSDLWindow(),1000,820));frames();
    button(TextId::HistoryExport);requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].ids.size(),3u);
    ImRect exportRect=header->NavRectRel[ImGuiNavLayer_Main];exportRect.Translate(header->Pos);
    EXPECT_TRUE(header->InnerRect.Contains(exportRect));EXPECT_LE(header->ContentSize.x,header->InnerRect.GetWidth());
    EXPECT_GE(browser->Pos.y,header->Pos.y+header->Size.y);
    SaveHistoryFrame(device,history,"history-selection-narrow.png");
    // 最近删除中的恢复、永久删除也统一使用顶部选择栏，永久删除仍需确认。
    for(const auto& id:selectedIds)ASSERT_TRUE(service.Store().Trash(id,error))<<error;
    history.Invalidate();frames();
    auto* sidebar=child("/history_sidebar_");ASSERT_NE(sidebar,nullptr);
    ClickHistory(history,sidebar->Pos.x+60,sidebar->Pos.y+sidebar->Size.y-112);frames();all();
    button(TextId::HistoryRestore);requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::Restore);
    EXPECT_EQ(std::set<std::string>(requests[0].ids.begin(),requests[0].ids.end()),std::set<std::string>(selectedIds.begin(),selectedIds.end()));
    button(TextId::HistoryPurge);EXPECT_TRUE(history.TakeRequests().empty());ASSERT_FALSE(ctx->OpenPopupStack.empty());
    dialog=ctx->OpenPopupStack.back().Window;ASSERT_NE(dialog,nullptr);
    ImGui::ActivateItemByID(dialog->GetID(Tr(Language::ChineseSimplified,TextId::HistoryConfirm)));frames();
    requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::Purge);
    EXPECT_EQ(std::set<std::string>(requests[0].ids.begin(),requests[0].ids.end()),std::set<std::string>(selectedIds.begin(),selectedIds.end()));
    SaveHistoryFrame(device,history,"history-selection-trash.png");
    history.Close();
    HistoryWindow english(device,service,Language::English,Theme::Dark);ASSERT_TRUE(english.Show());
    ASSERT_TRUE(SDL_SetWindowSize(english.GetSDLWindow(),1000,820));english.RenderFrame();english.RenderFrame();
    ImGui::SetCurrentContext(english.GetImGuiContext());ctx=ImGui::GetCurrentContext();header=child("/history_selection_header_");ASSERT_NE(header,nullptr);
    ClickHistory(english,header->Pos.x+20,header->Pos.y+19);english.RenderFrame();english.RenderFrame();
    ImGui::ActivateItemByID(header->GetID(Tr(Language::English,TextId::HistoryExport)));english.RenderFrame();english.RenderFrame();
    requests=english.TakeRequests();ASSERT_EQ(requests.size(),1u);EXPECT_EQ(requests[0].ids.size(),3u);
    EXPECT_LE(header->ContentSize.x,header->InnerRect.GetWidth());EXPECT_EQ(header->ScrollMax.x,0);
    SaveHistoryFrame(device,english,"history-selection-narrow-english.png");
}

TEST_F(ScrollCaptureGpuOutputTest, HistoryTagsSaveInlineOnEnterBlurAndSelectionChangeAndDeleteFromCorner) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root=std::filesystem::temp_directory_path()/("shotlet-history-tags-"+NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec;std::filesystem::remove_all(path,ec); } } cleanup{root};
    HistoryService service(root);std::string error;
    for(int i=0;i<2;++i){
        CaptureDocument document;document.width=320;document.height=240;document.logicalSize={320,240};
        document.createdAtUnixMs=HistoryNow()-i*1000;document.originalRgba.assign(320*240*4,255);
        HistoryDetails details;details.ocrIndexed=true;details.tags={"已有标签"};
        auto generation=service.Store().RegisterCandidate(error);ASSERT_TRUE(generation);
        auto pixels=std::shared_ptr<const unsigned char>(document.originalRgba.data(),[](const unsigned char*){});
        ASSERT_TRUE(service.Store().Write(*generation,document,{pixels,320,240},"copy",error,details))<<error;
    }
    const auto records=service.Store().List();
    HistoryWindow history(device,service,Language::ChineseSimplified,Theme::Dark);ASSERT_TRUE(history.Show());
    history.RenderFrame();history.RenderFrame();ImGui::SetCurrentContext(history.GetImGuiContext());
    auto* ctx=ImGui::GetCurrentContext();
    auto child=[&](const char* name)->ImGuiWindow*{for(auto* w:ctx->Windows)if(std::strstr(w->Name,name))return w;return nullptr;};
    auto* info=child("/history_detail_info_");ASSERT_NE(info,nullptr);
    auto key=[&](SDL_Keycode code){
        SDL_Event event{};event.type=SDL_EVENT_KEY_DOWN;event.key.windowID=SDL_GetWindowID(history.GetSDLWindow());event.key.key=code;
        history.ProcessEvent(event);history.RenderFrame();event.type=SDL_EVENT_KEY_UP;history.ProcessEvent(event);history.RenderFrame();
    };
    auto start=[&]{
        ImGui::ActivateItemByID(info->GetID(Tr(Language::ChineseSimplified,TextId::HistoryAddTag)));
        history.RenderFrame();history.RenderFrame();history.RenderFrame();
        EXPECT_EQ(ctx->ActiveId,info->GetID("##new_history_tag"));
        EXPECT_TRUE(ctx->OpenPopupStack.empty());
    };
    auto type=[&](const char* text){ImGui::GetIO().AddInputCharactersUTF8(text);history.RenderFrame();};
    auto apply=[&](const HistoryWindow::Request& request){
        const auto ids=request.ids.empty()?std::vector<std::string>{request.id}:request.ids;
        for(const auto& id:ids)ASSERT_TRUE(service.Store().UpdateDetails(id,[&](HistoryDetails& details){
            if(request.action==HistoryWindow::Action::AddTag)details.tags.push_back(request.value);
            else std::erase(details.tags,request.value);
        },error))<<error;
        history.Invalidate();history.RenderFrame();history.RenderFrame();
    };
    start();type("  回车保存  ");SaveHistoryFrame(device,history,"history-tag-input-preview.png");key(SDLK_RETURN);
    auto requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::AddTag);EXPECT_EQ(requests[0].value,"回车保存");
    EXPECT_EQ(requests[0].ids,std::vector<std::string>{records[0].id});apply(requests[0]);
    EXPECT_NE(ctx->ActiveId,info->GetID("##new_history_tag"));EXPECT_TRUE(ctx->OpenPopupStack.empty());
    start();type("点击其他位置保存");ClickHistory(history,900,600);
    requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);EXPECT_EQ(requests[0].value,"点击其他位置保存");apply(requests[0]);
    EXPECT_NE(ctx->ActiveId,info->GetID("##new_history_tag"));
    // 切换截图时提交到最初的截图，不能把标签加到新选中的截图。
    start();type("切换截图保存");ClickHistory(history,450,200);
    requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].ids,std::vector<std::string>{records[0].id});EXPECT_EQ(requests[0].value,"切换截图保存");apply(requests[0]);
    auto second=service.Store().Get(records[1].id,error);ASSERT_TRUE(second);EXPECT_EQ(second->details.tags,std::vector<std::string>{"已有标签"});
    start();type("   ");ClickHistory(history,900,600);EXPECT_TRUE(history.TakeRequests().empty());
    start();type("取消输入");key(SDLK_ESCAPE);EXPECT_TRUE(history.TakeRequests().empty());EXPECT_TRUE(history.IsVisible());
    // 标签正文保留筛选功能；红叉位于同一标签右上角，点击只删除当前截图的标签。
    const auto tagId=ImHashStr("##history_tag",0,ImHashStr("已有标签",0,info->IDStack.back()));
    ImGui::ActivateItemByID(tagId);history.RenderFrame();history.RenderFrame();
    ASSERT_EQ(ctx->NavId,tagId);
    auto tagRect=info->NavRectRel[ImGuiNavLayer_Main];tagRect.Translate(info->Pos);
    // 删除角标超出胶囊右上角，外侧区域也要能点击，且不能误触标签筛选。
    ClickHistory(history,tagRect.Max.x+2,tagRect.Min.y-2);
    requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::RemoveTag);EXPECT_EQ(requests[0].id,records[1].id);EXPECT_EQ(requests[0].value,"已有标签");
    SaveHistoryFrame(device,history,"history-tag-remove-preview.png");apply(requests[0]);
    second=service.Store().Get(records[1].id,error);ASSERT_TRUE(second);EXPECT_TRUE(second->details.tags.empty());
    auto first=service.Store().Get(records[0].id,error);ASSERT_TRUE(first);EXPECT_EQ(first->details.tags.size(),4u);
    // 多选工具栏直接编辑，提交时保留最初的批量目标。
    ClickHistory(history,80,80);
    SDL_Event selectAll{};selectAll.type=SDL_EVENT_KEY_DOWN;selectAll.key.windowID=SDL_GetWindowID(history.GetSDLWindow());
    selectAll.key.key=SDLK_A;selectAll.key.mod=SDL_KMOD_CTRL;history.ProcessEvent(selectAll);history.RenderFrame();
    selectAll.type=SDL_EVENT_KEY_UP;selectAll.key.mod=0;history.ProcessEvent(selectAll);history.RenderFrame();
    auto* bar=child("/history_selection_header_");ASSERT_NE(bar,nullptr);
    ImGui::ActivateItemByID(bar->GetID(Tr(Language::ChineseSimplified,TextId::HistoryTags)));
    history.RenderFrame();history.RenderFrame();history.RenderFrame();
    ASSERT_EQ(ctx->ActiveId,bar->GetID("##new_history_tag"));EXPECT_TRUE(ctx->OpenPopupStack.empty());
    type("批量标签");key(SDLK_RETURN);requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);
    EXPECT_EQ(requests[0].action,HistoryWindow::Action::AddTag);EXPECT_EQ(requests[0].ids.size(),2u);EXPECT_EQ(requests[0].value,"批量标签");apply(requests[0]);
    key(SDLK_ESCAPE);
    // 右键菜单的新标签也原位输入，关闭菜单时保存，不再创建标签对话框。
    ClickHistory(history,260,200,ImGuiMouseButton_Right);history.RenderFrame();
    ASSERT_FALSE(ctx->OpenPopupStack.empty());auto* menu=ctx->OpenPopupStack.back().Window;ASSERT_NE(menu,nullptr);
    ImGui::ActivateItemByID(menu->GetID(Tr(Language::ChineseSimplified,TextId::HistoryTagManage)));
    history.RenderFrame();history.RenderFrame();history.RenderFrame();
    ASSERT_GE(ctx->OpenPopupStack.size(),2);auto* submenu=ctx->OpenPopupStack.back().Window;ASSERT_NE(submenu,nullptr);
    ImGui::ActivateItemByID(submenu->GetID(Tr(Language::ChineseSimplified,TextId::HistoryAddTag)));
    history.RenderFrame();history.RenderFrame();history.RenderFrame();
    ASSERT_EQ(ctx->ActiveId,submenu->GetID("##new_history_tag"));
    type("菜单标签");SaveHistoryFrame(device,history,"history-tag-menu-input-preview.png");ClickHistory(history,900,25);
    requests=history.TakeRequests();ASSERT_EQ(requests.size(),1u);EXPECT_EQ(requests[0].value,"菜单标签");
    EXPECT_EQ(requests[0].ids,std::vector<std::string>{records[0].id});apply(requests[0]);
    EXPECT_TRUE(ctx->OpenPopupStack.empty());
    EXPECT_EQ(ctx->ErrorCountCurrentFrame,0);
}

TEST_F(ScrollCaptureGpuOutputTest, HistoryFontMatchesNativeSettingsAtRetinaDensity) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root = std::filesystem::temp_directory_path() / ("shotlet-history-font-" + NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
    HistoryService service(root);
    HistoryWindow history(device, service, Language::ChineseSimplified, Theme::Dark);
    ASSERT_TRUE(history.Show());
    auto checkFont = [&] {
        history.RenderFrame();
        ImGui::SetCurrentContext(history.GetImGuiContext());
        const auto& io = ImGui::GetIO();
        int logicalWidth = 0, logicalHeight = 0, pixelWidth = 0, pixelHeight = 0;
        ASSERT_TRUE(SDL_GetWindowSize(history.GetSDLWindow(), &logicalWidth, &logicalHeight));
        ASSERT_TRUE(SDL_GetWindowSizeInPixels(history.GetSDLWindow(), &pixelWidth, &pixelHeight));
        // 字体按逻辑尺寸对齐，Retina 密度只应用于帧缓冲，避免重复放大。
        EXPECT_NEAR(io.DisplayFramebufferScale.x, float(pixelWidth) / logicalWidth, .01f);
        EXPECT_NEAR(io.DisplayFramebufferScale.y, float(pixelHeight) / logicalHeight, .01f);
        const CGFloat nativeWidth = [@"截图历史" sizeWithAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:13]}].width;
        EXPECT_NEAR(ImGui::CalcTextSize("截图历史").x, nativeWidth, 2.0f);
    };
    checkFont();
    history.SetTheme(Theme::Light); checkFont();
    history.Close(); ASSERT_TRUE(history.Show()); checkFont();
}

TEST_F(ScrollCaptureGpuOutputTest, HistoryWindowHidesImmediatelyAndPreparesFrameBeforeRestoring) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root = std::filesystem::temp_directory_path() / ("shotlet-history-visibility-" + NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
    HistoryService service(root);
    HistoryWindow history(device, service, Language::ChineseSimplified, Theme::Dark);
    ASSERT_TRUE(history.Show());
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(history.GetSDLWindow()),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    ASSERT_NE(native, nil);
    EXPECT_EQ(native.animationBehavior, NSWindowAnimationBehaviorNone);
    auto frameCount = [&] {
        ImGui::SetCurrentContext(history.GetImGuiContext());
        const auto* draw = ImGui::GetDrawData();
        EXPECT_NE(draw, nullptr);
        if (draw) EXPECT_GT(draw->TotalVtxCount, 0);
        const int count = ImGui::GetFrameCount();
        ImGui::SetCurrentContext(context);
        return count;
    };
    int previousFrame = frameCount();
    for (int i = 0; i < 3; ++i) {
        history.Hide();
        EXPECT_FALSE(history.IsVisible());
        EXPECT_FALSE(native.isVisible);
        EXPECT_EQ(native.alphaValue, 0);
        history.RenderFrame();
        EXPECT_TRUE(history.Show());
        EXPECT_TRUE(native.isVisible);
        EXPECT_EQ(native.alphaValue, 1);
        const int currentFrame = frameCount();
        EXPECT_GT(currentFrame, previousFrame);
        previousFrame = currentFrame;
    }
}

TEST_F(ScrollCaptureGpuOutputTest, IdleHistoryWindowKeepsCachedMetadataUntilNotified) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root = std::filesystem::temp_directory_path() / ("shotlet-history-refresh-" + NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    HistoryService service(root);
    std::string error;
    CaptureDocument document; document.width = document.height = 8; document.logicalSize = {8, 8};
    document.createdAtUnixMs = HistoryNow(); document.originalRgba.assign(8 * 8 * 4, 255);
    const auto pixels = std::shared_ptr<const unsigned char>(document.originalRgba.data(), [](const unsigned char*) {});
    auto generation = service.Store().RegisterCandidate(error);
    ASSERT_TRUE(generation);
    HistoryDetails details; details.ocrIndexed = true;
    std::string id;
    ASSERT_TRUE(service.Store().Write(*generation, document, {pixels, 8, 8}, "copy", error, details, &id)) << error;
    HistoryWindow history(device, service, Language::ChineseSimplified, Theme::Dark);
    ASSERT_TRUE(history.Show());
    history.RenderFrame();
    const auto metadata = root / "history" / id / "meta.json", saved = root / "saved-meta.json";
    std::filesystem::rename(metadata, saved);
    struct RestoreMetadata {
        std::filesystem::path saved, metadata;
        ~RestoreMetadata() { std::error_code ec; std::filesystem::rename(saved, metadata, ec); }
    } restoreMetadata{saved, metadata};
    // 暂存元数据不会发布应用变更；跨过旧刷新周期后，窗口仍应使用缓存。
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    history.RenderFrame();
    SDL_Event copy{}; copy.type = SDL_EVENT_KEY_DOWN;
    copy.key.windowID = SDL_GetWindowID(history.GetSDLWindow()); copy.key.key = SDLK_C; copy.key.mod = SDL_KMOD_CTRL;
    ASSERT_TRUE(history.ProcessEvent(copy));
    auto requests = history.TakeRequests(); ASSERT_EQ(requests.size(), 1u); EXPECT_EQ(requests.front().id, id);
    std::filesystem::rename(saved, metadata);
    // 另一存储实例提交删除后，无需显式 Invalidate，下一帧就应刷新选中记录。
    HistoryStore external(root); ASSERT_TRUE(external.Delete(id, error)) << error;
    history.RenderFrame();
    ASSERT_TRUE(history.ProcessEvent(copy)); EXPECT_TRUE(history.TakeRequests().empty());
}

TEST_F(ScrollCaptureGpuOutputTest, HistoryWindowRoutesOnlyItsEventsAndReleasesResourcesOnClose) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root = std::filesystem::temp_directory_path() / ("shotlet-history-ui-" + NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
    HistoryService service(root);
    auto pixels = std::shared_ptr<unsigned char>(new unsigned char[320 * 240 * 4], std::default_delete<unsigned char[]>());
    std::memset(pixels.get(), 220, 320 * 240 * 4);
    auto candidate = service.Prepare(nullptr, {pixels, 320, 240}, "copy"); ASSERT_TRUE(candidate);
    service.Finish(std::move(candidate)); service.Drain();
    HistoryWindow history(device, service, Language::ChineseSimplified, Theme::Dark);
    ASSERT_TRUE(history.Show()); EXPECT_TRUE(history.IsVisible());
    history.RenderFrame(); EXPECT_EQ(ImGui::GetCurrentContext(), context);
    SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN; event.key.windowID = SDL_GetWindowID(window); event.key.key = SDLK_C; event.key.mod = SDL_KMOD_CTRL;
    EXPECT_FALSE(history.ProcessEvent(event));
    event.key.windowID = SDL_GetWindowID(history.GetSDLWindow()); EXPECT_TRUE(history.ProcessEvent(event));
    auto requests = history.TakeRequests(); ASSERT_EQ(requests.size(), 1u); EXPECT_EQ(requests[0].action, HistoryWindow::Action::Copy);
    history.SetTheme(Theme::Light); history.RenderFrame(); EXPECT_EQ(ImGui::GetCurrentContext(), context);
    history.Hide(); EXPECT_FALSE(history.IsVisible()); ASSERT_TRUE(history.Show());
    event = {}; event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED; event.window.windowID = SDL_GetWindowID(history.GetSDLWindow());
    EXPECT_TRUE(history.ProcessEvent(event)); history.RenderFrame(); EXPECT_EQ(history.GetSDLWindow(), nullptr);
    ASSERT_TRUE(history.Show()); history.RenderFrame();
}

TEST_F(ScrollCaptureGpuOutputTest, HistoryCopyToastDoesNotStealFocusAndExpires) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto root=std::filesystem::temp_directory_path()/("shotlet-history-toast-"+NewHistoryId());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path,ec); } } cleanup{root};
    HistoryService service(root);
    HistoryWindow history(device,service,Language::ChineseSimplified,Theme::Dark);ASSERT_TRUE(history.Show());
    history.RenderFrame();ImGui::SetCurrentContext(history.GetImGuiContext());
    auto* ctx=ImGui::GetCurrentContext();auto* focused=ctx->NavWindow;
    history.ShowToast(Tr(Language::ChineseSimplified,TextId::HistoryCopyDone));history.RenderFrame();history.RenderFrame();
    auto* toast=ImGui::FindWindowByName("history_copy_toast");ASSERT_NE(toast,nullptr);
    EXPECT_TRUE(toast->Active);EXPECT_FALSE(toast->Hidden);EXPECT_EQ(ctx->NavWindow,focused);
    EXPECT_EQ(toast->Flags&ImGuiWindowFlags_NoInputs,ImGuiWindowFlags_NoInputs);
    SaveHistoryFrame(device,history,"history-copy-toast-preview.png");
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));history.RenderFrame();
    EXPECT_FALSE(toast->Active);EXPECT_EQ(ctx->ErrorCountCurrentFrame,0);
    // 后续错误应清除成功提示，关闭窗口后也不能在重新打开时显示旧提示。
    history.ShowToast(Tr(Language::ChineseSimplified,TextId::HistoryCopyDone));
    history.SetError("测试复制失败");history.RenderFrame();EXPECT_FALSE(toast->Active);
    history.ShowToast(Tr(Language::ChineseSimplified,TextId::HistoryCopyDone));history.RenderFrame();EXPECT_TRUE(toast->Active);
    ImGui::SetCurrentContext(context);history.Close();ASSERT_TRUE(history.Show());history.RenderFrame();
    ImGui::SetCurrentContext(history.GetImGuiContext());
    toast=ImGui::FindWindowByName("history_copy_toast");EXPECT_TRUE(!toast||!toast->Active);
}

TEST_F(ScrollCaptureGpuOutputTest, RestoredMosaicCanBeSelectedAndDeletedDirectlyInRotatedEditor) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    auto image = std::make_shared<ScrollImage>(); image->width = 320; image->height = 240;
    image->rgba.resize(320 * 240 * 4);
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) {
        auto* p = image->rgba.data() + (y * 320 + x) * 4;
        p[0] = x % 256; p[1] = y; p[2] = (x + y) % 256; p[3] = 255;
    }
    ScreenOverlay::Config config;
    ImageEditor source(config, image, device); ASSERT_TRUE(source.Create());
    auto* canvas = source.GetCanvas(); canvas->SetTool(ToolType::Mosaic);
    canvas->OnMouseDown(ImVec2(60, 100)); canvas->OnMouseMove(ImVec2(180, 130)); canvas->OnMouseUp(ImVec2(180, 130));
    canvas->GetLayerManager().AddLayer(std::make_unique<RectangleLayer>(ImVec2(240, 160), ImVec2(290, 210), IM_COL32_WHITE, 2));
    std::string error;
    auto document = canvas->ExportDocument(error); ASSERT_TRUE(document) << error;
    const auto mosaicId = document->layers[0].id;
    for (int turns = 0; turns < 4; ++turns) {
        SCOPED_TRACE(turns);
        auto rotated = std::make_shared<CaptureDocument>(*document); rotated->quarterTurns = turns;
        ImageEditor editor(rotated, config, device); ASSERT_TRUE(editor.Create());
        auto* restored = editor.GetCanvas(); restored->SetUIScale(turns % 2 ? 2 : 1);
        editor.RenderFrame();
        ImGui::SetCurrentContext(editor.GetImGuiContext());
        const auto* panel = ImGui::FindWindowByName("editor_layers");
        EXPECT_EQ(panel, nullptr);
        auto click = [&](ImVec2 point, bool drag) {
            const auto& view = editor.GetViewTransform();
            const auto start = view.ToScreen(point), end = view.ToScreen(ImVec2(point.x + 15, point.y + 15));
            SDL_Event event{}; event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow()); event.button.button = SDL_BUTTON_LEFT;
            event.button.x = start.x; event.button.y = start.y; editor.ProcessEvent(event);
            if (drag) {
                event = {}; event.type = SDL_EVENT_MOUSE_MOTION;
                event.motion.windowID = SDL_GetWindowID(editor.GetSDLWindow());
                event.motion.x = end.x; event.motion.y = end.y; editor.ProcessEvent(event);
            }
            event = {}; event.type = SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow()); event.button.button = SDL_BUTTON_LEFT;
            event.button.x = drag ? end.x : start.x; event.button.y = drag ? end.y : start.y; editor.ProcessEvent(event);
        };
        // 普通标注直接点选后显示原有属性工具栏。
        click(ImVec2(265, 160), false); editor.RenderFrame();
        EXPECT_EQ(restored->GetPropertyTool(), ToolType::Rectangle);
        const auto* properties = ImGui::FindWindowByName("SubToolbar");
        ASSERT_NE(properties, nullptr); EXPECT_TRUE(properties->Active);
        // 马赛克选中仅显示边框；拖动不移动，且隐藏属性工具栏。
        const std::vector<uint8_t> before(restored->GetCurrentImageData(), restored->GetCurrentImageData() + image->rgba.size());
        click(ImVec2(120, 115), true); editor.RenderFrame();
        EXPECT_EQ(restored->GetSelectedLayerId(), mosaicId);
        EXPECT_FALSE(properties->Active);
        EXPECT_EQ(std::memcmp(before.data(), restored->GetCurrentImageData(), before.size()), 0);
        SDL_Event key{}; key.type = SDL_EVENT_KEY_DOWN; key.key.windowID = SDL_GetWindowID(editor.GetSDLWindow());
        key.key.key = turns % 2 ? SDLK_BACKSPACE : SDLK_DELETE; editor.ProcessEvent(key);
        editor.RenderFrame();
        EXPECT_FALSE(restored->HasSelectedLayer());
        ASSERT_EQ(restored->GetLayerManager().GetLayers().size(), 1u);
        EXPECT_STREQ(restored->GetLayerManager().GetLayers()[0]->GetTypeName(), "Rectangle");
        EXPECT_EQ(std::memcmp(restored->GetCurrentImageData(), document->originalRgba.data(), document->originalRgba.size()), 0);
        ImGui::SetCurrentContext(context);
    }
}

TEST_F(ScrollCaptureGpuOutputTest, TopCropButtonCancelsAndDoneAppliesCropWithoutCompletingCapture) {
    ImGuiUtils::ContextScope restore(context);
    constexpr int ImageWidth = 320, ImageHeight = 240, RgbaChannels = 4;
    constexpr float ViewActionCount = 5, CropIndex = ViewActionCount - 1, SeparatorWidth = 1;
    constexpr float ConfirmRightInset = 16, ConfirmButtonWidth = 50;
    auto image = std::make_shared<ScrollImage>();
    image->width = ImageWidth; image->height = ImageHeight;
    image->rgba.assign(size_t(ImageWidth) * ImageHeight * RgbaChannels, 255);
    ScreenOverlay::Config config;
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    editor.GetCanvas()->SetTool(ToolType::Rectangle);
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    editor.RenderFrame();
    const auto* toolbar = ImGui::FindWindowByName("Toolbar");
    ASSERT_NE(toolbar, nullptr);
    // 按真实绘制的第一个选中标注图标定位操作组，避免把 macOS 原生按钮避让宽度写死。
    float annotationLeft = toolbar->Pos.x + toolbar->Size.x;
    const auto selectedBackground = GetThemePalette(config.theme).selectedBackground;
    for (const auto& vertex : toolbar->DrawList->VtxBuffer)
        if (vertex.col == selectedBackground) annotationLeft = std::min(annotationLeft, vertex.pos.x);
    const float left = toolbar->Pos.x + toolbar->WindowPadding.x +
        LayoutImageEditorTitlebar(editor.GetSDLWindow(), Toolbar::EditorHeight);
    const float gap = (annotationLeft - left - ViewActionCount * Toolbar::IconSize - SeparatorWidth) / (ViewActionCount + 1);
    const ImVec2 cropButton(left + CropIndex * (Toolbar::IconSize + gap) + Toolbar::IconSize / 2,
                           toolbar->Pos.y + toolbar->Size.y / 2);
    const ImVec2 doneButton(toolbar->Pos.x + toolbar->Size.x - ConfirmRightInset - ConfirmButtonWidth / 2, cropButton.y);
    ClickWindow(editor, cropButton.x, cropButton.y);
    ASSERT_TRUE(editor.IsCropping());
    ClickWindow(editor, cropButton.x, cropButton.y);
    EXPECT_FALSE(editor.IsCropping());
    EXPECT_FALSE(editor.HasResult());
    ClickWindow(editor, cropButton.x, cropButton.y);
    ASSERT_TRUE(editor.IsCropping());
    const ImRect crop(ImVec2(40, 30), ImVec2(240, 180));
    const auto start = editor.GetViewTransform().ToScreen(crop.Min), end = editor.GetViewTransform().ToScreen(crop.Max);
    SDL_Event event{}; event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow()); event.button.button = SDL_BUTTON_LEFT;
    event.button.x = start.x; event.button.y = start.y; editor.ProcessEvent(event);
    event = {}; event.type = SDL_EVENT_MOUSE_MOTION; event.motion.windowID = SDL_GetWindowID(editor.GetSDLWindow());
    event.motion.x = end.x; event.motion.y = end.y; editor.ProcessEvent(event);
    event = {}; event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow()); event.button.button = SDL_BUTTON_LEFT;
    event.button.x = end.x; event.button.y = end.y; editor.ProcessEvent(event);
    editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "editor-crop-toolbar-active.png");
    ClickWindow(editor, doneButton.x, doneButton.y);
    EXPECT_FALSE(editor.IsCropping());
    EXPECT_FALSE(editor.HasResult());
    EXPECT_FALSE(editor.ShouldClose());
    EXPECT_FLOAT_EQ(editor.GetCanvas()->GetScreenshotSize().x, crop.GetWidth());
    EXPECT_FLOAT_EQ(editor.GetCanvas()->GetScreenshotSize().y, crop.GetHeight());
    editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "editor-crop-toolbar-applied.png");
}

TEST_F(ScrollCaptureGpuOutputTest, CropConfirmsAndCancelsThroughRotatedZoomedEditorMouseEvents) {
    struct RestoreContext { ImGuiContext* value; ~RestoreContext() { ImGui::SetCurrentContext(value); } } restore{context};
    for (int turns = 0; turns < 4; ++turns) {
        auto image = std::make_shared<ScrollImage>(); image->width = 320; image->height = 240;
        image->rgba.resize(320 * 240 * 4);
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) {
            auto* p = image->rgba.data() + (y * 320 + x) * 4;
            p[0] = x % 256; p[1] = y; p[2] = (x + y) % 256; p[3] = 255;
        }
        ScreenOverlay::Config config; ImageEditor editor(config, image, device); ASSERT_TRUE(editor.Create());
        auto* canvas = editor.GetCanvas(); canvas->SetTool(ToolType::Mosaic);
        canvas->OnMouseDown(ImVec2(60, 60)); canvas->OnMouseMove(ImVec2(180, 130)); canvas->OnMouseUp(ImVec2(180, 130));
        canvas->SetTool(ToolType::None);
        canvas->SetWatermarkText("CropWM");
        for (int i = 0; i < turns; ++i) canvas->RotateImageCounterClockwise();
        editor.RenderFrame();
        const auto originalSelection = canvas->GetSelection();
        const auto id = canvas->GetLayerManager().GetLayers()[0]->GetId();
        const ImRect crop(ImVec2(50.375f, 60.625f), ImVec2(250.625f, 190.875f));
        auto drag = [&] {
            const auto& view = editor.GetViewTransform();
            const auto start = view.ToScreen(crop.Min), end = view.ToScreen(crop.Max);
            SDL_Event event{}; event.type = SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow());
            event.button.button = SDL_BUTTON_LEFT; event.button.x = start.x; event.button.y = start.y; editor.ProcessEvent(event);
            event.type = SDL_EVENT_MOUSE_MOTION; event.motion.windowID = SDL_GetWindowID(editor.GetSDLWindow());
            event.motion.x = end.x; event.motion.y = end.y; editor.ProcessEvent(event);
            event = {}; event.type = SDL_EVENT_MOUSE_BUTTON_UP; event.button.windowID = SDL_GetWindowID(editor.GetSDLWindow());
            event.button.button = SDL_BUTTON_LEFT; event.button.x = end.x; event.button.y = end.y; editor.ProcessEvent(event);
        };
        editor.BeginCrop(); drag(); EXPECT_TRUE(editor.IsCropping()); editor.CancelCrop();
        EXPECT_FALSE(editor.IsCropping()); EXPECT_EQ(canvas->GetSelection().Max.x, originalSelection.Max.x);
        EXPECT_EQ(canvas->GetScreenshotSize().x, 320);
        editor.BeginCrop(); drag();
        EXPECT_NEAR(editor.GetCropRect().Min.x, crop.Min.x, .001f); EXPECT_NEAR(editor.GetCropRect().Max.y, crop.Max.y, .001f);
        // 导出以鼠标逆变换得到的同一矩形为基准，避免把坐标量化差误判为内容变化。
        auto expected = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(), canvas, editor.GetCropRect(), 1);
        ASSERT_TRUE(expected.data);
        std::string error; ASSERT_TRUE(editor.ApplyCrop(error)) << error; EXPECT_FALSE(editor.IsCropping()); editor.RenderFrame();
        auto* cropped = editor.GetCanvas(); EXPECT_EQ(cropped->GetImageQuarterTurns(), turns);
        EXPECT_EQ(cropped->GetWatermarkText(), "CropWM");
        EXPECT_LE(cropped->GetScreenshotSize().x, 201); EXPECT_LE(cropped->GetScreenshotSize().y, 131);
        ASSERT_EQ(cropped->GetLayerManager().GetLayers().size(), 1u); EXPECT_EQ(cropped->GetLayerManager().GetLayers()[0]->GetId(), id);
        auto actual = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(), cropped, cropped->GetSelection(), 1);
        ASSERT_TRUE(actual.data); ASSERT_EQ(actual.width, expected.width); ASSERT_EQ(actual.height, expected.height);
        int maximumDifference = 0;
        for (size_t i = 0; i < size_t(actual.width) * actual.height * 4; ++i)
            maximumDifference = std::max(maximumDifference, std::abs(int(actual.data[i]) - int(expected.data[i])));
        EXPECT_LE(maximumDifference, 1) << "旋转=" << turns;
        ASSERT_TRUE(cropped->DeleteLayerById(id));
        auto snapshot = cropped->ExportDocument(error); ASSERT_TRUE(snapshot) << error;
        EXPECT_EQ(std::memcmp(snapshot->originalRgba.data(), cropped->GetCurrentImageData(), snapshot->originalRgba.size()), 0);
        ImGui::SetCurrentContext(context);
    }
}

TEST(StructuredOcrTest, InvalidInputFailsAndEmptyImageSucceedsWithoutText) {
    auto invalid = OcrService::RecognizeDetailedAsync(nullptr, 0, 0).get(); EXPECT_FALSE(invalid.success); EXPECT_FALSE(invalid.error.empty());
    std::vector<unsigned char> white(200 * 100 * 4, 255);
    auto empty = OcrService::RecognizeDetailedAsync(white.data(), 200, 100).get();
    EXPECT_TRUE(empty.success) << empty.error; EXPECT_TRUE(empty.text.empty());
}

TEST_F(ScrollCaptureGpuOutputTest, NativeScrollMaskRendersTransparentSelectionAndTranslucentOutside) {
    // 检查实际绘制后的像素，而非仅检查窗口或图层的 opaque 属性。
    ShotletScrollMaskView* view = [[ShotletScrollMaskView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    view.selectionRect = NSMakeRect(20, 20, 60, 60);
    NSBitmapImageRep* pixels = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    ASSERT_NE(pixels, nil);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:pixels];
    EXPECT_NEAR([[pixels colorAtX:50 y:50] alphaComponent], 0.0, 0.01);
    EXPECT_NEAR([[pixels colorAtX:5 y:5] alphaComponent], 0.45, 0.02);
    // 选区改变和重绘后也不能遗留上次的黑色遮罩。
    view.selectionRect = NSMakeRect(0, 0, 100, 100);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:pixels];
    EXPECT_NEAR([[pixels colorAtX:5 y:5] alphaComponent], 0.0, 0.01);
}

TEST_F(ScrollCaptureGpuOutputTest, ReverseWarningIsRedAtSelectionTopLeftAndClears) {
    ShotletScrollMaskView* view = [[ShotletScrollMaskView alloc] initWithFrame:NSMakeRect(0, 0, 400, 200)];
    // 选区贴近屏幕顶部时，警告仍须可见。
    view.selectionRect = NSMakeRect(20, 20, 340, 180);
    view.reverseScrollBlocked = YES;
    NSBitmapImageRep* pixels = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    ASSERT_NE(pixels, nil);
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:pixels];
    const auto redPixelCount = [&] {
        int count = 0;
        for (int y = 0; y < 40; ++y) {
            for (int x = 20; x < 160; ++x) {
                NSColor* color = [[pixels colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
                if (color.alphaComponent > 0.2 && color.redComponent > 0.7 &&
                    color.greenComponent < 0.5 && color.blueComponent < 0.5) ++count;
            }
        }
        return count;
    };
    EXPECT_GT(redPixelCount(), 20);
    EXPECT_NEAR([[pixels colorAtX:200 y:100] alphaComponent], 0.0, 0.01);
    view.reverseScrollBlocked = NO;
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:pixels];
    EXPECT_EQ(redPixelCount(), 0);
}

TEST_F(ScrollCaptureGpuOutputTest, FailedOverlayPreparationDoesNotLeakWindow) {
    // 测试进程没有 Application 共享 GPU，真实 PreCreate 应失败且立即销毁部分窗口。
    int before = 0;
    SDL_Window** windows = SDL_GetWindows(&before);
    SDL_free(windows);
    ScreenOverlay overlay;
    ScreenOverlay::Config config;
    config.windowWidth = 320;
    config.windowHeight = 240;
    EXPECT_FALSE(overlay.PreCreate(config));
    int after = 0;
    windows = SDL_GetWindows(&after);
    SDL_free(windows);
    EXPECT_EQ(after, before);
}

TEST_F(ScrollCaptureGpuOutputTest, CocoaBackingLayerHonorsNativeOpacity) {
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    ASSERT_NE(native, nil);
    // 覆盖 SDL 的 updateLayer 分支，并确保普通不透明窗口仍保留黑色兜底。
    native.contentView.wantsLayer = YES;
    native.opaque = YES;
    [native.contentView updateLayer];
    ASSERT_NE(native.contentView.layer.backgroundColor, nullptr);
    EXPECT_EQ(CGColorGetAlpha(native.contentView.layer.backgroundColor), 1.0);
    ConfigureWindowTransparent(window);
    [native.contentView updateLayer];
    ASSERT_NE(native.contentView.layer.backgroundColor, nullptr);
    EXPECT_EQ(CGColorGetAlpha(native.contentView.layer.backgroundColor), 0.0);
}

namespace shotlet {
class ScreenOverlayLifecycleTest : public ::ScrollCaptureGpuOutputTest {
protected:
    void SetUp() override {
        ScrollCaptureGpuOutputTest::SetUp();
        if (!initialized) return;
        application = std::make_unique<Application>(true);
        application->m_gpuDevice = device;
    }
    void TearDown() override {
        // GPU 归基础夹具所有，Application 不能重复销毁。
        if (application) application->m_gpuDevice = nullptr;
        application.reset();
        ImGui::SetCurrentContext(context);
        ScrollCaptureGpuOutputTest::TearDown();
    }
    std::unique_ptr<Application> application;
};

TEST_F(ScreenOverlayLifecycleTest, BackingRemainsTransparentAcrossSwapchainReleaseAndReuse) {
    constexpr int widthLogical = 320;
    constexpr int heightLogical = 240;
    constexpr int rgbaChannels = 4;
    constexpr int reuseCount = 3;
    ScreenOverlay overlay;
    ScreenOverlay::Config config;
    config.windowWidth = widthLogical;
    config.windowHeight = heightLogical;
    ASSERT_TRUE(overlay.PreCreate(config));
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(overlay.GetSDLWindow()), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    ASSERT_NE(native, nil);
    for (int iteration = 0; iteration < reuseCount; ++iteration) {
        SCOPED_TRACE(iteration);
        auto pixels = std::make_unique<unsigned char[]>(widthLogical * heightLogical * rgbaChannels);
        std::memset(pixels.get(), 255, widthLogical * heightLogical * rgbaChannels);
        ASSERT_TRUE(overlay.SetCaptureData(std::move(pixels), widthLogical, heightLogical,
                                          widthLogical * rgbaChannels));
        EXPECT_FALSE(native.opaque);
        // 退出释放 Metal 子视图后，剩余的内容视图也必须保持透明。
        // 显式触发 SDL 的真实重绘入口，检查其是否忽略了原生透明设置。
        NSBitmapImageRep* backing = [native.contentView bitmapImageRepForCachingDisplayInRect:native.contentView.bounds];
        ASSERT_NE(backing, nil);
        [native.contentView cacheDisplayInRect:native.contentView.bounds toBitmapImageRep:backing];
        NSColor* center = [[backing colorAtX:widthLogical / 2 y:heightLogical / 2]
            colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
        EXPECT_EQ(center.alphaComponent, 0.0) << "SDL backing RGB=" << center.redComponent << ","
            << center.greenComponent << "," << center.blueComponent;
        for (NSView* child in native.contentView.subviews) {
            if ([child.layer isKindOfClass:CAMetalLayer.class]) EXPECT_FALSE(child.layer.opaque);
        }
        overlay.ShowOverlay();
        EXPECT_TRUE(native.visible);
        EXPECT_EQ(native.alphaValue, 1.0);
        EXPECT_FALSE(native.ignoresMouseEvents);
        overlay.Hide();
        EXPECT_FALSE(native.visible);
        EXPECT_EQ(native.alphaValue, 0.0);
        EXPECT_TRUE(native.ignoresMouseEvents);
        overlay.ResetForReuse();
        [native.contentView cacheDisplayInRect:native.contentView.bounds toBitmapImageRep:backing];
        center = [[backing colorAtX:widthLogical / 2 y:heightLogical / 2]
            colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
        EXPECT_EQ(center.alphaComponent, 0.0) << "swapchain 释放后的 SDL 背景必须透明";
        for (NSView* child in native.contentView.subviews)
            EXPECT_FALSE([child.layer isKindOfClass:CAMetalLayer.class]);
    }
}
}

TEST_F(ScrollCaptureGpuOutputTest, CanvasExportKeepsMaximumHeightAtFittedDisplaySize) {
    constexpr int width = 40, height = 16384;
    auto pixels = std::make_unique<uint8_t[]>(size_t(width) * height * 4);
    std::memset(pixels.get(), 255, size_t(width) * height * 4);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width = width; info.height = height;
    info.layer_count_or_depth = 1; info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    ASSERT_NE(texture, nullptr) << SDL_GetError();
    SDL_GPUTransferBufferCreateInfo transfer = {};
    transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transfer.size = width * height * 4;
    auto buffer = SDL_CreateGPUTransferBuffer(device, &transfer);
    ASSERT_NE(buffer, nullptr);
    void* mapped = SDL_MapGPUTransferBuffer(device, buffer, false);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(mapped, pixels.get(), transfer.size); SDL_UnmapGPUTransferBuffer(device, buffer);
    auto command = SDL_AcquireGPUCommandBuffer(device);
    auto copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo upload = {}; upload.transfer_buffer = buffer;
    SDL_GPUTextureRegion region = {}; region.texture = texture; region.w = width; region.h = height; region.d = 1;
    SDL_UploadToGPUTexture(copy, &upload, &region, false);
    SDL_EndGPUCopyPass(copy); SDL_SubmitGPUCommandBuffer(command);
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll((ImTextureID)(intptr_t)texture, ImVec2(width, height),
        ImVec2(1.25f, 512), std::move(pixels), width * 4);
    auto result = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), 2);
    EXPECT_EQ(result.width, width); EXPECT_EQ(result.height, height);
    if (result.data) {
        EXPECT_EQ(result.data[3], 255);
        EXPECT_EQ(result.data[(size_t(height) * width - 1) * 4 + 3], 255);
    } else ADD_FAILURE() << "长图输出为空";
    SDL_ReleaseGPUTransferBuffer(device, buffer); SDL_ReleaseGPUTexture(device, texture);
}

TEST_F(ScrollCaptureGpuOutputTest, CpuCropWithPaddedRowsMatchesGpuOutputAtRetinaScale) {
    constexpr int width = 128, height = 96, pitch = width * 4 + 16;
    auto pixels = std::make_unique<unsigned char[]>(pitch * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = pixels.get() + y * pitch + x * 4;
            pixel[0] = x; pixel[1] = y; pixel[2] = 17; pixel[3] = 255;
        }
    }
    auto* texture = CreateScreenshotTexture(device, width, height, pixels.get(), pitch);
    ASSERT_NE(texture, nullptr);
    Canvas cpuCanvas, gpuCanvas;
    cpuCanvas.SetScreenshotOwned((ImTextureID)(intptr_t)texture, ImVec2(width, height),
        ImVec2(width / 2, height / 2), std::move(pixels), pitch);
    // 没有 CPU 像素的同一选区必须走 GPU 导出，校验裁剪、通道和方向一致。
    gpuCanvas.SetScreenshot((ImTextureID)(intptr_t)texture, ImVec2(width, height),
        ImVec2(width / 2, height / 2));
    for (Canvas* canvas : {&cpuCanvas, &gpuCanvas}) {
        canvas->OnMouseDown(ImVec2(10, 10));
        canvas->OnMouseMove(ImVec2(50, 40));
        canvas->OnMouseUp(ImVec2(50, 40));
    }
    auto cpu = FBORenderer::RenderToOffscreen(device, window, context, &cpuCanvas, cpuCanvas.GetSelection(), 2);
    auto gpu = FBORenderer::RenderToOffscreen(device, window, context, &gpuCanvas, gpuCanvas.GetSelection(), 2);
    ASSERT_NE(cpu.data, nullptr);
    ASSERT_NE(gpu.data, nullptr);
    EXPECT_EQ(cpu.width, 80); EXPECT_EQ(cpu.height, 60);
    ASSERT_EQ(cpu.width, gpu.width); ASSERT_EQ(cpu.height, gpu.height);
    EXPECT_EQ(std::memcmp(cpu.data.get(), gpu.data.get(), size_t(cpu.width) * cpu.height * 4), 0);
    for (int y = 0; y < cpu.height; ++y) {
        for (int x = 0; x < cpu.width; ++x) {
            const auto* pixel = cpu.data.get() + (y * cpu.width + x) * 4;
            EXPECT_EQ(pixel[0], x + 20); EXPECT_EQ(pixel[1], y + 20);
            EXPECT_EQ(pixel[2], 17); EXPECT_EQ(pixel[3], 255);
        }
    }
    SDL_ReleaseGPUTexture(device, texture);
}

TEST(ScrollCaptureOutputTest, PinWindowFitsScreenAndPreservesFullImage) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if (NSScreen.screens.count == 0) GTEST_SKIP() << "当前会话没有显示器";
        constexpr int width = 800, height = 16384;
        std::vector<uint8_t> pixels(size_t(width) * height * 4, 255);
        ASSERT_TRUE(PinWindow::Create(pixels.data(), width, height, 100, 100, 2));
        NSWindow* pin = nil;
        for (NSWindow* window in NSApp.windows) {
            if ([window.contentView isKindOfClass:NSImageView.class] && window.visible) { pin = window; break; }
        }
        EXPECT_NE(pin, nil);
        if (pin) {
            EXPECT_LE(pin.contentView.bounds.size.height, pin.screen.visibleFrame.size.height);
            NSImage* image = ((NSImageView*)pin.contentView).image;
            ASSERT_GT(image.representations.count, 0u);
            EXPECT_EQ(image.representations.firstObject.pixelsHigh, height);
        }
        PinWindow::Destroy();
    }
}

class PinWindowTest : public testing::Test {
protected:
    void SetUp() override {
        [NSApplication sharedApplication];
        if (NSScreen.screens.count == 0)
            GTEST_SKIP() << "当前会话没有显示器";
        PinWindow::Destroy();
    }

    void TearDown() override { PinWindow::Destroy(); }

    NSWindow *CreateWindow(int width, int height, unsigned char red = 255) {
        constexpr int rgbaBytesPerPixel = 4;
        constexpr float screenOrigin = 100;
        std::vector<unsigned char> pixels(size_t(width) * height * rgbaBytesPerPixel, 255);
        for (size_t offset = 0; offset < pixels.size(); offset += rgbaBytesPerPixel)
            pixels[offset] = red;
        if (!PinWindow::Create(pixels.data(), width, height, screenOrigin, screenOrigin))
            return nil;
        for (NSWindow *window in NSApp.windows) {
            if (![window.contentView isKindOfClass:NSImageView.class] || !window.visible)
                continue;
            NSImage *image = ((NSImageView *) window.contentView).image;
            if (image.size.width == width && image.size.height == height)
                return window;
        }
        return nil;
    }

    NSButton *PinButton(NSWindow *window) {
        NSView *container = window.titlebarAccessoryViewControllers.firstObject.view;
        for (NSView *view in container.subviews) {
            if ([view isKindOfClass:NSButton.class])
                return (NSButton *) view;
        }
        return nil;
    }

    NSMenu *ContextMenu(NSWindow *window) {
        NSEvent *event = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:window.windowNumber
                                             context:nil
                                         eventNumber:0
                                          clickCount:1
                                            pressure:1];
        return [window.contentView menuForEvent:event];
    }
};

namespace {
    NSPasteboard *PinTestPasteboard(id, SEL) { return [NSPasteboard pasteboardWithName:@"org.shotlet.pin-window-tests"]; }
} // namespace

TEST_F(PinWindowTest, CopyUsesOwningWindowsPixelsAfterAnotherWindowCloses) {
    @autoreleasepool {
        // 使用独立剪贴板验证真实 PNG 输出，不覆盖用户的系统剪贴板。
        Method method = class_getClassMethod(NSPasteboard.class, @selector(generalPasteboard));
        IMP original = method_setImplementation(method, reinterpret_cast<IMP>(PinTestPasteboard));
        @try {
            if ([NSPasteboard generalPasteboard].changeCount < 0)
                GTEST_SKIP() << "当前会话无法连接测试剪贴板";
            constexpr unsigned char firstRed = 37, secondRed = 119;
            constexpr int firstWidth = 240, firstHeight = 160;
            constexpr int secondWidth = 320, secondHeight = 200;
            NSWindow *first = CreateWindow(firstWidth, firstHeight, firstRed);
            NSWindow *second = CreateWindow(secondWidth, secondHeight, secondRed);
            ASSERT_NE(first, nil);
            ASSERT_NE(second, nil);
            auto expectCopy = [&](NSWindow *window, int width, int height, unsigned char red) {
                NSMenuItem *copyItem = ContextMenu(window).itemArray.firstObject;
                ASSERT_TRUE([NSApp sendAction:copyItem.action to:copyItem.target from:copyItem]);
                NSData *png = [[NSPasteboard generalPasteboard] dataForType:NSPasteboardTypePNG];
                ASSERT_NE(png, nil);
                NSBitmapImageRep *image = [NSBitmapImageRep imageRepWithData:png];
                ASSERT_NE(image, nil);
                EXPECT_EQ(image.pixelsWide, width);
                EXPECT_EQ(image.pixelsHigh, height);
                constexpr int rgbaChannels = 4;
                NSUInteger pixel[rgbaChannels]{};
                [image getPixel:pixel atX:0 y:0];
                EXPECT_EQ(pixel[0], red);
            };
            expectCopy(first, firstWidth, firstHeight, firstRed);
            expectCopy(second, secondWidth, secondHeight, secondRed);
            [second close];
            EXPECT_TRUE(first.visible);
            EXPECT_TRUE(PinWindow::IsVisible());
            expectCopy(first, firstWidth, firstHeight, firstRed);
        } @finally {
            [[NSPasteboard generalPasteboard] releaseGlobally];
            method_setImplementation(method, original);
        }
    }
}

TEST_F(PinWindowTest, MultipleWindowsKeepImagesAndCloseIndependently) {
    @autoreleasepool {
        constexpr int firstWidth = 240, firstHeight = 160;
        constexpr int secondWidth = 320, secondHeight = 200;
        NSWindow *first = CreateWindow(firstWidth, firstHeight);
        ASSERT_NE(first, nil);
        NSWindow *second = CreateWindow(secondWidth, secondHeight);
        ASSERT_NE(second, nil);
        EXPECT_NE(first, second);
        EXPECT_TRUE(first.visible);
        EXPECT_TRUE(second.visible);
        EXPECT_TRUE(PinWindow::IsVisible());
        NSImage *firstImage = ((NSImageView *) first.contentView).image;
        EXPECT_EQ(firstImage.representations.firstObject.pixelsWide, firstWidth);
        EXPECT_EQ(firstImage.representations.firstObject.pixelsHigh, firstHeight);

        [first close];
        PinWindow::ProcessEvents();
        EXPECT_FALSE(first.visible);
        EXPECT_TRUE(second.visible);
        EXPECT_TRUE(PinWindow::IsVisible());
        NSImage *secondImage = ((NSImageView *) second.contentView).image;
        EXPECT_EQ(secondImage.representations.firstObject.pixelsWide, secondWidth);
        EXPECT_EQ(secondImage.representations.firstObject.pixelsHigh, secondHeight);

        [second close];
        PinWindow::ProcessEvents();
        EXPECT_FALSE(PinWindow::IsVisible());
    }
}

TEST_F(PinWindowTest, PinToggleAndContextMenusBelongToTheirWindow) {
    @autoreleasepool {
        NSWindow *first = CreateWindow(240, 160);
        NSWindow *second = CreateWindow(320, 200);
        ASSERT_NE(first, nil);
        ASSERT_NE(second, nil);
        NSButton *button = PinButton(first);
        ASSERT_NE(button, nil);
        [button performClick:nil];
        EXPECT_EQ(first.level, NSNormalWindowLevel);
        EXPECT_EQ(second.level, NSStatusWindowLevel);

        // 第二个窗口处于前台时，旧窗口菜单仍明确指向自己的图片视图。
        NSMenu *firstMenu = ContextMenu(first);
        NSMenu *secondMenu = ContextMenu(second);
        ASSERT_EQ(firstMenu.numberOfItems, 2);
        ASSERT_EQ(secondMenu.numberOfItems, 2);
        for (NSMenuItem *item in firstMenu.itemArray)
            EXPECT_EQ(item.target, first.contentView);
        for (NSMenuItem *item in secondMenu.itemArray)
            EXPECT_EQ(item.target, second.contentView);
    }
}

TEST_F(PinWindowTest, FailedCreationPreservesWindowsAndDestroyClosesAll) {
    @autoreleasepool {
        NSWindow *first = CreateWindow(240, 160);
        NSWindow *second = CreateWindow(320, 200);
        ASSERT_NE(first, nil);
        ASSERT_NE(second, nil);
        EXPECT_FALSE(PinWindow::Create(nullptr, 240, 160, 100, 100));
        EXPECT_TRUE(first.visible);
        EXPECT_TRUE(second.visible);

        PinWindow::SetTheme(Theme::Light);
        EXPECT_TRUE([first.appearance.name isEqualToString:NSAppearanceNameAqua]);
        EXPECT_TRUE([second.appearance.name isEqualToString:NSAppearanceNameAqua]);
        PinWindow::SetTheme(Theme::Dark);
        EXPECT_TRUE([first.appearance.name isEqualToString:NSAppearanceNameDarkAqua]);
        EXPECT_TRUE([second.appearance.name isEqualToString:NSAppearanceNameDarkAqua]);

        PinWindow::Destroy();
        EXPECT_FALSE(first.visible);
        EXPECT_FALSE(second.visible);
        EXPECT_FALSE(PinWindow::IsVisible());
    }
}

TEST_F(PinWindowTest, MinimizedWindowStaysAliveUntilClosed) {
    @autoreleasepool {
        NSWindow *window = CreateWindow(240, 160);
        ASSERT_NE(window, nil);
        window.animationBehavior = NSWindowAnimationBehaviorNone;
        [window miniaturize:nil];
        // Dock 完成最小化后才更新窗口状态，等待原生事件，避免断言动画中间态。
        constexpr NSTimeInterval minimizeTimeoutSeconds = 1;
        constexpr NSTimeInterval eventIntervalSeconds = 0.01;
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:minimizeTimeoutSeconds];
        while (!window.miniaturized && deadline.timeIntervalSinceNow > 0) {
            PinWindow::ProcessEvents();
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:eventIntervalSeconds]];
        }
        EXPECT_TRUE(window.miniaturized);
        EXPECT_TRUE(PinWindow::IsVisible());
        [window close];
        EXPECT_FALSE(PinWindow::IsVisible());
    }
}

TEST_F(ScrollCaptureGpuOutputTest, ApplyEditorGpuBufferCountAcceptsCallerProvidedValue) {
    @autoreleasepool {
        // 依赖方向修复后的签名验证：count 由 ui 层调用方传入，
        // 合法值（2/3）不崩溃，非法值被忽略（无 CAMetalLayer 时仅告警日志）。
        ApplyEditorGpuBufferCount(nullptr, 2);
        ApplyEditorGpuBufferCount(window, 2);
        ApplyEditorGpuBufferCount(window, 4);  // 非法值：函数应直接返回
    }
}

TEST_F(ScrollCaptureGpuOutputTest, EditorWindowIsNormalBorderedCenteredAndBounded) {
    @autoreleasepool {
        // 模拟真实创建路径：ImageEditor::CreateSDLWindow 以 RESIZABLE 创建窗口
        SDL_SetWindowResizable(window, true);
        ASSERT_TRUE(SetupImageEditorWindow(window, 1600, 12000));
        auto flags = SDL_GetWindowFlags(window);
        EXPECT_FALSE(flags & SDL_WINDOW_BORDERLESS);
        EXPECT_FALSE(flags & SDL_WINDOW_ALWAYS_ON_TOP);
        EXPECT_TRUE(flags & SDL_WINDOW_RESIZABLE);
        NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        ASSERT_NE(native, nil);
        EXPECT_EQ(native.level, NSNormalWindowLevel);
        EXPECT_TRUE(native.opaque);
        EXPECT_TRUE(native.hasShadow);
        EXPECT_TRUE(native.styleMask & NSWindowStyleMaskClosable);
        EXPECT_LE(native.frame.size.height, native.screen.visibleFrame.size.height);
        EXPECT_LE(native.frame.size.width, native.screen.visibleFrame.size.width);
        EXPECT_LE(native.frame.size.width * native.screen.backingScaleFactor, 1560 + 1);
        EXPECT_NEAR(native.frame.size.height, std::floor(native.screen.visibleFrame.size.height - 96), 1);
        EXPECT_NEAR(NSMidX(native.frame), NSMidX(native.screen.visibleFrame), 1);
        EXPECT_NEAR(NSMidY(native.frame), NSMidY(native.screen.visibleFrame), 1);
        EXPECT_GE(native.screen.visibleFrame.size.width - native.frame.size.width, 94);
        EXPECT_GE(native.screen.visibleFrame.size.height - native.frame.size.height, 94);
        const double longHeight = native.frame.size.height;
        // 小图也使用有界窗口，居中且不切换到全屏。
        ASSERT_TRUE(SetupImageEditorWindow(window, 640, 480));
        EXPECT_LE(native.frame.size.width * native.screen.backingScaleFactor, 1560 + 1);
        EXPECT_LE(native.frame.size.height * native.screen.backingScaleFactor, 1000 + 1);
        EXPECT_NEAR(NSMidX(native.frame), NSMidX(native.screen.visibleFrame), 1);
        EXPECT_NEAR(NSMidY(native.frame), NSMidY(native.screen.visibleFrame), 1);
        EXPECT_LT(native.frame.size.height, longHeight);
        EXPECT_FALSE(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    }
}

TEST_F(ScrollCaptureGpuOutputTest, EditorZoomAndScrollingDoNotChangeAnnotationExport) {
    constexpr int width = 200, height = 3000;
    auto pixels = std::make_unique<uint8_t[]>(size_t(width) * height * 4);
    std::memset(pixels.get(), 255, size_t(width) * height * 4);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width = width; info.height = height;
    info.layer_count_or_depth = 1; info.num_levels = 1;
    auto texture = SDL_CreateGPUTexture(device, &info);
    ASSERT_NE(texture, nullptr);
    SDL_GPUTransferBufferCreateInfo bufferInfo = {};
    bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bufferInfo.size = width * height * 4;
    auto upload = SDL_CreateGPUTransferBuffer(device, &bufferInfo);
    ASSERT_NE(upload, nullptr);
    auto mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(mapped, pixels.get(), bufferInfo.size);
    SDL_UnmapGPUTransferBuffer(device, upload);
    auto command = SDL_AcquireGPUCommandBuffer(device);
    auto copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source = {}; source.transfer_buffer = upload;
    SDL_GPUTextureRegion target = {}; target.texture = texture; target.w = width; target.h = height; target.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &target, false);
    SDL_EndGPUCopyPass(copy); SDL_SubmitGPUCommandBuffer(command);
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll((ImTextureID)(intptr_t)texture, ImVec2(width, height),
        ImVec2(100, 1500), std::move(pixels), width * 4);
    canvas.SetImageEditing(true);
    const ImageEditorTransform transform{ImVec2(60, -700), 0.75f};
    canvas.SetTool(ToolType::Rectangle);
    canvas.SetToolColor(IM_COL32(255, 0, 0, 255));
    canvas.SetToolFill(true);
    canvas.OnMouseDown(transform.ToImage(transform.ToScreen(ImVec2(20, 1100))));
    canvas.OnMouseMove(transform.ToImage(transform.ToScreen(ImVec2(80, 1200))));
    canvas.OnMouseUp(transform.ToImage(transform.ToScreen(ImVec2(80, 1200))));
    // 用实际编辑渲染走一遍变换，再导出；导出不能继承视口的缩放和裁剪。
    ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(800, 600));
    ImGui::Begin("editor-annotation", nullptr, ImGuiWindowFlags_NoDecoration);
    canvas.RenderInEditor(ImGui::GetWindowDrawList(), transform, ImRect(0, 54, 800, 566));
    ImGui::End(); ImGui::Render();
    auto result = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), 2);
    ASSERT_NE(result.data, nullptr);
    EXPECT_EQ(result.width, width); EXPECT_EQ(result.height, height);
    auto redPixel = result.data.get() + (size_t(2300) * width + 100) * 4;
    EXPECT_EQ(redPixel[0], 255); EXPECT_EQ(redPixel[1], 0); EXPECT_EQ(redPixel[2], 0);
    EXPECT_EQ(result.data[0], 255); EXPECT_EQ(result.data[1], 255);
    EXPECT_EQ(result.data[(size_t(width) * height - 1) * 4 + 3], 255);
    SDL_ReleaseGPUTransferBuffer(device, upload); SDL_ReleaseGPUTexture(device, texture);
}

TEST_F(ScrollCaptureGpuOutputTest, Small952By500ImageOpensWithinPhysicalPixelLimit) {
    @autoreleasepool {
        ASSERT_TRUE(SetupImageEditorWindow(window, 952, 500));
        NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        ASSERT_NE(native, nil);
        const double backingScale = native.screen.backingScaleFactor;
        EXPECT_LE(native.frame.size.width * backingScale, 1560 + 1);
        EXPECT_LE(native.frame.size.height * backingScale, 1000 + 1);
        EXPECT_NEAR(NSMidX(native.frame), NSMidX(native.screen.visibleFrame), 1);
        EXPECT_NEAR(NSMidY(native.frame), NSMidY(native.screen.visibleFrame), 1);
        int pixelWidth = 0, pixelHeight = 0;
        ASSERT_TRUE(SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight));
        EXPECT_LE(pixelWidth, 1560);
        EXPECT_LE(pixelHeight, 1000);
    }
}

TEST_F(ScrollCaptureGpuOutputTest, RotatedExportPreservesPixelsAnnotationsAndFullEdges) {
    constexpr int width = 80, height = 120;
    auto pixels = std::make_unique<uint8_t[]>(width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = pixels.get() + (y * width + x) * 4;
            pixel[0] = x < width / 2 ? 40 : 200;
            pixel[1] = y < height / 2 ? 60 : 220;
            pixel[2] = 30;
            pixel[3] = 255;
        }
    }
    auto* texture = CreateScreenshotTexture(device, width, height, pixels.get(), width * 4);
    ASSERT_NE(texture, nullptr);
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll((ImTextureID)(intptr_t)texture, ImVec2(width, height),
        ImVec2(width / 2, height / 2), std::move(pixels), width * 4);
    canvas.SetImageEditing(true);
    canvas.SetTool(ToolType::Rectangle);
    canvas.SetToolColor(IM_COL32(0, 0, 255, 255));
    canvas.SetToolFill(true);
    canvas.OnMouseDown(ImVec2(5, 5));
    canvas.OnMouseMove(ImVec2(15, 15));
    canvas.OnMouseUp(ImVec2(15, 15));
    for (int turns = 0; turns < 4; ++turns) {
        auto result = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), 2);
        ASSERT_NE(result.data, nullptr);
        EXPECT_EQ(result.width, turns % 2 ? height : width);
        EXPECT_EQ(result.height, turns % 2 ? width : height);
        const ImageEditorTransform output{ImVec2(0, 0), 1, turns, ImVec2(width, height)};
        const auto pixelAtOriginal = [&](int x, int y) {
            const ImVec2 p = output.ToScreen(ImVec2(x + 0.5f, y + 0.5f));
            return result.data.get() + (size_t(int(p.y)) * result.width + int(p.x)) * 4;
        };
        for (int y : {0, height - 1}) {
            for (int x : {0, width - 1}) {
                const auto* pixel = pixelAtOriginal(x, y);
                EXPECT_EQ(pixel[0], x < width / 2 ? 40 : 200);
                EXPECT_EQ(pixel[1], y < height / 2 ? 60 : 220);
                EXPECT_EQ(pixel[2], 30);
                EXPECT_EQ(pixel[3], 255);
            }
        }
        const auto* annotation = pixelAtOriginal(20, 20);
        EXPECT_EQ(annotation[0], 0);
        EXPECT_EQ(annotation[1], 0);
        EXPECT_EQ(annotation[2], 255);
        canvas.RotateImageCounterClockwise();
    }
    SDL_ReleaseGPUTexture(device, texture);
}

TEST_F(ScrollCaptureGpuOutputTest, LongImageEditorScrollReachesBottomWithoutHorizontalCropping) {
    auto image = std::make_shared<ScrollImage>();
    image->width = 1600;
    image->height = 6000;
    image->rgba.assign(size_t(image->width) * image->height * 4, 255);
    ScreenOverlay::Config config;
    config.scaleFactor = 2;
    config.language = Language::English;
    struct RestoreContext {
        ImGuiContext* previous;
        ~RestoreContext() { ImGui::SetCurrentContext(previous); }
    } restore{context};
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    EXPECT_STREQ(SDL_GetWindowTitle(editor.GetSDLWindow()), "Shotlet · Image editor");
    for (int i = 0; i < 4; ++i) editor.RenderFrame();
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    const auto* toolbarWindow = ImGui::FindWindowByName("Toolbar");
    const auto* viewportWindow = ImGui::FindWindowByName("image_editor_viewport");
    ASSERT_NE(toolbarWindow, nullptr);
    ASSERT_NE(viewportWindow, nullptr);
    EXPECT_FLOAT_EQ(toolbarWindow->Size.y, Toolbar::EditorHeight);
    EXPECT_FLOAT_EQ(viewportWindow->Pos.y, toolbarWindow->Pos.y + toolbarWindow->Size.y);
    ImGuiWindow* child = nullptr;
    for (auto* candidate : ImGui::GetCurrentContext()->Windows) {
        if (std::strstr(candidate->Name, "image_editor_scroll")) { child = candidate; break; }
    }
    ASSERT_NE(child, nullptr);
    EXPECT_GT(child->ScrollMax.y, 0);
    EXPECT_LE(child->ScrollMax.x, 1);
    ImGui::SetScrollY(child, child->ScrollMax.y);
    for (int i = 0; i < 3; ++i) editor.RenderFrame();
    EXPECT_NEAR(child->Scroll.y, child->ScrollMax.y, 1);
    // 图片实际绘制四边形的底边应进入视口，证明不是只有滚动条数值变化。
    bool imageVisible = false;
    for (const auto& command : child->DrawList->CmdBuffer) {
        if (command.ElemCount != 6) continue;
        float bottom = -1e9f;
        bool imageUv = false;
        for (unsigned int i = 0; i < command.ElemCount; ++i) {
            const auto index = child->DrawList->IdxBuffer[command.IdxOffset + i] + command.VtxOffset;
            const auto& vertex = child->DrawList->VtxBuffer[index];
            bottom = std::max(bottom, vertex.pos.y);
            imageUv = imageUv || (vertex.uv.x == 1 && vertex.uv.y == 1);
        }
        if (imageUv && bottom >= child->InnerClipRect.Min.y && bottom <= child->InnerClipRect.Max.y) imageVisible = true;
    }
    EXPECT_TRUE(imageVisible);
    EXPECT_EQ(editor.GetCanvas()->GetScreenshotSize().y, 6000);
}


TEST_F(ScrollCaptureGpuOutputTest, ArrowHeadKeepsSweptWingsInHighDpiGpuOutput) {
    constexpr int width = 420, height = 707, channels = 4;
    constexpr float scale = 2;
    constexpr uint8_t background = 30;
    const ImU32 arrowColor = IM_COL32(150, 220, 0, 255);
    auto pixels = std::make_unique<uint8_t[]>(size_t(width) * height * channels);
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        auto* pixel = pixels.get() + i * channels;
        pixel[0] = pixel[1] = pixel[2] = background;
        pixel[3] = 255;
    }
    const auto releaseTexture = [this](SDL_GPUTexture* texture) { SDL_ReleaseGPUTexture(device, texture); };
    const std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> texture(
        CreateScreenshotTexture(device, width, height, pixels.get(), width * channels), releaseTexture);
    ASSERT_NE(texture, nullptr);
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll((ImTextureID)(intptr_t)texture.get(), ImVec2(width, height),
        ImVec2(width / scale, height / scale), std::move(pixels), width * channels);
    // 三档线宽走真实画布坐标与 Retina 输出，生成可检查的实际像素预览。
    constexpr float arrowSpacing = 65, firstArrowX = 40, tailY = 332, tipY = 30;
    int index = 0;
    for (const float thickness : Canvas::THICKNESS_OPTIONS) {
        const float x = firstArrowX + index++ * arrowSpacing;
        auto arrow = std::make_unique<ArrowLayer>(ImVec2(x, tailY), ImVec2(x, tipY), arrowColor, thickness);
        arrow->SetShowHandles(false);
        canvas.GetLayerManager().AddLayer(std::move(arrow));
    }
    auto output = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), scale);
    ASSERT_NE(output.data, nullptr);
    ASSERT_EQ(output.width, width);
    ASSERT_EQ(output.height, height);
    const auto pixelAt = [&](int x, int y) { return output.data.get() + (size_t(y) * width + x) * channels; };
    for (const int x : {306, 374}) {
        EXPECT_EQ(pixelAt(x, 156)[0], 150);
        EXPECT_EQ(pixelAt(x, 156)[1], 220);
    }
    for (const int x : {316, 364}) EXPECT_EQ(pixelAt(x, 164)[1], background);
    EXPECT_EQ(pixelAt(340, 200)[1], 220);
    ASSERT_TRUE(WritePngToFile(output.data.get(), width, height,
        std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/arrow-head-preview.png"));
}

TEST_F(ScrollCaptureGpuOutputTest, NumberMarkerExportsDigitsAtHighDpiAndAfterMoving) {
    constexpr int width = 400, height = 400;
    auto pixels = std::make_unique<uint8_t[]>(size_t(width) * height * 4);
    std::memset(pixels.get(), 255, size_t(width) * height * 4);
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; info.width = width; info.height = height;
    info.layer_count_or_depth = 1; info.num_levels = 1;
    auto texture = SDL_CreateGPUTexture(device, &info);
    ASSERT_NE(texture, nullptr);
    SDL_GPUTransferBufferCreateInfo transfer = {};
    transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transfer.size = width * height * 4;
    auto buffer = SDL_CreateGPUTransferBuffer(device, &transfer);
    ASSERT_NE(buffer, nullptr);
    auto mapped = SDL_MapGPUTransferBuffer(device, buffer, false);
    ASSERT_NE(mapped, nullptr);
    std::memcpy(mapped, pixels.get(), transfer.size); SDL_UnmapGPUTransferBuffer(device, buffer);
    auto command = SDL_AcquireGPUCommandBuffer(device);
    auto copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo upload = {}; upload.transfer_buffer = buffer;
    SDL_GPUTextureRegion region = {}; region.texture = texture; region.w = width; region.h = height; region.d = 1;
    SDL_UploadToGPUTexture(copy, &upload, &region, false);
    SDL_EndGPUCopyPass(copy); SDL_SubmitGPUCommandBuffer(command);
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll((ImTextureID)(intptr_t)texture, ImVec2(width, height),
        ImVec2(200, 200), std::move(pixels), width * 4);
    canvas.SetImageEditing(true);
    canvas.SetTool(ToolType::Mosaic);
    canvas.OnMouseDown(ImVec2(40, 40)); canvas.OnMouseMove(ImVec2(60, 80)); canvas.OnMouseUp(ImVec2(60, 80));
    canvas.SetTool(ToolType::Number);
    canvas.SetToolColor(IM_COL32(0, 0, 255, 255));
    canvas.OnMouseDown(ImVec2(60, 60)); canvas.OnMouseUp(ImVec2(60, 60));
    canvas.CommitTextInput();
    canvas.OnMouseDown(ImVec2(60, 60)); canvas.OnMouseMove(ImVec2(130, 120)); canvas.OnMouseUp(ImVec2(130, 120));
    ASSERT_TRUE(canvas.HasSelectedNumberLayer());
    // 实际编辑预览变换不得进入随后导出的图像坐标。
    ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
    ImGui::Begin("number-preview", nullptr, ImGuiWindowFlags_NoDecoration);
    canvas.RenderInEditor(ImGui::GetWindowDrawList(), {ImVec2(30, 40), 0.75f}, ImRect(0, 0, 800, 600));
    ImGui::End(); ImGui::Render();
    auto result = FBORenderer::RenderToOffscreen(device, window, context, &canvas, canvas.GetSelection(), 2);
    ASSERT_NE(result.data, nullptr);
    EXPECT_EQ(result.width, width); EXPECT_EQ(result.height, height);
    auto pixel = [&](int x, int y) { return result.data.get() + (size_t(y) * width + x) * 4; };
    EXPECT_EQ(pixel(120, 120)[0], 255); EXPECT_EQ(pixel(120, 120)[1], 255); // 旧位置无残影
    EXPECT_EQ(pixel(238, 240)[0], 0); EXPECT_EQ(pixel(238, 240)[2], 255); // 新位置有蓝色圆
    EXPECT_EQ(pixel(228, 208)[0], 255); EXPECT_EQ(pixel(228, 208)[1], 255); // 方框角无选择边框
    int whiteInk = 0;
    for (int y = 218; y < 262; ++y) {
        for (int x = 245; x < 275; ++x) {
            if (pixel(x, y)[0] > 100 && pixel(x, y)[1] > 100) ++whiteInk;
        }
    }
    EXPECT_GT(whiteInk, 15); // 圆内部必须有数字，不能只输出背景
    // 实际保存为 PNG 并回读，验证最终文件内容。
    NSString* path = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-number-%@.png", NSUUID.UUID.UUIDString];
    ASSERT_TRUE(WritePngToFile(result.data.get(), width, height, path.UTF8String));
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr);
    ASSERT_NE(source, nullptr);
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    ASSERT_NE(image, nullptr);
    std::vector<uint8_t> decoded(size_t(width) * height * 4);
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGContextRef bitmap = CGBitmapContextCreate(decoded.data(), width, height, 8, width * 4, color,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    ASSERT_NE(bitmap, nullptr);
    CGContextDrawImage(bitmap, CGRectMake(0, 0, width, height), image);
    EXPECT_EQ(std::memcmp(decoded.data(), result.data.get(), decoded.size()), 0);
    CGContextRelease(bitmap); CGColorSpaceRelease(color); CGImageRelease(image); CFRelease(source);
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    SDL_ReleaseGPUTransferBuffer(device, buffer); SDL_ReleaseGPUTexture(device, texture);
}

TEST_F(ScrollCaptureGpuOutputTest, NumberToolbarFitsNarrowAndHighDpiLayouts) {
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(320, 240), ImVec2(320, 240), nullptr, 1280);
    canvas.SetTool(ToolType::Number);
    Toolbar toolbar;
    toolbar.SetScrollCallback([] {});
    toolbar.SetLanguage(Language::English);
    toolbar.SetCanvas(&canvas);
    SubToolbar sub;
    sub.SetLanguage(Language::English);
    sub.SetCanvas(&canvas); sub.SetToolbar(&toolbar);
    for (bool editor : {false, true}) {
        toolbar.SetRecordCallback(editor ? std::function<void()>() : [] {});
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            for (int width : {380, 640, 1280}) {
                toolbar.SetEditorMode(editor); toolbar.SetUIScale(scale); sub.SetUIScale(scale);
                toolbar.SetScrollCallback(editor ? std::function<void()>{} : [] {});
                for (int frame = 0; frame < 2; ++frame) {
                    SCOPED_TRACE(testing::Message() << "editor=" << editor << ", scale=" << scale
                                 << ", width=" << width << ", frame=" << frame);
                    ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame();
                    ImGui::GetIO().DisplaySize = ImVec2(width, 700);
                    ImGui::NewFrame();
                    toolbar.Render(); sub.Render();
                    EXPECT_TRUE(toolbar.HasSelectedToolPos());
                    const auto* window = ImGui::FindWindowByName("Toolbar");
                    ASSERT_NE(window, nullptr);
                    EXPECT_LE(window->DC.CursorMaxPos.x, toolbar.GetWindowRect().Max.x + 1);
                    EXPECT_LE(window->DC.CursorMaxPos.y, toolbar.GetWindowRect().Max.y + 1);
                    EXPECT_FLOAT_EQ(toolbar.GetWindowRect().GetHeight(),
                                    editor ? std::floor(Toolbar::EditorHeight * scale) : Toolbar::FloatingHeight * scale);
                    if (editor) {
                        EXPECT_NEAR(toolbar.GetSelectedToolPos().y + toolbar.GetSelectedIconSize() / 2,
                                    toolbar.GetWindowRect().GetCenter().y, 1);
                        // ImGui 根据上一帧内容计算滚动范围，切换工具栏模式后检查稳定帧。
                        if (frame > 0) EXPECT_LE(window->ScrollMax.y, 1);
                    }
                    EXPECT_LE(toolbar.GetWindowRect().Max.x, width + 1);
                    EXPECT_LE(sub.GetWindowRect().Max.x, width + 1);
                    ImGui::Render();
                }
            }
        }
    }
}

TEST_F(ScrollCaptureGpuOutputTest, TitlebarInsetPreservesIconGeometryAndExcludesControlsFromDragging) {
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(320, 240), ImVec2(320, 240), nullptr, 1280);
    canvas.SetTool(ToolType::Number);
    Toolbar toolbar;
    toolbar.SetCanvas(&canvas); toolbar.SetEditorMode(true);
    constexpr float contentWidth = 760, titlebarInset = 80, viewportHeight = 500;
    constexpr float vertexTolerance = 0.001f;
    std::vector<ImDrawVert> baseline;
    const ImU32 background = GetThemePalette(Theme::Dark).editorToolbarBackground;
    for (float inset : {0.0f, titlebarInset}) {
        toolbar.SetEditorLeadingInset(inset);
        for (int frame = 0; frame < 2; ++frame) {
            ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame();
            auto& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(contentWidth + inset, viewportHeight);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            ImGui::NewFrame(); toolbar.Render();
            ImGui::Render();
        }
        EXPECT_FLOAT_EQ(toolbar.GetSelectedIconSize(), Toolbar::IconSize);
        const auto selected = toolbar.GetSelectedToolPos();
        EXPECT_FALSE(toolbar.IsEditorDragArea(ImVec2(selected.x + Toolbar::IconSize / 2, selected.y + Toolbar::IconSize / 2)));
        EXPECT_TRUE(toolbar.IsEditorDragArea(ImVec2(selected.x, 0)));
        EXPECT_FALSE(toolbar.IsEditorDragArea(ImVec2(selected.x, Toolbar::EditorHeight)));
        if (inset > 0) EXPECT_FALSE(toolbar.IsEditorDragArea(ImVec2(inset / 2, Toolbar::EditorHeight / 2)));
        const auto* window = ImGui::FindWindowByName("Toolbar");
        ASSERT_NE(window, nullptr);
        std::vector<ImDrawVert> vertices;
        for (auto vertex : window->DrawList->VtxBuffer) {
            // 排除随窗口宽度变化的背景，比较实际图标、描边和按钮的三角网格。
            if (vertex.col == background) continue;
            vertex.pos.x -= inset;
            vertices.push_back(vertex);
        }
        if (baseline.empty()) { baseline = std::move(vertices); continue; }
        ASSERT_EQ(vertices.size(), baseline.size());
        for (size_t index = 0; index < vertices.size(); ++index) {
            EXPECT_NEAR(vertices[index].pos.x, baseline[index].pos.x, vertexTolerance);
            EXPECT_NEAR(vertices[index].pos.y, baseline[index].pos.y, vertexTolerance);
            EXPECT_EQ(vertices[index].col, baseline[index].col);
        }
    }
}

TEST_F(ScrollCaptureGpuOutputTest, NumberLabelEditorInputAndAutomaticAnchorsSurviveExportAndRestore) {
    constexpr int width = 1000, height = 700, channels = 4;
    constexpr float markerX = 250, initialMarkerY = 400, movedMarkerY = 100;
    constexpr ImU32 markerColor = IM_COL32(210, 60, 30, 255);
    auto image = std::make_shared<ScrollImage>();
    image->width = width; image->height = height;
    image->rgba.assign(size_t(width) * height * channels, 255);
    ScreenOverlay::Config config;
    ImGuiUtils::ContextScope restore(context);
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    auto* canvas = editor.GetCanvas();
    canvas->SetTool(ToolType::Number);
    canvas->SetToolColor(Colors::PresetBlue);
    canvas->OnMouseDown(ImVec2(markerX, initialMarkerY));
    canvas->OnMouseUp(ImVec2(markerX, initialMarkerY));
    ASSERT_TRUE(canvas->IsTextInputActive());
    canvas->SetSelectedLayerColor(markerColor);
    editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "number-label-empty.png");
    ImGui::GetIO().AddInputCharactersUTF8("位置说明");
    editor.RenderFrame();
    auto* marker = dynamic_cast<NumberLayer*>(canvas->GetLayerManager().GetLayers()[0].get());
    ASSERT_NE(marker, nullptr);
    EXPECT_EQ(marker->GetColor(), markerColor);
    EXPECT_EQ(marker->GetLabel()->GetText(), "位置说明");
    const auto frame = marker->GetLabelFrameBounds();
    EXPECT_FLOAT_EQ(marker->GetConnector().first.y, frame.Max.y);
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "number-label-input.png");
    canvas->OnMouseDown(ImVec2(markerX, initialMarkerY));
    canvas->OnMouseMove(ImVec2(markerX, movedMarkerY));
    canvas->OnMouseUp(ImVec2(markerX, movedMarkerY));
    EXPECT_FALSE(canvas->IsTextInputActive());
    EXPECT_FLOAT_EQ(marker->GetCenter().y, initialMarkerY); // 失焦手势不拖动圆点或创建新标注
    ASSERT_EQ(canvas->GetLayerManager().GetLayers().size(), 1u);
    canvas->OnMouseDown(ImVec2(markerX, initialMarkerY));
    canvas->OnMouseMove(ImVec2(markerX, movedMarkerY));
    canvas->OnMouseUp(ImVec2(markerX, movedMarkerY));
    EXPECT_FLOAT_EQ(marker->GetLabelFrameBounds().Min.y, frame.Min.y);
    EXPECT_FLOAT_EQ(marker->GetConnector().first.y, frame.Min.y);
    editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "number-label-moved.png");
    auto output = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        canvas, canvas->GetSelection(), 1);
    ASSERT_NE(output.data, nullptr);
    ASSERT_EQ(output.width, width); ASSERT_EQ(output.height, height);
    const auto pixel = [&](int x, int y) { return output.data.get() + (size_t(y) * width + x) * channels; };
    const auto [start, end] = marker->GetConnector();
    EXPECT_LT(pixel(int(markerX), int((start.y + end.y) / 2))[1], 100); // 框上方连线确实导出
    EXPECT_EQ(pixel(int(markerX), int(frame.Min.y + TextLayer::FramePadding / 2))[1], 255); // 连线不能穿进框内
    const int borderY = int(frame.Min.y);
    for (int x = int(frame.Min.x) + 1; x < int(frame.Max.x) - 1; ++x) {
        const unsigned char* borderPixel = pixel(x, borderY);
        for (int y = borderY - 1; y <= borderY + 1; ++y)
            if (pixel(x, y)[1] < borderPixel[1]) borderPixel = pixel(x, y);
        EXPECT_LT(borderPixel[1], 240) << x; // 实线边框连续导出，不能出现虚线间隙
        EXPECT_GT(borderPixel[0], borderPixel[1]) << x;
        EXPECT_GT(borderPixel[1], borderPixel[2]) << x; // 边框和圆点均为同一红色，不能回退为灰边
    }
    const std::string preview = std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/number-label-export.png";
    ASSERT_TRUE(WritePngToFile(output.data.get(), width, height, preview));
    std::string error;
    auto document = canvas->ExportDocument(error);
    ASSERT_TRUE(document) << error;
    const auto releaseTexture = [this](SDL_GPUTexture* texture) { SDL_ReleaseGPUTexture(device, texture); };
    const std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> restoredTexture(
        CreateScreenshotTexture(device, width, height, document->originalRgba.data(), width * channels), releaseTexture);
    ASSERT_NE(restoredTexture, nullptr);
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, (ImTextureID)(intptr_t)restoredTexture.get(), error)) << error;
    auto second = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        &restored, restored.GetSelection(), 1);
    ASSERT_NE(second.data, nullptr);
    EXPECT_EQ(std::memcmp(second.data.get(), output.data.get(), size_t(width) * height * channels), 0);
}

TEST_F(ScrollCaptureGpuOutputTest, HighlightKeepsRegionsBrightAndExportsOneSharedMask) {
    constexpr int width = 1000, height = 700, channels = 4;
    constexpr std::array<unsigned char, channels> background{180, 210, 240, 255};
    auto image = std::make_shared<ScrollImage>();
    image->width = width; image->height = height;
    image->rgba.resize(size_t(width) * height * channels);
    for (size_t i = 0; i < image->rgba.size(); i += channels)
        std::copy(background.begin(), background.end(), image->rgba.begin() + i);
    ScreenOverlay::Config config;
    config.language = Language::ChineseSimplified;
    ImGuiUtils::ContextScope restore(context);
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    auto* canvas = editor.GetCanvas();
    canvas->SetTool(ToolType::Highlight);
    canvas->OnMouseDown(ImVec2(100, 200)); canvas->OnMouseMove(ImVec2(430, 500)); canvas->OnMouseUp(ImVec2(430, 500));
    canvas->SetHighlightEllipse(true);
    canvas->OnMouseDown(ImVec2(350, 150)); canvas->OnMouseMove(ImVec2(750, 550)); canvas->OnMouseUp(ImVec2(750, 550));
    ASSERT_EQ(canvas->GetLayerManager().GetLayers().size(), 2u);
    editor.RenderFrame(); editor.RenderFrame();
    auto* panel = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(panel, nullptr);
    const float optionSize = panel->Size.y - 2 * panel->WindowPadding.y;
    const ImVec2 rectangleButton(panel->Pos.x + panel->WindowPadding.x + optionSize / 2,
                                panel->Pos.y + panel->WindowPadding.y + optionSize / 2);
    ClickWindow(editor, rectangleButton.x, rectangleButton.y);
    EXPECT_FALSE(canvas->GetHighlightEllipse());
    // 滑块位于透明度数值左侧，点击轨道右侧应提高透明度。
    panel = ImGui::FindWindowByName("SubToolbar");
    ClickWindow(editor, panel->Pos.x + panel->Size.x - panel->WindowPadding.x - panel->Size.y,
        panel->Pos.y + panel->Size.y / 2);
    EXPECT_GT(canvas->GetHighlightTransparency(), HighlightSettings::DefaultTransparency);
    canvas->SetHighlightTransparency(HighlightSettings::DefaultTransparency);
    canvas->SetHighlightEllipse(true);
    editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "highlight-editor-preview.png");
    const ImVec2 brightPoints[] = {{200, 300}, {550, 350}, {400, 350}};
    const ImVec2 darkPoints[] = {{360, 160}, {950, 650}};
    for (int percent : {0, 25, 50, 100}) {
        canvas->SetHighlightTransparency(percent);
        auto output = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
            canvas, canvas->GetSelection(), 1);
        ASSERT_NE(output.data, nullptr);
        const auto pixel = [&](ImVec2 point) { return output.data.get() + (size_t(point.y) * width + size_t(point.x)) * channels; };
        for (auto point : brightPoints) {
            for (int channel = 0; channel < channels; ++channel) EXPECT_EQ(pixel(point)[channel], background[channel]);
        }
        for (auto point : darkPoints) {
            for (int channel = 0; channel < channels - 1; ++channel)
                EXPECT_NEAR(pixel(point)[channel], background[channel] * percent / HighlightSettings::MaximumTransparency, 1);
            EXPECT_EQ(pixel(point)[channels - 1], 255);
        }
        if (percent == HighlightSettings::DefaultTransparency) {
            size_t artifacts = 0;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const float ellipseX = (static_cast<float>(x) + 0.5f - 550) / 200;
                    const float ellipseY = (static_cast<float>(y) + 0.5f - 350) / 200;
                    const float distanceSquared = ellipseX * ellipseX + ellipseY * ellipseY;
                    constexpr float edgeTolerance = 0.02f;
                    if (std::abs(distanceSquared - 1) < edgeTolerance) continue;
                    const bool inside = (x >= 100 && x < 430 && y >= 200 && y < 500) || distanceSquared < 1;
                    const auto sample = pixel(ImVec2(x, y));
                    for (int channel = 0; channel < channels - 1; ++channel) {
                        const int expected = inside ? background[channel] : background[channel] * percent / HighlightSettings::MaximumTransparency;
                        if (std::abs(int(sample[channel]) - expected) > 1) { ++artifacts; break; }
                    }
                }
            }
            EXPECT_EQ(artifacts, 0u); // 全图检查共享边，不能残留漏亮点或重复混合的暗点
            ASSERT_TRUE(WritePngToFile(output.data.get(), width, height,
                std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/highlight-export.png"));
        }
    }
    canvas->SetHighlightTransparency(65);
    std::string error;
    auto document = canvas->ExportDocument(error);
    ASSERT_TRUE(document) << error;
    const auto release = [this](SDL_GPUTexture* texture) { SDL_ReleaseGPUTexture(device, texture); };
    const std::unique_ptr<SDL_GPUTexture, decltype(release)> texture(
        CreateScreenshotTexture(device, width, height, document->originalRgba.data(), width * channels), release);
    ASSERT_NE(texture, nullptr);
    Canvas restored;
    ASSERT_TRUE(restored.RestoreDocument(*document, (ImTextureID)(intptr_t)texture.get(), error)) << error;
    auto originalOutput = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        canvas, canvas->GetSelection(), 1);
    auto restoredOutput = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        &restored, restored.GetSelection(), 1);
    ASSERT_NE(originalOutput.data, nullptr); ASSERT_NE(restoredOutput.data, nullptr);
    EXPECT_EQ(std::memcmp(originalOutput.data.get(), restoredOutput.data.get(), image->rgba.size()), 0);
    // 同一文档改用 Retina 逻辑坐标，输出仍须保持相同的实际像素和高亮位置。
    auto retinaDocument = *document;
    constexpr float retinaDensity = 2;
    retinaDocument.logicalSize.x /= retinaDensity; retinaDocument.logicalSize.y /= retinaDensity;
    retinaDocument.pixelScale = {retinaDensity, retinaDensity};
    for (auto& layer : retinaDocument.layers) {
        auto& highlight = std::get<DocumentHighlight>(layer.data);
        highlight.start.x /= retinaDensity; highlight.start.y /= retinaDensity;
        highlight.end.x /= retinaDensity; highlight.end.y /= retinaDensity;
    }
    Canvas retina;
    ASSERT_TRUE(retina.RestoreDocument(retinaDocument, (ImTextureID)(intptr_t)texture.get(), error)) << error;
    auto retinaOutput = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        &retina, retina.GetSelection(), retinaDensity);
    ASSERT_NE(retinaOutput.data, nullptr);
    EXPECT_EQ(retinaOutput.width, width); EXPECT_EQ(retinaOutput.height, height);
    EXPECT_EQ(std::memcmp(originalOutput.data.get(), retinaOutput.data.get(), image->rgba.size()), 0);
    restored.RotateImageCounterClockwise();
    auto rotated = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        &restored, restored.GetSelection(), 1);
    ASSERT_NE(rotated.data, nullptr);
    EXPECT_EQ(rotated.width, height); EXPECT_EQ(rotated.height, width);
    const auto rotatedBright = rotated.data.get() + (size_t(width - 1 - 200) * height + 300) * channels;
    for (int channel = 0; channel < channels; ++channel) EXPECT_EQ(rotatedBright[channel], background[channel]);
}

TEST_F(ScrollCaptureGpuOutputTest, HighlightSubToolbarFitsThemesDpiAndNarrowWindows) {
    Canvas canvas;
    canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(320, 240), ImVec2(320, 240), nullptr, 1280);
    canvas.SetTool(ToolType::Highlight);
    Toolbar toolbar;
    toolbar.SetCanvas(&canvas);
    SubToolbar sub;
    sub.SetCanvas(&canvas); sub.SetToolbar(&toolbar);
    for (auto language : {Language::English, Language::ChineseSimplified}) {
        toolbar.SetLanguage(language); sub.SetLanguage(language);
        for (auto theme : {Theme::Dark, Theme::Light}) {
            toolbar.SetTheme(theme); sub.SetTheme(theme);
            for (bool editor : {false, true}) {
                toolbar.SetEditorMode(editor);
                for (float scale : {1.0f, 1.5f, 2.0f}) {
                    toolbar.SetUIScale(scale); sub.SetUIScale(scale);
                    for (int width : {380, 640, 1280}) {
                        SCOPED_TRACE(testing::Message() << "width=" << width << ", scale=" << scale << ", editor=" << editor);
                        ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::GetIO().DisplaySize = ImVec2(width, 700);
                        ImGui::NewFrame(); toolbar.Render(); sub.Render();
                        auto* window = ImGui::FindWindowByName("SubToolbar");
                        ASSERT_NE(window, nullptr);
                        EXPECT_TRUE(toolbar.HasSelectedToolPos());
                        EXPECT_LE(window->DC.CursorMaxPos.x, sub.GetWindowRect().Max.x + 1);
                        EXPECT_LE(window->DC.CursorMaxPos.y, sub.GetWindowRect().Max.y + 1);
                        EXPECT_LE(sub.GetWindowRect().Max.x, width + 1);
                        ImGui::Render();
                    }
                }
            }
        }
    }
}

TEST_F(ScrollCaptureGpuOutputTest, HighlightMaskStaysBelowExistingAndNewAnnotations) {
    constexpr int width = 1000, height = 700, channels = 4;
    constexpr ImU32 annotationColor = IM_COL32(230, 30, 20, 255);
    constexpr std::array<unsigned char, channels> annotationPixel{230, 30, 20, 255};
    constexpr std::array<unsigned char, channels> backgroundPixel{180, 210, 240, 255};
    constexpr float firstRowY = 40, secondRowY = 560, rowHeight = 40, strokeWidth = 6;
    auto image = std::make_shared<ScrollImage>();
    image->width = width; image->height = height;
    image->rgba.resize(size_t(width) * height * channels);
    for (size_t i = 0; i < image->rgba.size(); i += channels)
        std::copy(backgroundPixel.begin(), backgroundPixel.end(), image->rgba.begin() + i);
    ImGuiUtils::ContextScope restore(context);
    ImageEditor editor({}, image, device);
    ASSERT_TRUE(editor.Create());
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    auto* canvas = editor.GetCanvas();
    auto& manager = canvas->GetLayerManager();
    const auto addAnnotations = [&](float y) {
        auto rectangle = std::make_unique<RectangleLayer>(ImVec2(20, y), ImVec2(80, y + rowHeight), annotationColor, strokeWidth);
        rectangle->SetFilled(true); rectangle->SetShowBorder(false);
        manager.AddLayer(std::move(rectangle));
        auto ellipse = std::make_unique<EllipseLayer>(ImVec2(110, y), ImVec2(170, y + rowHeight), annotationColor, strokeWidth);
        ellipse->SetFilled(true); ellipse->SetShowBorder(false);
        manager.AddLayer(std::move(ellipse));
        auto arrow = std::make_unique<ArrowLayer>(ImVec2(200, y + rowHeight / 2), ImVec2(260, y + rowHeight / 2), annotationColor, strokeWidth);
        arrow->SetShowHandles(false); manager.AddLayer(std::move(arrow));
        manager.AddLayer(std::make_unique<BrushLayer>(std::vector<ImVec2>{{290, y + rowHeight / 2}, {350, y + rowHeight / 2}}, annotationColor, strokeWidth));
        manager.AddLayer(std::make_unique<TextLayer>(ImVec2(380, y), "TEXT", annotationColor, 24, 100));
        auto number = std::make_unique<NumberLayer>(ImVec2(550, y + rowHeight / 2), 1, annotationColor, 32);
        number->SetLabel(ImVec2(650, y), "NOTE", 20, 100, 1);
        manager.AddLayer(std::move(number));
    };
    addAnnotations(firstRowY);
    canvas->SetTool(ToolType::Highlight);
    canvas->OnMouseDown(ImVec2(300, 160)); canvas->OnMouseMove(ImVec2(600, 480)); canvas->OnMouseUp(ImVec2(600, 480));
    addAnnotations(secondRowY);
    canvas->SetTool(ToolType::Rectangle);
    canvas->SetToolColor(Colors::PresetGreen); canvas->SetToolFill(true);
    canvas->OnMouseDown(ImVec2(820, 600)); canvas->OnMouseMove(ImVec2(940, 660));
    editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "highlight-annotation-preview.png");
    canvas->OnMouseUp(ImVec2(940, 660));
    auto output = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        canvas, canvas->GetSelection(), 1);
    ASSERT_NE(output.data, nullptr);
    const auto pixel = [&](int x, int y) { return output.data.get() + (size_t(y) * width + x) * channels; };
    for (const float y : {firstRowY, secondRowY}) {
        for (const int x : {50, 140, 230, 320, 539})
            for (int channel = 0; channel < channels; ++channel)
                EXPECT_EQ(pixel(x, int(y + rowHeight / 2))[channel], annotationPixel[channel]) << x << "," << y;
        for (const auto textRange : {ImVec2(380, 480), ImVec2(640, 750)}) {
            int originalColorPixels = 0;
            for (int row = int(y); row < int(y + rowHeight); ++row)
                for (int x = int(textRange.x); x < int(textRange.y); ++x)
                    if (std::memcmp(pixel(x, row), annotationPixel.data(), channels) == 0) ++originalColorPixels;
            EXPECT_GT(originalColorPixels, 10);
        }
    }
    const auto* green = pixel(880, 630);
    EXPECT_EQ(green[0], (Colors::PresetGreen >> IM_COL32_R_SHIFT) & 255);
    EXPECT_EQ(green[1], (Colors::PresetGreen >> IM_COL32_G_SHIFT) & 255);
    EXPECT_NEAR(pixel(980, 680)[0], backgroundPixel[0] / 2, 1);
    EXPECT_EQ(pixel(500, 300)[0], backgroundPixel[0]);
    ASSERT_TRUE(WritePngToFile(output.data.get(), width, height,
        std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/highlight-annotation-export.png"));
}

TEST_F(ScrollCaptureGpuOutputTest, WatermarkPreservesPixelsAcrossCropRestoreDpiAndRotation) {
    ImGuiUtils::ContextScope restore(context);
    auto image = std::make_shared<ScrollImage>();
    image->width = 480; image->height = 320;
    image->rgba.assign(size_t(image->width) * image->height * 4, 255);
    ScreenOverlay::Config config;
    config.language = Language::ChineseSimplified;
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    editor.RenderFrame();
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    auto* canvas = editor.GetCanvas();
    auto empty = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        canvas, canvas->GetSelection(), 1);
    ASSERT_TRUE(empty.data);
    canvas->SetTool(ToolType::Watermark);
    canvas->SetWatermarkText("水印测试");
    canvas->SetWatermarkOpacity(65);
    canvas->SetSelectedLayerColor(Colors::PresetBlue);
    editor.RenderFrame(); editor.RenderFrame();
    SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(), "watermark-editor-preview.png");
    auto marked = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
        canvas, canvas->GetSelection(), 1);
    ASSERT_TRUE(marked.data);
    EXPECT_NE(std::memcmp(empty.data.get(), marked.data.get(), size_t(marked.width) * marked.height * 4), 0);
    ASSERT_TRUE(WritePngToFile(marked.data.get(), marked.width, marked.height,
        std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/watermark-export.png"));
    const ImRect crop(ImVec2(40, 32), ImVec2(410, 280));
    std::string error;
    for (int turns = 0; turns < 4; ++turns) {
        ImGui::SetCurrentContext(editor.GetImGuiContext());
        for (const bool cropping : {false, true}) {
            SCOPED_TRACE(testing::Message() << "rotation=" << turns << ", crop=" << cropping);
            const auto region = cropping ? crop : canvas->GetSelection();
            auto expected = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
                canvas, region, 1);
            ASSERT_TRUE(expected.data);
            auto document = cropping ? canvas->ExportCroppedDocument(crop, error) : canvas->ExportDocument(error);
            ASSERT_TRUE(document) << error;
            auto snapshot = std::make_shared<CaptureDocument>(std::move(*document));
            ImageEditor target(snapshot, config, device);
            ASSERT_TRUE(target.Create());
            EXPECT_EQ(target.GetCanvas()->GetWatermarkOpacity(), 65);
            for (const float uiScale : {1.0f, 2.0f}) {
                target.GetCanvas()->SetUIScale(uiScale);
                target.RenderFrame();
                auto actual = FBORenderer::RenderToOffscreen(device, target.GetSDLWindow(), target.GetImGuiContext(),
                    target.GetCanvas(), target.GetCanvas()->GetSelection(), 2);
                ASSERT_TRUE(actual.data);
                ASSERT_EQ(actual.width, expected.width);
                ASSERT_EQ(actual.height, expected.height);
                if (turns == 0 && !cropping && uiScale == 1) {
                    EXPECT_EQ(target.GetCanvas()->GetToolColor(), canvas->GetToolColor());
                    ASSERT_TRUE(WritePngToFile(actual.data.get(), actual.width, actual.height,
                        std::string(SHOTLET_HISTORY_PREVIEW_DIR) + "/watermark-restored-export.png"));
                }
                // 不同窗口的动态字体图集排列不同，斜字边缘会有少量线性采样差异。
                // 同时约束差异像素数与平均误差，缺字、错色或位移均不能靠平均值掩盖。
                constexpr double maxDifferentPixelFraction = 0.001;
                constexpr double maxAverageChannelDifference = 0.01;
                const size_t pixels = size_t(actual.width) * actual.height;
                size_t differentPixels = 0, totalDifference = 0;
                for (size_t pixel = 0; pixel < pixels; ++pixel) {
                    int maximumDifference = 0;
                    for (size_t channel = 0; channel < 4; ++channel) {
                        const size_t index = pixel * 4 + channel;
                        const int difference = std::abs(int(actual.data[index]) - int(expected.data[index]));
                        totalDifference += static_cast<size_t>(difference);
                        maximumDifference = std::max(maximumDifference, difference);
                    }
                    if (maximumDifference > 1) ++differentPixels;
                }
                EXPECT_LE(static_cast<double>(differentPixels) / static_cast<double>(pixels), maxDifferentPixelFraction);
                EXPECT_LE(static_cast<double>(totalDifference) / static_cast<double>(pixels * 4), maxAverageChannelDifference);
            }
            ImGui::SetCurrentContext(editor.GetImGuiContext());
        }
        canvas->RotateImageCounterClockwise();
    }
}

TEST_F(ScrollCaptureGpuOutputTest, WatermarkEscapeEndsInputBeforeClosingEditor) {
    ImGuiUtils::ContextScope restore(context);
    auto document = std::make_shared<CaptureDocument>();
    document->width = 480; document->height = 320;
    document->logicalSize = {480, 320};
    document->originalRgba.assign(size_t(document->width) * document->height * 4, 255);
    OverlayManager manager;
    ScreenOverlay::Config config;
    ASSERT_TRUE(manager.OpenDocument(document, config, device));
    manager.ProcessFrame();
    auto* canvas = manager.GetResultCanvas();
    ASSERT_NE(canvas, nullptr);
    canvas->SetWatermarkText("Watermark");
    canvas->SetTool(ToolType::Watermark);
    manager.ProcessFrame(); manager.ProcessFrame();
    auto* gui = manager.GetResultImGuiContext();
    ImGui::SetCurrentContext(gui);
    auto* toolbar = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(toolbar, nullptr);
    const auto inputId = toolbar->GetID("##watermark_input");
    ASSERT_EQ(ImGui::GetActiveID(), inputId);
    ImGui::GetIO().AddInputCharactersUTF8("X");
    manager.ProcessFrame();
    EXPECT_EQ(canvas->GetWatermarkText(), "X"); // 默认聚焦会全选原文字，输入替换选区。
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = SDL_GetWindowID(manager.GetResultSDLWindow());
    event.key.key = SDLK_ESCAPE;
    event.key.scancode = SDL_SCANCODE_ESCAPE;
    ASSERT_TRUE(SDL_PushEvent(&event));
    manager.ProcessFrame();
    EXPECT_FALSE(manager.IsCancelled());
    ImGui::SetCurrentContext(gui);
    EXPECT_NE(ImGui::GetActiveID(), inputId);
    EXPECT_EQ(canvas->GetWatermarkText(), "Watermark");
    event.type = SDL_EVENT_KEY_UP;
    ASSERT_TRUE(SDL_PushEvent(&event));
    manager.ProcessFrame();
    event.type = SDL_EVENT_KEY_DOWN;
    ASSERT_TRUE(SDL_PushEvent(&event));
    manager.ProcessFrame();
    EXPECT_TRUE(manager.IsCancelled());
}

TEST_F(ScrollCaptureGpuOutputTest, WatermarkOpacitySliderChangesPreviewAndExport) {
    ImGuiUtils::ContextScope restore(context);
    constexpr int width = 480, height = 320, channels = 4;
    auto image = std::make_shared<ScrollImage>();
    image->width = width; image->height = height;
    image->rgba.assign(size_t(width) * height * channels, 255);
    ScreenOverlay::Config config;
    config.language = Language::ChineseSimplified;
    ImageEditor editor(config, image, device);
    ASSERT_TRUE(editor.Create());
    ImGui::SetCurrentContext(editor.GetImGuiContext());
    auto* canvas = editor.GetCanvas();
    canvas->SetTool(ToolType::Watermark);
    canvas->SetWatermarkText("水印测试");
    canvas->SetSelectedLayerColor(IM_COL32(0, 0, 0, 255));
    ASSERT_EQ(canvas->GetWatermarkOpacity(), 30);
    for (const auto theme : {Theme::Light, Theme::Dark}) {
        editor.SetTheme(theme);
        editor.RenderFrame(); editor.RenderFrame();
        SaveWindowFrame(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
            theme == Theme::Light ? "watermark-opacity-light.png" : "watermark-opacity-dark.png");
    }
    auto* panel = ImGui::FindWindowByName("SubToolbar");
    ASSERT_NE(panel, nullptr);
    ClickWindow(editor, panel->Pos.x + panel->Size.x - panel->WindowPadding.x - panel->Size.y,
        panel->Pos.y + panel->Size.y / 2);
    EXPECT_GT(canvas->GetWatermarkOpacity(), 30);
    EXPECT_EQ(canvas->GetWatermarkText(), "水印测试");
    uint64_t previousDarkness = 0;
    uint64_t partialDarkness = 0;
    for (const int opacity : {0, 30, 100}) {
        canvas->SetWatermarkOpacity(opacity);
        editor.RenderFrame();
        auto output = FBORenderer::RenderToOffscreen(device, editor.GetSDLWindow(), editor.GetImGuiContext(),
            canvas, canvas->GetSelection(), 1);
        ASSERT_TRUE(output.data);
        ASSERT_EQ(output.width, width); ASSERT_EQ(output.height, height);
        uint64_t darkness = 0;
        for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel) darkness += 255 - output.data[pixel * channels];
        if (opacity == 0) EXPECT_EQ(darkness, 0u);
        else EXPECT_GT(darkness, previousDarkness);
        if (opacity == 30) partialDarkness = darkness;
        if (opacity == 100) {
            // 以实际 GPU 混合后的像素总量核对 30% 的显示强度，容许 8 位量化误差。
            EXPECT_NEAR(static_cast<double>(partialDarkness) / static_cast<double>(darkness), 0.30, 0.02);
        }
        previousDarkness = darkness;
    }
}
