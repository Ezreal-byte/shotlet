#include <gtest/gtest.h>
#include <SDL3/SDL.h>
#include "scrollcapture/ScrollCaptureStitcher.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace shotlet {
namespace {

class ScrollCaptureRealImageTest : public testing::Test {
protected:
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    void SetUp() override {
        // 直接解码用户提供的 PNG；文件缺失或损坏必须失败，不能悄悄跳过回归。
        const auto* path = SHOTLET_SOURCE_DIR "/tests/testdata/pure_color.png";
        using SurfacePtr = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
        const SurfacePtr decoded(SDL_LoadPNG(path), SDL_DestroySurface);
        ASSERT_NE(decoded, nullptr) << path << ": " << SDL_GetError();
        // RGBA32 在各端序平台都按 RGBA 字节排列；逐行复制以尊重 SDL 的 pitch。
        const SurfacePtr rgba(SDL_ConvertSurface(decoded.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        ASSERT_NE(rgba, nullptr) << SDL_GetError();
        width = rgba->w;
        height = rgba->h;
        ASSERT_EQ(width, 1380);
        ASSERT_EQ(height, 2652);
        pixels.resize(size_t(width) * height * 4);
        ASSERT_TRUE(SDL_LockSurface(rgba.get())) << SDL_GetError();
        for (int y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + size_t(y) * width * 4,
                        static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * rgba->pitch,
                        size_t(width) * 4);
        }
        SDL_UnlockSurface(rgba.get());
    }

    PixelView FrameAt(int y, int viewportHeight) const {
        return {pixels.data() + size_t(y) * width * 4, width, viewportHeight, width * 4};
    }

    void VerifyFullImage(int direction, bool biasedPrior) {
        constexpr int viewportHeight = 900;
        std::vector<int> positions = {0, 400, 800, 1200, 1600, height - viewportHeight};
        if (direction < 0) std::reverse(positions.begin(), positions.end());
        ScrollCaptureStitcher stitcher;
        ASSERT_TRUE(stitcher.StartSession(FrameAt(positions.front(), viewportHeight)));
        for (size_t i = 1; i < positions.size(); ++i) {
            const int dy = positions[i] - positions[i - 1];
            SCOPED_TRACE(testing::Message() << "源图 y=" << positions[i] << ", 真值 dy=" << dy);
            const auto result = stitcher.ProcessFrame(
                {FrameAt(positions[i], viewportHeight), biasedPrior ? dy / 2 : 0,
                 biasedPrior, true, direction});
            ASSERT_EQ(result.status, StitchStatus::Ok) << stitcher.GetLastErrorMessage();
            EXPECT_EQ(result.dy, dy);
            EXPECT_EQ(result.appendedTopRows, direction < 0 ? -dy : 0);
            EXPECT_EQ(result.appendedBottomRows, direction > 0 ? dy : 0);
            EXPECT_EQ(stitcher.FixedHeaderHeight(), 0);
            EXPECT_EQ(stitcher.FixedFooterHeight(), 0);
        }
        std::vector<uint8_t> output;
        int outputWidth = 0, outputHeight = 0;
        ASSERT_TRUE(stitcher.ExportFinalImage(output, outputWidth, outputHeight));
        ASSERT_EQ(outputWidth, width);
        ASSERT_EQ(outputHeight, height);
        ASSERT_EQ(output.size(), pixels.size());
        EXPECT_EQ(std::memcmp(output.data(), pixels.data(), pixels.size()), 0)
            << "导出应保留全部真实留白和文字，不漏行、不重复";
    }
};

TEST_F(ScrollCaptureRealImageTest, PureColorScreenshotScrollsDownWithoutPrior) {
    // 为什么：真实终端截图的大面积纯色、细文字和稀疏时间戳应提供准确位移证据。
    VerifyFullImage(1, false);
}

TEST_F(ScrollCaptureRealImageTest, PureColorScreenshotScrollsUpWithoutPrior) {
    // 为什么：从底部向上追加同样不能误裁留白或改变原图文字。
    VerifyFullImage(-1, false);
}

TEST_F(ScrollCaptureRealImageTest, PureColorScreenshotIgnoresBiasedPrior) {
    // 为什么：先验偏小一半时，真实图片证据仍须确定实际位移和长图高度。
    VerifyFullImage(1, true);
}

// 覆盖式叠加层（悬浮滚动条滑块、悬浮按钮）：不随页面滚动的灰色矩形。
void PaintOverlay(std::vector<uint8_t>& frame, int width, int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; ++y) for (int x = x0; x < x0 + w; ++x) {
        auto* p = frame.data() + (size_t(y) * width + x) * 4;
        p[0] = p[1] = p[2] = 140;
    }
}

class ScrollCaptureRealImageOverlayTest : public ScrollCaptureRealImageTest {
protected:
    static constexpr int kViewport = 900;
    std::vector<uint8_t> Copy(int y) const {
        const auto view = FrameAt(y, kViewport);
        return {view.data, view.data + size_t(width) * kViewport * 4};
    }
    StitchResult Process(ScrollCaptureStitcher& stitcher, const std::vector<uint8_t>& first,
                         const std::vector<uint8_t>& next) const {
        EXPECT_TRUE(stitcher.StartSession({first.data(), width, kViewport, width * 4}));
        // 与 macOS 连续模式一致：无先验、运动帧严格阈值、位移上限为内容带一半。
        return stitcher.ProcessFrame({{next.data(), width, kViewport, width * 4}, 0, false, false, 1,
                                      kViewport / 2});
    }
};

TEST_F(ScrollCaptureRealImageOverlayTest, OverlayScrollbarAndFloatingButtonDoNotBlockStitch) {
    // 为什么：macOS 滚动时悬浮滚动条淡入并随页面移动，网页常有悬浮按钮。它们只占窄竖条，
    // 却会让稀疏终端行整行验证失败（实测置信度 0.42–0.64 < 0.75），提示置信度不足并锁住滚动。
    for (const int variant : {0, 1, 2}) {
        SCOPED_TRACE(testing::Message() << "variant=" << variant);
        auto first = Copy(800), next = Copy(1000);
        if (variant == 0) PaintOverlay(next, width, width - 22, 340, 14, 305);        // 滚动条淡入
        if (variant == 1) {                                                           // 滑块随滚动下移
            PaintOverlay(first, width, width - 22, 272, 14, 305);
            PaintOverlay(next, width, width - 22, 340, 14, 305);
        }
        if (variant == 2) {                                                           // 正文中的悬浮按钮
            PaintOverlay(first, width, 1100, 600, 120, 120);
            PaintOverlay(next, width, 1100, 600, 120, 120);
        }
        ScrollCaptureStitcher stitcher;
        const auto result = Process(stitcher, first, next);
        ASSERT_EQ(result.status, StitchStatus::Ok) << stitcher.GetLastErrorMessage();
        EXPECT_EQ(result.dy, 200);
        EXPECT_TRUE(result.usedColumnExclusion);
        EXPECT_EQ(result.appendedBottomRows, 200);
    }
}

TEST_F(ScrollCaptureRealImageOverlayTest, StaticFrameWithFadedScrollbarIsNoChange) {
    // 为什么：失败后输入暂停、页面静止而滚动条淡出。若判为失配，基准帧永远对不上，
    // 采集卡死在置信度不足；只有窄竖条变化且其余结构一致时应判为无变化。
    auto first = Copy(1000);
    const auto next = Copy(1000);
    PaintOverlay(first, width, width - 22, 340, 14, 305);
    ScrollCaptureStitcher stitcher;
    const auto result = Process(stitcher, first, next);
    EXPECT_EQ(result.status, StitchStatus::NoChange) << stitcher.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 0);
    EXPECT_EQ(stitcher.GetStore()->Height(), 0);
}

TEST_F(ScrollCaptureRealImageOverlayTest, WideMismatchIsNotExcludedToForceStitch) {
    // 为什么：竖条排除只用于窄叠加层。左半幅换成页面远处内容（大面积重绘）时仍必须失败，
    // 不能排除失配区后靠剩余列拼出错误长图。
    const auto first = Copy(800);
    auto next = Copy(1000);
    const auto other = Copy(1700);
    for (int y = 0; y < kViewport; ++y) {
        std::memcpy(next.data() + size_t(y) * width * 4, other.data() + size_t(y) * width * 4,
                    size_t(width / 2) * 4);
    }
    ScrollCaptureStitcher stitcher;
    const auto result = Process(stitcher, first, next);
    EXPECT_NE(result.status, StitchStatus::Ok);
    EXPECT_NE(result.status, StitchStatus::NoChange);
    EXPECT_EQ(stitcher.GetStore()->Height(), 0);
}

TEST_F(ScrollCaptureRealImageTest, PureBackgroundDoesNotAppendFromPrior) {
    // 为什么：选取原图顶部连续纯色区域，不能仅凭滚轮先验生成额外内容。
    constexpr int viewportHeight = 300;
    const auto first = FrameAt(240, viewportHeight);
    const auto next = FrameAt(260, viewportHeight);
    ASSERT_EQ(std::memcmp(first.data, next.data, size_t(width) * viewportHeight * 4), 0);
    ScrollCaptureStitcher stitcher;
    ASSERT_TRUE(stitcher.StartSession(first));
    const auto result = stitcher.ProcessFrame({next, 20, true, true, 1});
    EXPECT_EQ(result.status, StitchStatus::NoChange);
    EXPECT_EQ(result.dy, 0);
    EXPECT_EQ(result.appendedBottomRows, 0);
    EXPECT_EQ(stitcher.GetStore()->Height(), 0);
    std::vector<uint8_t> output;
    int outputWidth = 0, outputHeight = 0;
    ASSERT_TRUE(stitcher.ExportFinalImage(output, outputWidth, outputHeight));
    EXPECT_EQ(outputWidth, width);
    EXPECT_EQ(outputHeight, viewportHeight);
    ASSERT_EQ(output.size(), size_t(width) * viewportHeight * 4);
    EXPECT_EQ(std::memcmp(output.data(), first.data, output.size()), 0);
}

}  // namespace
}  // namespace shotlet
