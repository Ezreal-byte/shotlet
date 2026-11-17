#include "utils/UiGpuUtils.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

struct SDL_Window {};
struct SDL_GPUDevice {};
struct SDL_GPUCommandBuffer {};
struct SDL_GPUTexture {};
struct SDL_GPURenderPass {};

namespace {
constexpr int WindowWidthPixels = 1280;
constexpr int WindowHeightPixels = 820;
constexpr int RestoreCycleCount = 32;

enum class Step { AcquireCommand, AcquireSwapchain, Prepare, BeginPass, Draw, EndPass, Submit, Cancel };
enum class Failure { None, WindowSize, Command, Swapchain, NoSwapchain, RenderPass, Submit };
struct FrameState {
    SDL_WindowFlags flags = 0;
    int pixelWidth = WindowWidthPixels, pixelHeight = WindowHeightPixels;
    Failure failure = Failure::None;
    bool swapchainAcquired = false;
    bool commandActive = false;
    std::vector<Step> steps;
} state;
SDL_Window window;
SDL_GPUDevice device;
SDL_GPUCommandBuffer command;
SDL_GPUTexture texture;
SDL_GPURenderPass pass;

const std::vector<Step> SuccessfulFrame{Step::AcquireCommand, Step::AcquireSwapchain, Step::Prepare,
    Step::BeginPass, Step::Draw, Step::EndPass, Step::Submit};

class ImGuiGpuFrameTest : public testing::Test {
protected:
    void SetUp() override {
        state = {};
        drawData.DisplaySize = ImVec2(WindowWidthPixels, WindowHeightPixels);
        drawData.FramebufferScale = ImVec2(1, 1);
        drawData.TotalVtxCount = 3;
    }
    void TearDown() override { EXPECT_FALSE(state.commandActive); }
    bool Submit() { return shotlet::ImGuiUtils::SubmitGpuDrawData(&device, &window, &drawData); }
    ImDrawData drawData;
};
} // namespace

// 仅替换 SDL/GPU 边界：验证真实公共提交逻辑，不创建桌面窗口或依赖特定显卡。
extern "C" {
SDL_WindowFlags SDLCALL SDL_GetWindowFlags(SDL_Window* source) {
    EXPECT_EQ(source, &window);
    return state.flags;
}
bool SDLCALL SDL_GetWindowSizeInPixels(SDL_Window* source, int* width, int* height) {
    EXPECT_EQ(source, &window);
    *width = state.pixelWidth;
    *height = state.pixelHeight;
    return state.failure != Failure::WindowSize;
}
SDL_GPUCommandBuffer* SDLCALL SDL_AcquireGPUCommandBuffer(SDL_GPUDevice* source) {
    EXPECT_EQ(source, &device);
    EXPECT_FALSE(state.commandActive);
    state.steps.push_back(Step::AcquireCommand);
    if (state.failure == Failure::Command) return nullptr;
    state.commandActive = true;
    return &command;
}
bool SDLCALL SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer* buffer, SDL_Window* source,
    SDL_GPUTexture** swapchain, Uint32*, Uint32*) {
    EXPECT_EQ(buffer, &command);
    EXPECT_EQ(source, &window);
    EXPECT_TRUE(state.commandActive);
    state.steps.push_back(Step::AcquireSwapchain);
    *swapchain = nullptr;
    if (state.failure == Failure::Swapchain) return false;
    if (state.failure == Failure::NoSwapchain) {
        // 模拟帧开始时尚未最小化，但等待交换链期间窗口变为最小化。
        state.flags |= SDL_WINDOW_MINIMIZED;
        return true;
    }
    state.swapchainAcquired = true;
    *swapchain = &texture;
    return true;
}
SDL_GPURenderPass* SDLCALL SDL_BeginGPURenderPass(SDL_GPUCommandBuffer* buffer,
    const SDL_GPUColorTargetInfo* targets, Uint32 count, const SDL_GPUDepthStencilTargetInfo*) {
    EXPECT_EQ(buffer, &command);
    EXPECT_TRUE(state.swapchainAcquired);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(targets->texture, &texture);
    state.steps.push_back(Step::BeginPass);
    return state.failure == Failure::RenderPass ? nullptr : &pass;
}
void SDLCALL SDL_EndGPURenderPass(SDL_GPURenderPass* source) {
    EXPECT_EQ(source, &pass);
    state.steps.push_back(Step::EndPass);
}
bool SDLCALL SDL_SubmitGPUCommandBuffer(SDL_GPUCommandBuffer* buffer) {
    EXPECT_EQ(buffer, &command);
    EXPECT_TRUE(state.commandActive);
    EXPECT_TRUE(state.swapchainAcquired);
    state.commandActive = state.swapchainAcquired = false;
    state.steps.push_back(Step::Submit);
    return state.failure != Failure::Submit;
}
bool SDLCALL SDL_CancelGPUCommandBuffer(SDL_GPUCommandBuffer* buffer) {
    EXPECT_EQ(buffer, &command);
    EXPECT_TRUE(state.commandActive);
    EXPECT_FALSE(state.swapchainAcquired);
    state.commandActive = false;
    state.steps.push_back(Step::Cancel);
    return true;
}
} // extern "C"

void ImGui_ImplSDLGPU3_PrepareDrawData(ImDrawData*, SDL_GPUCommandBuffer* buffer) {
    EXPECT_EQ(buffer, &command);
    EXPECT_TRUE(state.swapchainAcquired);
    state.steps.push_back(Step::Prepare);
}
void ImGui_ImplSDLGPU3_RenderDrawData(ImDrawData*, SDL_GPUCommandBuffer* buffer,
    SDL_GPURenderPass* source, SDL_GPUGraphicsPipeline*) {
    EXPECT_EQ(buffer, &command);
    EXPECT_EQ(source, &pass);
    state.steps.push_back(Step::Draw);
}

TEST_F(ImGuiGpuFrameTest, RepeatedMinimizeAndRestoreSkipsGpuWorkAndResumesRendering) {
    for (int cycle = 0; cycle < RestoreCycleCount; ++cycle) {
        SCOPED_TRACE(cycle);
        state.flags = SDL_WINDOW_MINIMIZED;
        state.steps.clear();
        EXPECT_FALSE(Submit());
        EXPECT_TRUE(state.steps.empty());
        // Windows 最小化时保留原窗口尺寸，也没有 HIDDEN 标志。
        EXPECT_EQ(state.pixelWidth, WindowWidthPixels);
        state.flags = 0;
        EXPECT_TRUE(Submit());
        EXPECT_EQ(state.steps, SuccessfulFrame);
    }
}

TEST_F(ImGuiGpuFrameTest, HiddenAndInvalidDrawableSizesDoNotAcquireCommands) {
    EXPECT_FALSE(shotlet::ImGuiUtils::SubmitGpuDrawData(&device, nullptr, &drawData));
    state.flags = SDL_WINDOW_HIDDEN;
    EXPECT_FALSE(Submit());
    state.flags = 0;
    for (const auto size : std::array{SDL_Point{0, WindowHeightPixels}, SDL_Point{WindowWidthPixels, 0}}) {
        state.pixelWidth = size.x;
        state.pixelHeight = size.y;
        EXPECT_FALSE(Submit());
    }
    state.pixelWidth = WindowWidthPixels;
    state.pixelHeight = WindowHeightPixels;
    state.failure = Failure::WindowSize;
    EXPECT_FALSE(Submit());
    EXPECT_TRUE(state.steps.empty());
}

TEST_F(ImGuiGpuFrameTest, EmptyAndZeroFramebufferDrawDataDoesNotAcquireCommands) {
    EXPECT_FALSE(shotlet::ImGuiUtils::SubmitGpuDrawData(&device, &window, nullptr));
    drawData.TotalVtxCount = 0;
    EXPECT_FALSE(Submit());
    drawData.TotalVtxCount = 3;
    drawData.DisplaySize = ImVec2();
    EXPECT_FALSE(Submit());
    drawData.DisplaySize = ImVec2(WindowWidthPixels, WindowHeightPixels);
    drawData.FramebufferScale = ImVec2();
    EXPECT_FALSE(Submit());
    EXPECT_TRUE(state.steps.empty());
}

TEST_F(ImGuiGpuFrameTest, SwapchainUnavailableCancelsBeforePreparingAndRecoversOnRestore) {
    for (auto failure : {Failure::Swapchain, Failure::NoSwapchain}) {
        SCOPED_TRACE(int(failure));
        state.failure = failure;
        state.steps.clear();
        EXPECT_FALSE(Submit());
        EXPECT_FALSE(state.commandActive);
        EXPECT_EQ(state.steps, (std::vector{Step::AcquireCommand, Step::AcquireSwapchain, Step::Cancel}));
        state.failure = Failure::None;
        state.flags = 0;
        state.steps.clear();
        EXPECT_TRUE(Submit());
        EXPECT_EQ(state.steps, SuccessfulFrame);
    }
}

TEST_F(ImGuiGpuFrameTest, CommandAllocationFailureDoesNotSubmitOrCancelANullCommand) {
    state.failure = Failure::Command;
    EXPECT_FALSE(Submit());
    EXPECT_EQ(state.steps, (std::vector{Step::AcquireCommand}));
}

TEST_F(ImGuiGpuFrameTest, RenderPassFailureStillSubmitsTheAcquiredSwapchain) {
    state.failure = Failure::RenderPass;
    EXPECT_FALSE(Submit());
    EXPECT_EQ(state.steps, (std::vector{Step::AcquireCommand, Step::AcquireSwapchain, Step::Prepare,
        Step::BeginPass, Step::Submit}));
}

TEST_F(ImGuiGpuFrameTest, FailedSubmissionDoesNotReuseTheConsumedCommand) {
    state.failure = Failure::Submit;
    EXPECT_FALSE(Submit());
    EXPECT_EQ(state.steps, SuccessfulFrame);
    EXPECT_FALSE(state.commandActive);
    state.failure = Failure::None;
    state.steps.clear();
    EXPECT_TRUE(Submit());
    EXPECT_EQ(state.steps, SuccessfulFrame);
}
