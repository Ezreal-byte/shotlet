#include "ui/SettingsWindow.h"
#include "platform/ApplicationPlatform.h"
#include "platform/PlatformMainContext.h"
#include "history/DocumentCodec.h"
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <imgui_impl_sdlgpu3.h>
#include <array>
#include <cmath>
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>

namespace shotlet {
class SettingsWindowTest : public testing::Test {
protected:
    static constexpr int LogicalWidth = 640;
    static constexpr int LogicalHeight = 560;
    static constexpr int SidebarLogicalWidth = 160;
    static constexpr int LogsPage = 3;
    static constexpr float LogLocationButtonCenterX = 550;
    static constexpr float LogLocationButtonCenterY = 129;
    static constexpr float ViewLogButtonCenterX = 235;
    static constexpr float ViewLogButtonCenterY = 224;
    static constexpr int NativeCloseCycles = 3;
    SDL_GPUDevice* device=nullptr;
    std::unique_ptr<SettingsWindow> window;
    std::vector<Preferences> saved;
    bool allowSave=true;
    std::filesystem::path chosenLogDirectory;
    int viewedLogs = 0;
    bool allowViewLog = true;
    void SetUp() override {
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        ApplyApplicationPlatformSDLHints(); ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
        device=CreateApplicationGpuDevice(false); ASSERT_NE(device,nullptr)<<SDL_GetError();
        SettingsWindow::Callbacks callbacks;
        callbacks.save=[this](const Preferences& preferences,std::string& error) {
            if (!allowSave) { error="模拟保存失败"; return false; }
            if (preferences.HasDuplicateHotkeys()) {
                error="快捷键重复"; return false;
            }
            saved.push_back(preferences); return true;
        };
        callbacks.enableHistory=[](bool,std::string&) { return true; };
        callbacks.clearHistory=[](std::string&) { return true; };
        callbacks.chooseDirectory=[] { return std::filesystem::path(); };
        callbacks.chooseLogDirectory = [this] { return chosenLogDirectory; };
        callbacks.saveLogDirectory = [this, save = callbacks.save](const std::filesystem::path& directory, std::string& error) {
            auto preferences = window->m_preferences;
            preferences.logDirectory = directory;
            return save(preferences, error);
        };
        callbacks.viewLog = [this](std::string& error) {
            ++viewedLogs;
            if (allowViewLog) return true;
            error = "模拟打开失败";
            return false;
        };
        window=std::make_unique<SettingsWindow>(device,Preferences{},Language::ChineseSimplified,Theme::Dark,std::move(callbacks));
        SettingsWindow::StorageInfo storage; storage.directory="/Users/jiaqi/Pictures/Shotlet";
        storage.used=45*1024*1024; storage.count=26; storage.capacity=1024ull*1024*1024*1024;
        storage.available=320ull*1024*1024*1024; storage.ready=true; window->SetStorageInfo(storage);
        window->SetLogPath("/Users/jiaqi/Library/Logs/Shotlet/runtime.log");
        ASSERT_TRUE(window->Show())<<SDL_GetError();
        int width=0,height=0;
        ASSERT_TRUE(SDL_GetWindowSize(window->m_window,&width,&height));
        EXPECT_EQ(width,LogicalWidth); EXPECT_EQ(height,LogicalHeight);
        ASSERT_TRUE(SDL_GetWindowSizeInPixels(window->m_window,&width,&height));
        const float density=SDL_GetWindowPixelDensity(window->m_window);
        EXPECT_EQ(width,std::lround(LogicalWidth*density)); EXPECT_EQ(height,std::lround(LogicalHeight*density));
        EXPECT_EQ(SDL_GetWindowFlags(window->m_window)&SDL_WINDOW_RESIZABLE,0u);
    }
    void TearDown() override { window.reset(); DestroyApplicationGpuDevice(device,false); SDL_QuitSubSystem(SDL_INIT_VIDEO); }
    void Page(int page) { window->m_page=page; window->RenderFrame(); }
    int CurrentPage() { return window->m_page; }
    void Click(float x,float y) {
        ImGui::SetCurrentContext(window->m_context);
        auto& io=ImGui::GetIO(); io.ConfigInputTrickleEventQueue=false;
        SDL_Event event{}; event.type=SDL_EVENT_MOUSE_MOTION; event.motion.windowID=SDL_GetWindowID(window->m_window);
        event.motion.x=x; event.motion.y=y; window->ProcessEvent(event);
        event={}; event.type=SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.windowID=SDL_GetWindowID(window->m_window);
        event.button.button=SDL_BUTTON_LEFT; window->ProcessEvent(event);
        io.AddMouseButtonEvent(0,false); window->RenderFrame();
        io.AddMousePosEvent(x,y); io.AddMouseButtonEvent(0,true); window->RenderFrame();
        io.AddMouseButtonEvent(0,false); window->RenderFrame();
        event.type=SDL_EVENT_MOUSE_BUTTON_UP; window->ProcessEvent(event);
    }
    void ClickRecorder(int index) {
        constexpr float firstRecorderCenterY = 129.f;
        constexpr float recorderRowStride = 43.f;
        int width=0; SDL_GetWindowSize(window->m_window,&width,nullptr);
        Click(width-170*window->m_scale,(firstRecorderCenterY+index*recorderRowStride)*window->m_scale);
    }
    void Record(int index) { window->m_recording=index; }
    HotkeyBinding Binding(int index) { return window->m_preferences.Hotkeys()[size_t(index)]; }
    void Key(SDL_Keycode key, SDL_Scancode scan, int raw, SDL_Keymod mod=SDL_KMOD_NONE) {
        SDL_Event event{}; event.type=SDL_EVENT_KEY_DOWN; event.key.windowID=SDL_GetWindowID(window->m_window);
        event.key.key=key; event.key.scancode=scan; event.key.raw=raw; event.key.mod=mod;
        ASSERT_TRUE(window->ProcessEvent(event));
    }
    void SavePreview(const char* name) {
        window->RenderFrame();
        ImGui::SetCurrentContext(window->m_context);
        EXPECT_EQ(ImGui::GetCurrentContext()->ErrorCountCurrentFrame,0);
        auto* data=ImGui::GetDrawData(); ASSERT_NE(data,nullptr);
        const int width=int(data->DisplaySize.x*data->FramebufferScale.x),height=int(data->DisplaySize.y*data->FramebufferScale.y);
        const float density=SDL_GetWindowPixelDensity(window->m_window);
        ASSERT_EQ(width,std::lround(LogicalWidth*density)); ASSERT_EQ(height,std::lround(LogicalHeight*density));
        SDL_GPUTextureCreateInfo info{}; info.type=SDL_GPU_TEXTURETYPE_2D;
        info.format=SDL_GetGPUSwapchainTextureFormat(device,window->m_window); info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width=width; info.height=height; info.layer_count_or_depth=1; info.num_levels=1; info.sample_count=SDL_GPU_SAMPLECOUNT_1;
        auto* texture=SDL_CreateGPUTexture(device,&info); ASSERT_NE(texture,nullptr);
        SDL_GPUTransferBufferCreateInfo bufferInfo{}; bufferInfo.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bufferInfo.size=width*height*4;
        auto* buffer=SDL_CreateGPUTransferBuffer(device,&bufferInfo); ASSERT_NE(buffer,nullptr);
        auto* command=SDL_AcquireGPUCommandBuffer(device); ASSERT_NE(command,nullptr);
        ImGui_ImplSDLGPU3_PrepareDrawData(data,command);
        SDL_GPUColorTargetInfo target{}; target.texture=texture; target.load_op=SDL_GPU_LOADOP_CLEAR; target.store_op=SDL_GPU_STOREOP_STORE;
        auto* pass=SDL_BeginGPURenderPass(command,&target,1,nullptr); ASSERT_NE(pass,nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(data,command,pass); SDL_EndGPURenderPass(pass);
        auto* copy=SDL_BeginGPUCopyPass(command);
        SDL_GPUTextureRegion source{}; source.texture=texture; source.w=width; source.h=height; source.d=1;
        SDL_GPUTextureTransferInfo destination{}; destination.transfer_buffer=buffer; destination.pixels_per_row=width; destination.rows_per_layer=height;
        SDL_DownloadFromGPUTexture(copy,&source,&destination); SDL_EndGPUCopyPass(copy);
        auto* fence=SDL_SubmitGPUCommandBufferAndAcquireFence(command); ASSERT_NE(fence,nullptr);
        ASSERT_TRUE(SDL_WaitForGPUFences(device,true,&fence,1)); SDL_ReleaseGPUFence(device,fence);
        auto* pixels=static_cast<unsigned char*>(SDL_MapGPUTransferBuffer(device,buffer,false)); ASSERT_NE(pixels,nullptr);
        std::vector<unsigned char> rgba(pixels,pixels+size_t(width)*height*4);
        if (info.format==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || info.format==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
            for (size_t i=0;i<rgba.size();i+=4) std::swap(rgba[i],rgba[i+2]);
        // 侧栏固定为 160 逻辑单位，Retina 下应为 320 实际像素，不能重复缩放。
        const int sidebarPixelWidth=int(std::lround(SidebarLogicalWidth*density));
        // 新增日志菜单经过窗口中线，取底部空白处检查侧栏宽度，避开菜单高亮。
        const size_t sampleRow = size_t(height - 1) * width;
        const bool dark=window->m_theme==Theme::Dark;
        for (int channel=0;channel<3;++channel) {
            EXPECT_EQ(rgba[(sampleRow+sidebarPixelWidth-1)*4+channel],dark?46:237);
            EXPECT_EQ(rgba[(sampleRow+sidebarPixelWidth)*4+channel],dark?30:250);
        }
        SDL_UnmapGPUTransferBuffer(device,buffer);
        std::string error; EXPECT_TRUE(WriteHistoryPng(std::filesystem::path(SHOTLET_SETTINGS_PREVIEW_DIR)/name,rgba.data(),width,height,error))<<error;
        SDL_ReleaseGPUTransferBuffer(device,buffer); SDL_ReleaseGPUTexture(device,texture);
    }
    bool Recording() { return window->IsRecording(); }
    std::string Error() { return window->m_error; }
    std::filesystem::path LogDirectory() { return window->m_preferences.logDirectory; }
    SDL_Window* GetSDLWindow() const { return window->m_window; }
    void ClickLogLocation() {
        Click(LogLocationButtonCenterX * window->m_scale, LogLocationButtonCenterY * window->m_scale);
    }
    void ClickViewLog() { Click(ViewLogButtonCenterX * window->m_scale, ViewLogButtonCenterY * window->m_scale); }
};

TEST_F(SettingsWindowTest, TwoInlineRecordersSaveImmediatelyAndKeepTheSettingsWindowOpen) {
    Page(1); Record(0); Key(SDLK_RETURN,SDL_SCANCODE_RETURN,kVK_Return,SDL_KMOD_CTRL|SDL_KMOD_ALT);
    ASSERT_EQ(saved.size(),1u); EXPECT_EQ(Binding(0),(HotkeyBinding{HOTKEY_MOD_CTRL|HOTKEY_MOD_ALT,kVK_Return}));
    EXPECT_TRUE(window->IsVisible()); EXPECT_FALSE(Recording());
    const auto capture=Binding(0);
    Record(1); Key(SDLK_F6,SDL_SCANCODE_F6,kVK_F6);
    ASSERT_EQ(saved.size(),2u); EXPECT_EQ(Binding(1),(HotkeyBinding{0,kVK_F6})); EXPECT_EQ(Binding(0),capture);
    EXPECT_TRUE(window->IsVisible()); EXPECT_FALSE(Recording());
}
TEST_F(SettingsWindowTest, CopyLatestRecorderSavesClearsAndRejectsAnExistingShortcut) {
    Page(1); ClickRecorder(2); ASSERT_TRUE(Recording()); Key(SDLK_F8, SDL_SCANCODE_F8, kVK_F8);
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(Binding(2), (HotkeyBinding{0, kVK_F8}));
    EXPECT_TRUE(window->IsVisible());
    Record(2); Key(SDLK_H, SDL_SCANCODE_H, kVK_ANSI_H, SDL_KMOD_GUI | SDL_KMOD_SHIFT);
    EXPECT_EQ(Error(), "快捷键重复"); EXPECT_EQ(saved.size(), 1u);
    Key(SDLK_DELETE, SDL_SCANCODE_DELETE, kVK_ForwardDelete);
    EXPECT_FALSE(Binding(2).IsEnabled()); EXPECT_EQ(saved.size(), 2u);
}
TEST_F(SettingsWindowTest, ClickingTheSidebarAndShortcutFieldsActivatesTheCorrectRecorder) {
    Click(80,184); ASSERT_EQ(CurrentPage(),1);
    ClickRecorder(0); ASSERT_TRUE(Recording());
    Key(SDLK_F6,SDL_SCANCODE_F6,kVK_F6);
    ASSERT_EQ(saved.size(),1u); EXPECT_EQ(Binding(0),(HotkeyBinding{0,kVK_F6}));
    ClickRecorder(1); ASSERT_TRUE(Recording());
    Key(SDLK_F7,SDL_SCANCODE_F7,kVK_F7);
    ASSERT_EQ(saved.size(),2u); EXPECT_EQ(Binding(1),(HotkeyBinding{0,kVK_F7}));
    Click(80,228); EXPECT_EQ(CurrentPage(),2); EXPECT_FALSE(Recording());
}
TEST_F(SettingsWindowTest, InvalidDuplicateAndFailedSavesPreserveCommittedValuesAndEscapeCancelsRecording) {
    Page(1); const auto original=Binding(0); Record(0);
    Key(SDLK_A,SDL_SCANCODE_A,kVK_ANSI_A); EXPECT_TRUE(saved.empty()); EXPECT_EQ(Binding(0),original); EXPECT_TRUE(Recording());
    Key(SDLK_H,SDL_SCANCODE_H,kVK_ANSI_H,SDL_KMOD_GUI|SDL_KMOD_SHIFT);
    EXPECT_TRUE(saved.empty()); EXPECT_EQ(Error(),"快捷键重复"); EXPECT_EQ(Binding(0),original);
    allowSave=false; Key(SDLK_F6,SDL_SCANCODE_F6,kVK_F6);
    EXPECT_EQ(Error(),"模拟保存失败"); EXPECT_EQ(Binding(0),original); EXPECT_TRUE(Recording());
    Key(SDLK_ESCAPE,SDL_SCANCODE_ESCAPE,kVK_Escape); EXPECT_FALSE(Recording()); EXPECT_TRUE(window->IsVisible());
    allowSave=true; Record(1); Key(SDLK_BACKSPACE,SDL_SCANCODE_BACKSPACE,kVK_Delete);
    EXPECT_FALSE(Binding(1).IsEnabled()); EXPECT_EQ(saved.size(),1u);
}
TEST_F(SettingsWindowTest, AllPagesRenderInBothThemesAndEnglishAndProduceReviewablePreviews) {
    for (int theme=0;theme<2;++theme) {
        window->SetTheme(theme==0?Theme::Dark:Theme::Light);
        for (int page=0;page<=LogsPage;++page) {
            Page(page); SavePreview(std::format("settings-{}-{}.png",theme==0?"dark":"light",page).c_str());
        }
    }
    Page(1); Record(0); SavePreview("settings-recording.png");
    window->SetPreferences(Preferences{},Language::English); Page(2); SavePreview("settings-english-storage.png");
    Page(LogsPage); SavePreview("settings-english-logs.png");
}

TEST_F(SettingsWindowTest, LogLocationSavesImmediatelyAndCancellationAndFailurePreservePreferences) {
    Page(LogsPage);
    ClickLogLocation();
    EXPECT_TRUE(saved.empty());
    chosenLogDirectory = std::filesystem::path(u8"/tmp/Shotlet 运行日志");
    ClickLogLocation();
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved.back().logDirectory, chosenLogDirectory);
    EXPECT_EQ(LogDirectory(), chosenLogDirectory);
    // 重选当前偏好目录也要发出切换请求，以覆盖本次启动的命令行日志目录。
    ClickLogLocation();
    ASSERT_EQ(saved.size(), 2u);
    allowSave = false;
    chosenLogDirectory = "/tmp/another-log-directory";
    ClickLogLocation();
    EXPECT_EQ(saved.size(), 2u);
    EXPECT_EQ(LogDirectory(), saved.back().logDirectory);
    EXPECT_EQ(Error(), "模拟保存失败");
    EXPECT_TRUE(window->IsVisible());
}

TEST_F(SettingsWindowTest, ViewingLogsInvokesTheSystemOpenCallbackAndDisplaysFailures) {
    Page(LogsPage);
    ClickViewLog();
    EXPECT_EQ(viewedLogs, 1);
    EXPECT_TRUE(Error().empty());
    allowViewLog = false;
    ClickViewLog();
    EXPECT_EQ(viewedLogs, 2);
    EXPECT_EQ(Error(), "模拟打开失败");
    EXPECT_TRUE(window->IsVisible());
}
TEST_F(SettingsWindowTest, CreatingRenderingAndClosingSettingsPreserveTheHostImGuiContext) {
    auto* previous=ImGui::GetCurrentContext();
    auto* host=ImGui::CreateContext(); ImGui::SetCurrentContext(host);
    window->Close(); EXPECT_EQ(ImGui::GetCurrentContext(),host);
    EXPECT_TRUE(window->Show()); EXPECT_EQ(ImGui::GetCurrentContext(),host);
    window->SetTheme(Theme::Light); EXPECT_EQ(ImGui::GetCurrentContext(),host);
    window->RenderFrame(); EXPECT_EQ(ImGui::GetCurrentContext(),host);
    window->Close(); EXPECT_EQ(ImGui::GetCurrentContext(),host);
    ImGui::DestroyContext(host); ImGui::SetCurrentContext(previous);
}

TEST_F(SettingsWindowTest, NativeCloseButtonClosesOnlySettingsAndAllowsReopening) {
    for (int cycle = 0; cycle < NativeCloseCycles; ++cycle) {
        SCOPED_TRACE(cycle);
        // 与应用主循环一样逐帧排空原生临时对象，覆盖关闭后再次创建的生命周期。
        PlatformAutoreleasePool pool;
        ASSERT_TRUE(window->Show()) << SDL_GetError();
        const auto windowId = SDL_GetWindowID(GetSDLWindow());
        NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(GetSDLWindow()),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        ASSERT_NE(native, nil);
        NSButton* close = [native standardWindowButton:NSWindowCloseButton];
        ASSERT_NE(close, nil);
        [close performClick:nil];
        bool requestedClose = false, requestedQuit = false;
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            requestedClose |= event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == windowId;
            requestedQuit |= event.type == SDL_EVENT_QUIT;
            window->ProcessEvent(event);
        }
        EXPECT_TRUE(requestedClose);
        EXPECT_FALSE(requestedQuit) << "关闭最后一个可见的设置窗口不能触发应用退出";
        window->RenderFrame();
        EXPECT_FALSE(window->IsVisible());
        EXPECT_EQ(GetSDLWindow(), nullptr);
    }
    EXPECT_TRUE(window->Show()) << SDL_GetError();
    EXPECT_TRUE(window->IsVisible());
}
} // namespace shotlet
