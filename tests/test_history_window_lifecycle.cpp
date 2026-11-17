#include "ui/HistoryWindow.h"
#include "platform/ApplicationPlatform.h"
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <chrono>
#include <memory>

namespace shotlet {
namespace {
constexpr int RestoreCycleCount = 6;
constexpr Uint32 WindowEventPollIntervalMs = 10;
constexpr auto WindowTransitionTimeout = std::chrono::seconds(2);
}

class HistoryWindowLifecycleTest : public testing::Test {
protected:
    void SetUp() override {
        m_previousContext = ImGui::GetCurrentContext();
        InitializeApplicationPlatform();
        ApplyApplicationPlatformSDLHints();
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
        m_device = CreateApplicationGpuDevice(false);
        ASSERT_NE(m_device, nullptr) << SDL_GetError();
        m_root = std::filesystem::temp_directory_path() / ("shotlet-history-lifecycle-" + NewHistoryId());
        m_service = std::make_unique<HistoryService>(m_root);
        m_hostContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_hostContext);
        m_history = std::make_unique<HistoryWindow>(m_device, *m_service, Language::ChineseSimplified, Theme::Light);
        ASSERT_TRUE(m_history->Show()) << SDL_GetError();
        EXPECT_EQ(ImGui::GetCurrentContext(), m_hostContext);
        m_history->RenderFrame();
        PumpEvents();
    }
    void TearDown() override {
        m_history.reset();
        if (m_hostContext) ImGui::DestroyContext(m_hostContext);
        ImGui::SetCurrentContext(m_previousContext);
        m_service.reset();
        DestroyApplicationGpuDevice(m_device, false);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (!m_root.empty()) {
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
        }
    }
    void PumpEvents() {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) m_history->ProcessEvent(event);
    }
    bool SetMinimized(bool minimized) {
        auto* window = m_history->GetSDLWindow();
        if (!(minimized ? SDL_MinimizeWindow(window) : SDL_RestoreWindow(window))) return false;
        const auto deadline = std::chrono::steady_clock::now() + WindowTransitionTimeout;
        // 原生窗口状态变化可能异步完成，等待实际标志，而不是仅向事件队列塞入伪造通知。
        do {
            PumpEvents();
            if (bool(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) == minimized) return true;
            SDL_Delay(WindowEventPollIntervalMs);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    int FrameCount() const { return m_history->GetImGuiContext()->FrameCount; }
    SDL_GPUDevice* m_device = nullptr;
    ImGuiContext* m_previousContext = nullptr;
    ImGuiContext* m_hostContext = nullptr;
    std::filesystem::path m_root;
    std::unique_ptr<HistoryService> m_service;
    std::unique_ptr<HistoryWindow> m_history;
};

TEST_F(HistoryWindowLifecycleTest, RepeatedMinimizeAndRestorePausesFramesAndPreservesTheWindow) {
    auto* window = m_history->GetSDLWindow();
    auto* context = m_history->GetImGuiContext();
    int expectedPixelWidth = 0, expectedPixelHeight = 0;
    ASSERT_TRUE(SDL_GetWindowSizeInPixels(window, &expectedPixelWidth, &expectedPixelHeight));
    for (int cycle = 0; cycle < RestoreCycleCount; ++cycle) {
        SCOPED_TRACE(cycle);
        ASSERT_TRUE(SetMinimized(true)) << SDL_GetError();
        EXPECT_TRUE(m_history->IsVisible());
        const int minimizedFrameCount = FrameCount();
        m_history->RenderFrame();
        m_history->RenderFrame();
        EXPECT_EQ(FrameCount(), minimizedFrameCount);
        EXPECT_EQ(ImGui::GetCurrentContext(), m_hostContext);

        ASSERT_TRUE(SetMinimized(false)) << SDL_GetError();
        m_history->RenderFrame();
        EXPECT_EQ(FrameCount(), minimizedFrameCount + 1);
        EXPECT_EQ(m_history->GetSDLWindow(), window);
        EXPECT_EQ(m_history->GetImGuiContext(), context);
        EXPECT_EQ(ImGui::GetCurrentContext(), m_hostContext);
        EXPECT_EQ(context->ErrorCountCurrentFrame, 0);
        int pixelWidth = 0, pixelHeight = 0;
        ASSERT_TRUE(SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight));
        EXPECT_EQ(pixelWidth, expectedPixelWidth);
        EXPECT_EQ(pixelHeight, expectedPixelHeight);
    }
    ASSERT_TRUE(SDL_WaitForGPUIdle(m_device)) << SDL_GetError();
}

TEST_F(HistoryWindowLifecycleTest, CloseRequestWhileMinimizedStillReleasesWindowAndContext) {
    ASSERT_TRUE(SetMinimized(true)) << SDL_GetError();
    SDL_Event close{};
    close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    close.window.windowID = SDL_GetWindowID(m_history->GetSDLWindow());
    ASSERT_TRUE(m_history->ProcessEvent(close));
    m_history->RenderFrame();
    EXPECT_EQ(m_history->GetSDLWindow(), nullptr);
    EXPECT_EQ(m_history->GetImGuiContext(), nullptr);
    EXPECT_EQ(ImGui::GetCurrentContext(), m_hostContext);
}
} // namespace shotlet
