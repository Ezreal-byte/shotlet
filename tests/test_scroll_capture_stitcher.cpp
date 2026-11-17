// ScrollCaptureStitcher 单元测试
// 对应设计文档 16.1 的位移估计/偏移状态机/固定头尾/周期校验场景。
// 每个测试顶部的"为什么"注释说明该行为存在的业务理由（CLAUDE.md Rule 9）。
// 测试即"知道真值 dy"的临时 Controller：先验用真值（或刻意偏差），
// 断言 dy / 追加行数 / 偏移状态 / 最终导出与源图逐字节一致。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "scrollcapture/ScrollCaptureStitcher.h"
#include "scroll_capture_test_data.h"

namespace shotlet {
namespace {

constexpr int kW = 200;          // 帧宽
constexpr int kVH = 500;         // 视口（帧）高
constexpr int kRowBytes = kW * 4;

PixelView ViewOf(const std::vector<uint8_t>& v, int h) {
    return PixelView{v.data(), kW, h, kRowBytes};
}

FrameInput MakeFrame(const std::vector<uint8_t>& v) {
    return FrameInput{ViewOf(v, kVH), 0, false, true};
}

FrameInput MakeFrameWithPrior(const std::vector<uint8_t>& v, int expectedShift) {
    return FrameInput{ViewOf(v, kVH), expectedShift, true, true};
}

// 固定侧栏和正文均有密集结构，避免用纯色侧栏掩盖错误零位移。
std::vector<uint8_t> MakeSideFrame(int width, int height, int offset, int left, int right,
                                  int header = 0, int footer = 0, int seed = 19) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const bool fixed = x < left || x >= width - right || y < header || y >= height - footer;
        const auto value = scrolltest::Hash32(uint32_t(seed + x), uint32_t(y + (fixed ? 0 : offset)));
        auto* p = pixels.data() + (size_t(y) * width + x) * 4;
        p[0] = p[1] = p[2] = uint8_t(value);
        p[3] = 255;
    }
    return pixels;
}

TEST(ScrollCaptureStitcherTest, FixedSidesMatchForBothPlatformContractsAndKeepOutputWidth) {
    // 为什么：macOS 运动帧和 Windows 稳定帧都不能被占主导的固定侧栏误判成零位移。
    constexpr int width = 640, height = 480, shift = 24;
    for (bool continuous : {false, true}) for (int direction : {-1, 1}) {
        for (const auto [left, right] : {std::pair{96, 0}, {0, 320}, {240, 240}, {576, 0}}) {
            SCOPED_TRACE(testing::Message() << continuous << "," << direction << "," << left << "," << right);
            const int offset = direction > 0 ? 0 : 96;
            auto first = MakeSideFrame(width, height, offset, left, right);
            ScrollCaptureStitcher st;
            ASSERT_TRUE(st.StartSession({first.data(), width, height, width * 4}));
            for (int i = 1; i <= 3; ++i) {
                auto next = MakeSideFrame(width, height, offset + direction * i * shift, left, right);
                auto result = st.ProcessFrame({{next.data(), width, height, width * 4}, direction * shift,
                    !continuous, !continuous, direction, continuous ? height / 2 : 0});
                ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
                EXPECT_EQ(result.dy, direction * shift);
            }
            std::vector<uint8_t> output;
            int outWidth = 0, outHeight = 0;
            ASSERT_TRUE(st.ExportFinalImage(output, outWidth, outHeight));
            EXPECT_EQ(outWidth, width);
            ASSERT_EQ(outHeight, height + shift * 3);
            auto truth = MakeSideFrame(width, outHeight, offset + (direction < 0 ? -shift * 3 : 0), 0, 0);
            for (int y = 0; y < outHeight; ++y) {
                EXPECT_EQ(std::memcmp(output.data() + (size_t(y) * width + left) * 4,
                    truth.data() + (size_t(y) * width + left) * 4, size_t(width - left - right) * 4), 0);
            }
        }
    }
}

TEST(ScrollCaptureStitcherTest, SideRegionChangesAndFailedFrameDoNotPoisonAcceptedBaseline) {
    // 为什么：侧栏折叠或展开后必须重新选范围，失败帧不能污染缓存和已拼结果。
    constexpr int width = 640, height = 480;
    auto first = MakeSideFrame(width, height, 0, 320, 0);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession({first.data(), width, height, width * 4}));
    auto moved = MakeSideFrame(width, height, 24, 320, 0);
    ASSERT_EQ(st.ProcessFrame({{moved.data(), width, height, width * 4}, 0, false, false, 1, 240}).dy, 24);
    auto unrelated = MakeSideFrame(width, height, 48, 480, 0, 0, 0, 91);
    EXPECT_NE(st.ProcessFrame({{unrelated.data(), width, height, width * 4}, 0, false, false, 1, 240}).status, StitchStatus::Ok);
    EXPECT_EQ(st.GetStore()->Height(), 504);
    auto collapsed = MakeSideFrame(width, height, 48, 96, 0);
    auto result = st.ProcessFrame({{collapsed.data(), width, height, width * 4}, 0, false, false, 1, 240});
    // 布局变化本身允许暂停；恢复同布局时以最后成功帧为基准继续。
    if (result.status != StitchStatus::Ok) {
        auto recovered = MakeSideFrame(width, height, 48, 320, 0);
        result = st.ProcessFrame({{recovered.data(), width, height, width * 4}, 0, false, false, 1, 240});
    }
    ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 24);
    EXPECT_EQ(st.GetStore()->Height(), 528);
}

TEST(ScrollCaptureStitcherTest, FixedSidesAndBarsAreDetectedWithoutCroppingContent) {
    // 为什么：排除侧栏后，固定头尾仍须各保留一次，正文高度不能减少。
    constexpr int width = 640, height = 480, header = 40, footer = 24;
    auto first = MakeSideFrame(width, height, 0, 240, 160, header, footer);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession({first.data(), width, height, width * 4}));
    for (int offset : {24, 48, 72}) {
        auto next = MakeSideFrame(width, height, offset, 240, 160, header, footer);
        auto result = st.ProcessFrame({{next.data(), width, height, width * 4}, 0, false, false, 1, 200});
        ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
        EXPECT_EQ(result.dy, 24);
        EXPECT_EQ(st.FixedHeaderHeight(), header);
        EXPECT_EQ(st.FixedFooterHeight(), footer);
    }
    std::vector<uint8_t> output;
    int outWidth = 0, outHeight = 0;
    ASSERT_TRUE(st.ExportFinalImage(output, outWidth, outHeight));
    EXPECT_EQ(outWidth, width);
    EXPECT_EQ(outHeight, height + 72);
    EXPECT_EQ(std::memcmp(output.data(), first.data(), size_t(header) * width * 4), 0);
}

TEST(ScrollCaptureStitcherTest, RepeatedContentInsideFixedSidesRemainsAmbiguous) {
    // 为什么：多个锚点仍可能来自重复行，不能忽略竞争位移后错误追加。
    constexpr int width = 640, height = 480, left = 320;
    auto first = MakeSideFrame(width, height, 0, left, 0);
    auto repeated = scrolltest::MakePeriodicRows(width, 0, 1000, 80, 8);
    for (int y = 0; y < height; ++y) {
        std::memcpy(first.data() + (size_t(y) * width + left) * 4,
                    repeated.data() + (size_t(y) * width + left) * 4, (width - left) * 4);
    }
    auto next = first;
    for (int y = 0; y < height; ++y) {
        std::memcpy(next.data() + (size_t(y) * width + left) * 4,
                    repeated.data() + (size_t(y + 24) * width + left) * 4, (width - left) * 4);
    }
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession({first.data(), width, height, width * 4}));
    const auto result = st.ProcessFrame({{next.data(), width, height, width * 4}, 0, false, false, 1, 240});
    EXPECT_EQ(result.status, StitchStatus::Ambiguous);
    EXPECT_EQ(st.GetStore()->Height(), 0);
}

TEST(ScrollCaptureStitcherTest, MostlyRepeatedContentWithDistinctAnchorsStillRejectsCompetingShift) {
    // 为什么：锚点独特不代表整个重叠区唯一；大量重复正文仍可能支持另一可信位移。
    constexpr int width = 640, height = 480, pageHeight = 900;
    std::vector<uint8_t> page(size_t(width) * pageHeight * 4);
    for (int y = 0; y < pageHeight; ++y) for (int x = 0; x < width; ++x) {
        const bool unique = y >= 24 && (y - 24) % 60 == 0;
        auto* p = page.data() + (size_t(y) * width + x) * 4;
        p[0] = p[1] = p[2] = uint8_t(scrolltest::Hash32(x + (unique ? 91 : 19), unique ? y : y % 80));
        p[3] = 255;
    }
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession({page.data(), width, height, width * 4}));
    const auto result = st.ProcessFrame({{page.data() + size_t(24) * width * 4, width, height, width * 4},
                                         0, false, false, 1, 240});
    EXPECT_EQ(result.status, StitchStatus::Ambiguous);
    EXPECT_EQ(st.GetStore()->Height(), 0);
}

TEST(ScrollCaptureStitcherTest, ContinuousShiftLimitAlsoRestrictsFallbackAndPreservesBaseline) {
    const auto page = scrolltest::MakeTexturedLongImage(kW, 1200, 7);
    scrolltest::FrameSpec spec; spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(page, kW, 1200, spec);
    for (bool prior : {false, true}) {
        ScrollCaptureStitcher stitcher;
        ASSERT_TRUE(stitcher.StartSession(ViewOf(first, kVH)));
        spec.contentY = 270;
        const auto tooFar = scrolltest::CutFrame(page, kW, 1200, spec);
        const auto failed = stitcher.ProcessFrame({ViewOf(tooFar, kVH), 270, prior, false, 1, 250});
        EXPECT_NE(failed.status, StitchStatus::Ok);
        EXPECT_NE(failed.status, StitchStatus::NoChange);
        EXPECT_EQ(stitcher.GetStore()->Height(), 0);
        EXPECT_EQ(stitcher.ViewportY(), 0);
        spec.contentY = 180;
        const auto recovered = scrolltest::CutFrame(page, kW, 1200, spec);
        const auto result = stitcher.ProcessFrame({ViewOf(recovered, kVH), 0, false, false, 1, 250});
        ASSERT_EQ(result.status, StitchStatus::Ok);
        EXPECT_EQ(result.dy, 180);
        std::vector<uint8_t> output;
        int w = 0, h = 0;
        ASSERT_TRUE(stitcher.ExportFinalImage(output, w, h));
        EXPECT_EQ(h, 680);
        EXPECT_TRUE(std::equal(output.begin(), output.end(), page.begin()));
    }
}

TEST(ScrollCaptureStitcherTest, SuccessfulStartClearsPreviousError) {
    ScrollCaptureStitcher st;
    EXPECT_TRUE(st.GetLastErrorMessage().empty());
    EXPECT_EQ(st.GetLastErrorId(), TextId::Count);
    ASSERT_FALSE(st.StartSession({}));
    ASSERT_EQ(st.GetLastErrorId(), TextId::StitchInvalidFrame);
    ASSERT_FALSE(st.GetLastErrorMessage().empty());
    const auto first = scrolltest::MakeTexturedLongImage(kW, kVH, 7);
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    EXPECT_TRUE(st.GetLastErrorMessage().empty());
    EXPECT_EQ(st.GetLastErrorId(), TextId::Count);
}

TEST(ScrollCaptureStitcherTest, RecoveredFrameClearsPreviousError) {
    const auto source = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    // 为什么：原地恢复和成功追加都必须同步清除错误文字与错误 ID。
    for (const int shift : {0, 200}) {
        ScrollCaptureStitcher st;
        ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
        ASSERT_EQ(st.ProcessFrame({}).status, StitchStatus::MatchFailed);
        ASSERT_EQ(st.GetLastErrorId(), TextId::StitchInvalidFrame);
        spec.contentY = shift;
        const auto next = scrolltest::CutFrame(source, kW, 900, spec);
        ASSERT_EQ(st.ProcessFrame(MakeFrame(next)).status,
                  shift == 0 ? StitchStatus::NoChange : StitchStatus::Ok);
        EXPECT_TRUE(st.GetLastErrorMessage().empty());
        EXPECT_EQ(st.GetLastErrorId(), TextId::Count);
    }
}

TEST(ScrollCaptureStitcherTest, SuccessfulExportClearsPreviousError) {
    ScrollCaptureStitcher st;
    const auto first = scrolltest::MakeTexturedLongImage(kW, kVH, 7);
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    ASSERT_EQ(st.ProcessFrame({}).status, StitchStatus::MatchFailed);
    ASSERT_FALSE(st.GetLastErrorMessage().empty());
    std::vector<uint8_t> out;
    int width = 0, height = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, width, height));
    EXPECT_EQ(out, first);
    EXPECT_TRUE(st.GetLastErrorMessage().empty());
    EXPECT_EQ(st.GetLastErrorId(), TextId::Count);
}

std::vector<uint8_t> MakeSparsePage(int width, int height) {
    std::vector<uint8_t> image(size_t(width) * height * 4, 245);
    for (size_t i = 3; i < image.size(); i += 4) image[i] = 255;
    return image;
}

// 小块模拟留白中的文字或图标；不同种子表示不同内容，背景始终相同。
void PaintSparseContent(std::vector<uint8_t>& image, int width, int x0, int y0,
                        int patchWidth, int patchHeight, uint32_t seed) {
    for (int y = 0; y < patchHeight; ++y) {
        for (int x = 0; x < patchWidth; ++x) {
            auto* p = image.data() + (size_t(y0 + y) * width + x0 + x) * 4;
            p[0] = p[1] = p[2] = uint8_t(20 + scrolltest::Hash32(seed + y, x) % 140);
        }
    }
}

TEST(ScrollCaptureStitcherTest, SparseContentPreservesWhitespaceInBothDirections) {
    // 为什么：少于 60 行的独特内容仍能定位，首对上下留白不能被当作固定栏裁掉。
    auto source = MakeSparsePage(kW, 900);
    PaintSparseContent(source, kW, 40, 260, 24, 18, 19);
    for (const int direction : {-1, 1}) {
        scrolltest::FrameSpec spec;
        spec.viewportHeight = kVH;
        spec.contentY = direction > 0 ? 0 : 120;
        const auto first = scrolltest::CutFrame(source, kW, 900, spec);
        spec.contentY += direction * 120;
        const auto next = scrolltest::CutFrame(source, kW, 900, spec);
        ScrollCaptureStitcher st;
        ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
        const auto result = st.ProcessFrame({ViewOf(next, kVH), direction * 70, true, true, direction});
        ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
        EXPECT_EQ(result.dy, direction * 120);
        EXPECT_EQ(st.FixedHeaderHeight(), 0);
        EXPECT_EQ(st.FixedFooterHeight(), 0);
        std::vector<uint8_t> output;
        int width = 0, height = 0;
        ASSERT_TRUE(st.ExportFinalImage(output, width, height));
        ASSERT_EQ(height, 620);
        EXPECT_TRUE(std::equal(output.begin(), output.end(), source.begin()));
    }
}

TEST(ScrollCaptureStitcherTest, NarrowContentOnWideBackgroundHasUniqueShift) {
    // 为什么：有效文字只占宽度的很小部分时，背景不能让所有候选得到相同高分。
    constexpr int width = 1600;
    auto source = MakeSparsePage(width, 900);
    PaintSparseContent(source, width, 120, 230, 24, 96, 23);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, width, 900, spec);
    spec.contentY = 120;
    const auto next = scrolltest::CutFrame(source, width, 900, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession({first.data(), width, kVH, width * 4}));
    const auto result = st.ProcessFrame({{next.data(), width, kVH, width * 4}, 0, false, true, 1});
    ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 120);
    std::vector<uint8_t> output;
    int outWidth = 0, outHeight = 0;
    ASSERT_TRUE(st.ExportFinalImage(output, outWidth, outHeight));
    ASSERT_EQ(outHeight, 620);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), source.begin()));
}

TEST(ScrollCaptureStitcherTest, FullWidthColorBoundariesProvideVerticalEvidence) {
    // 为什么：整行纯色不代表没有信息，上下色块边界应参与位移验证。
    auto source = MakeSparsePage(kW, 900);
    for (int y = 230; y < 410; ++y) {
        for (int x = 0; x < kW; ++x) {
            auto* p = source.data() + (size_t(y) * kW + x) * 4;
            p[0] = p[1] = p[2] = y < 320 ? 140 : 40;
        }
    }
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    spec.contentY = 120;
    const auto next = scrolltest::CutFrame(source, kW, 900, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    const auto result = st.ProcessFrame(MakeFrame(next));
    ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 120);
    std::vector<uint8_t> output;
    int width = 0, height = 0;
    ASSERT_TRUE(st.ExportFinalImage(output, width, height));
    ASSERT_EQ(height, 620);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), source.begin()));
}

TEST(ScrollCaptureStitcherTest, BackgroundCannotValidateUnrelatedNarrowContent) {
    // 为什么：两帧背景相同但文字完全不同，不能靠 90% 背景像素通过验证并污染结果。
    constexpr int width = 1600;
    auto first = MakeSparsePage(width, kVH);
    auto next = MakeSparsePage(width, kVH);
    PaintSparseContent(first, width, 120, 230, 24, 96, 23);
    PaintSparseContent(next, width, 120, 110, 24, 96, 91);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession({first.data(), width, kVH, width * 4}));
    const auto result = st.ProcessFrame({{next.data(), width, kVH, width * 4}, 120, true, true, 1});
    EXPECT_EQ(result.status, StitchStatus::MatchFailed);
    EXPECT_EQ(st.GetStore()->Height(), 0);
    std::vector<uint8_t> output;
    int outWidth = 0, outHeight = 0;
    ASSERT_TRUE(st.ExportFinalImage(output, outWidth, outHeight));
    EXPECT_EQ(output, first);
}

TEST(ScrollCaptureStitcherTest, SparseRepeatedContentCannotBeResolvedByInjectionPrior) {
    // 为什么：弱先验不能决定重复稀疏内容的实际位移，否则会把留白长度静默拼错。
    auto source = MakeSparsePage(kW, 1200);
    for (int y = 30; y + 8 < 1200; y += 120) {
        PaintSparseContent(source, kW, 40, y, 24, 8, 19);
    }
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, kW, 1200, spec);
    spec.contentY = 60;
    const auto next = scrolltest::CutFrame(source, kW, 1200, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    const auto result = st.ProcessFrame({ViewOf(next, kVH), 180, true, true, 1});
    EXPECT_EQ(result.status, StitchStatus::Ambiguous);
    EXPECT_EQ(st.GetStore()->Height(), 0);
}

TEST(ScrollCaptureStitcherTest, FixedBarsDoNotHideSparseScrollingContent) {
    // 为什么：聊天窗口的固定栏可能比消息更有纹理，但不能支配位移分数或吞掉消息间留白。
    auto source = MakeSparsePage(kW, 900);
    PaintSparseContent(source, kW, 40, 260, 24, 18, 19);
    scrolltest::FrameSpec spec;
    spec.fixedHeader = 48;
    spec.fixedFooter = 32;
    spec.viewportHeight = kVH - spec.fixedHeader - spec.fixedFooter;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    for (const int y : {120, 180}) {
        const int shift = y == 120 ? 120 : 60;
        spec.contentY = y;
        const auto next = scrolltest::CutFrame(source, kW, 900, spec);
        const auto result = st.ProcessFrame({ViewOf(next, kVH), shift, true, true, 1});
        ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
        EXPECT_EQ(result.dy, shift);
        EXPECT_EQ(st.FixedHeaderHeight(), 48);
        EXPECT_EQ(st.FixedFooterHeight(), 32);
    }
    std::vector<uint8_t> output;
    int width = 0, height = 0;
    ASSERT_TRUE(st.ExportFinalImage(output, width, height));
    ASSERT_EQ(height, 680);
    auto truth = scrolltest::MakeFixedRegion(kW, 48, spec.headerSeed);
    truth.insert(truth.end(), source.begin(), source.begin() + size_t(600) * kRowBytes);
    const auto footer = scrolltest::MakeFixedRegion(kW, 32, spec.footerSeed);
    truth.insert(truth.end(), footer.begin(), footer.end());
    EXPECT_EQ(output, truth);
}

TEST(ScrollCaptureStitcherTest, SparseContentWithNoiseAndPaddedPitchStillMatches) {
    // 为什么：掩码不能把采集噪声当成背景特征，也不能假定输入像素是紧凑存储。
    auto source = MakeSparsePage(kW, 900);
    PaintSparseContent(source, kW, 40, 260, 24, 18, 19);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    spec.contentY = 120;
    spec.noiseSeed = 42;
    spec.noiseAmplitude = 6;
    const auto next = scrolltest::CutFrame(source, kW, 900, spec);
    constexpr int pitch = kRowBytes + 32;
    std::vector<uint8_t> padded(size_t(pitch) * kVH, 0);
    for (int y = 0; y < kVH; ++y) {
        std::memcpy(padded.data() + size_t(y) * pitch, next.data() + size_t(y) * kRowBytes, kRowBytes);
    }
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    const auto result = st.ProcessFrame({{padded.data(), kW, kVH, pitch}, 70, true, true, 1});
    ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 120);
    EXPECT_EQ(st.FixedHeaderHeight(), 0);
    EXPECT_EQ(st.FixedFooterHeight(), 0);
}

TEST(ScrollCaptureStitcherTest, UnchangedContentWithNoiseDoesNotTriggerBlankRecovery) {
    // 为什么：到边界后只有采集噪声时，应保留结果并报告无变化，不能按空白画面继续推进。
    auto source = MakeSparsePage(kW, 900);
    PaintSparseContent(source, kW, 40, 260, 24, 18, 19);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    spec.noiseSeed = 42;
    spec.noiseAmplitude = 6;
    const auto next = scrolltest::CutFrame(source, kW, 900, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    const auto result = st.ProcessFrame({ViewOf(next, kVH), 120, true, true, 1});
    EXPECT_EQ(result.status, StitchStatus::NoChange);
    EXPECT_EQ(result.dy, 0);
    EXPECT_EQ(st.GetStore()->Height(), 0);
}

TEST(ScrollCaptureStitcherTest, FeaturesOutsideOverlapCannotJustifyAppending) {
    // 为什么：整帧都有内容但真实重叠区没有共同结构时，不能把任意背景位置当成可靠匹配。
    auto source = MakeSparsePage(kW, 900);
    PaintSparseContent(source, kW, 40, 20, 24, 18, 19);
    PaintSparseContent(source, kW, 40, 620, 24, 18, 23);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    spec.contentY = 200;
    const auto next = scrolltest::CutFrame(source, kW, 900, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    const auto result = st.ProcessFrame({ViewOf(next, kVH), 200, true, true, 1});
    EXPECT_NE(result.status, StitchStatus::Ok);
    EXPECT_NE(result.status, StitchStatus::NoChange);
    EXPECT_EQ(st.GetStore()->Height(), 0);
}

TEST(ScrollCaptureStitcherTest, RightMarginDoesNotSupplyFixedRegionOrMatchEvidence) {
    // 为什么：被排除的滚动条既不能阻断固定栏确认，也不能单独提供拼接证据。
    const auto source = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = 420;
    spec.fixedHeader = 48;
    spec.fixedFooter = 32;
    const auto first = scrolltest::CutFrame(source, kW, 900, spec);
    spec.contentY = 120;
    auto next = scrolltest::CutFrame(source, kW, 900, spec);
    for (int y = 0; y < kVH; ++y) {
        auto* p = next.data() + size_t(y) * kRowBytes + (kW - 1) * 4;
        p[0] = p[1] = p[2] = 255;
    }
    StitcherConfig config;
    config.matchRightMargin = 1;
    ScrollCaptureStitcher st(config);
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    const auto result = st.ProcessFrame(MakeFrame(next));
    ASSERT_EQ(result.status, StitchStatus::Ok) << st.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 120);
    EXPECT_EQ(st.FixedHeaderHeight(), 48);
    EXPECT_EQ(st.FixedFooterHeight(), 32);
}

// 默认配置 + 测试维度裁剪（小上限/小间隔便于触发边界，语义不变）
StitcherConfig TestConfig() {
    return StitcherConfig{};
}

TEST(ScrollCaptureStitcherTest, FirstFrameHeldUntilFirstDisplacedPair) {
    // 为什么：固定头/尾高度在首个位移帧对出现前未知（设计 11.3），
    // 首帧提前提交会在后续头估计收缩时被迫删除已写行（append-only 语义被破坏）
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    const auto f1 = scrolltest::CutFrame(src, kW, 900, spec);
    ASSERT_TRUE(st.StartSession(ViewOf(f1, kVH)));
    EXPECT_EQ(st.GetStore()->Height(), 0);  // 首帧暂存，未提交

    spec.contentY = 200;
    const auto f2 = scrolltest::CutFrame(src, kW, 900, spec);
    const auto r = st.ProcessFrame(MakeFrame(f2));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, 200);
    EXPECT_EQ(st.GetStore()->Height(), 700);  // 首帧 500 + 追加 200，此刻起 append-only
}

TEST(ScrollCaptureStitcherTest, ScrollDownAppendsBottomRows) {
    // 为什么：向下滚动是主路径，追加行数与导出区间一致是长图正确性的底线
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    spec.contentY = 200;
    auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, 200);
    EXPECT_EQ(r.appendedBottomRows, 200);
    EXPECT_EQ(r.appendedTopRows, 0);
    EXPECT_EQ(st.ViewportY(), 200);

    spec.contentY = 400;
    r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.appendedBottomRows, 200);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(w, kW);
    EXPECT_EQ(h, 900);
    EXPECT_EQ(out.size(), src.size());
    EXPECT_EQ(std::memcmp(out.data(), src.data(), src.size()), 0);  // 逐字节 == 源图 [0,900)
}

TEST(ScrollCaptureStitcherTest, ScrollUpAppendsTopAndRebases) {
    // 为什么：向上滚动超出已存储顶部时需要前插 + 重定基（viewport 归零），
    // 若只前插不平移偏移状态，后续所有行号映射都会错
    const auto src = scrolltest::MakeTexturedLongImage(kW, 1000, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 500;  // 首帧取中段，向上滚动必然越过存储顶部
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 1000, spec), kVH)));

    spec.contentY = 300;
    const auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 1000, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, -200);
    EXPECT_EQ(r.appendedTopRows, 200);
    EXPECT_EQ(r.appendedBottomRows, 0);
    EXPECT_EQ(st.ViewportY(), 0);  // 重定基：内容带顶边即存储 0 行

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 700);  // 源图 [300,1000) 区间
    EXPECT_EQ(std::memcmp(out.data(), src.data() + static_cast<size_t>(300) * kRowBytes,
                          out.size()),
              0);
}

TEST(ScrollCaptureStitcherTest, OppositeDisplacementDoesNotChangeAcceptedImage) {
    const auto src = scrolltest::MakeTexturedLongImage(kW, 1000, 7);
    ScrollCaptureStitcher st(TestConfig());
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 200;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 1000, spec), kVH)));
    spec.contentY = 100;
    const auto upward = scrolltest::CutFrame(src, kW, 1000, spec);
    ASSERT_EQ(st.ProcessFrame({ViewOf(upward, kVH), -100, true, true, -1}).status,
              StitchStatus::Ok);
    const int height = st.GetStore()->Height();
    const int viewport = st.ViewportY();

    spec.contentY = 300;
    const auto reverse = scrolltest::CutFrame(src, kW, 1000, spec);
    const auto result = st.ProcessFrame({ViewOf(reverse, kVH), 200, true, true, -1});
    EXPECT_EQ(result.status, StitchStatus::MatchFailed);
    EXPECT_EQ(st.GetStore()->Height(), height);
    EXPECT_EQ(st.ViewportY(), viewport);
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 600);
    EXPECT_EQ(std::memcmp(out.data(), src.data() + size_t(100) * kRowBytes, out.size()), 0);
}

TEST(ScrollCaptureStitcherTest, ScrollBackIntoStoredRegionAppendsNothing) {
    // 为什么：滚回已存储区域若再次追加会造成内容重复（长图最常见的翻车模式）
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    spec.contentY = 200;
    ASSERT_EQ(st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec))).status,
              StitchStatus::Ok);
    spec.contentY = 400;
    ASSERT_EQ(st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec))).status,
              StitchStatus::Ok);
    EXPECT_EQ(st.GetStore()->Height(), 900);

    // 滚回中间与顶部：已存储，绝不追加
    spec.contentY = 200;
    auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.appendedBottomRows, 0);
    EXPECT_EQ(r.appendedTopRows, 0);

    spec.contentY = 0;
    r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.appendedBottomRows, 0);
    EXPECT_EQ(r.appendedTopRows, 0);
    EXPECT_EQ(st.GetStore()->Height(), 900);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 900);
    EXPECT_EQ(std::memcmp(out.data(), src.data(), src.size()), 0);
}

TEST(ScrollCaptureStitcherTest, ScrollBackThenExtendOtherEnd) {
    // 为什么：用户来回滚动后继续向下是真实交互模式，
    // 滚回帧（无追加）后再次向下必须无缝续接，不重不漏
    const auto src = scrolltest::MakeTexturedLongImage(kW, 1600, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    // viewport 真值序列：0 → 300 → 600 → 300 → 0 → 300 → 600 → 900
    const int ys[] = {0, 300, 600, 300, 0, 300, 600, 900};
    spec.contentY = ys[0];
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 1600, spec), kVH)));
    for (int i = 1; i < 8; ++i) {
        spec.contentY = ys[i];
        const auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 1600, spec)));
        ASSERT_EQ(r.status, StitchStatus::Ok) << "第 " << i << " 帧";
        // 滚回段（300/0/300/600 已存储）无追加；0 帧起 900 处再向下扩展 300 行
        const bool stored = (i >= 3 && i <= 6);
        EXPECT_EQ(r.appendedBottomRows, stored ? 0 : 300) << "第 " << i << " 帧";
    }
    EXPECT_EQ(st.GetStore()->Height(), 1400);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 1400);
    EXPECT_EQ(std::memcmp(out.data(), src.data(), out.size()), 0);  // == 源图 [0,1400)
}

TEST(ScrollCaptureStitcherTest, DelayedShiftOutsidePriorWindowUsesFullSearch) {
    constexpr int height = 1200;
    const auto src = scrolltest::MakeTexturedLongImage(kW, 2000, 7);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = height;
    spec.contentY = 400;
    auto first = scrolltest::CutFrame(src, kW, 2000, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, height)));
    spec.contentY -= 288;
    auto current = scrolltest::CutFrame(src, kW, 2000, spec);
    const auto result = st.ProcessFrame({ViewOf(current, height), -10, true, true});
    ASSERT_EQ(result.status, StitchStatus::Ok);
    EXPECT_TRUE(result.usedFallbackSearch);
    EXPECT_EQ(result.dy, -288);
    EXPECT_TRUE(st.GetLastErrorMessage().empty());
    EXPECT_EQ(st.GetLastErrorId(), TextId::Count);
    std::vector<uint8_t> output;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(output, w, h));
    EXPECT_EQ(h, height + 288);
    EXPECT_TRUE(std::equal(output.begin(), output.end(), src.begin() + 112 * kRowBytes));
}

TEST(ScrollCaptureStitcherTest, FullSearchStillRejectsInsufficientOverlap) {
    const auto src = scrolltest::MakeTexturedLongImage(kW, 1600, 7);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    auto first = scrolltest::CutFrame(src, kW, 1600, spec);
    ScrollCaptureStitcher st;
    ASSERT_TRUE(st.StartSession(ViewOf(first, kVH)));
    spec.contentY = 310;
    auto current = scrolltest::CutFrame(src, kW, 1600, spec);
    const auto result = st.ProcessFrame({ViewOf(current, kVH), 10, true, true});
    EXPECT_EQ(result.status, StitchStatus::MatchFailed);
    EXPECT_TRUE(result.usedFallbackSearch);
    EXPECT_EQ(st.ViewportY(), 0);
}

TEST(ScrollCaptureStitcherTest, LargeShiftFailsInsteadOfMisaligning) {
    // 为什么：重叠不足（真位移超出搜索范围）时宁可报 MatchFailed 进恢复流程，
    // 也不能接受低置信度候选把长图错位拼接（错位是静默数据损坏）
    const auto src = scrolltest::MakeTexturedLongImage(kW, 1600, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 1600, spec), kVH)));

    spec.contentY = 310;  // 真位移 310 > maxAbs(=带高 500 - minOverlap 200 = 300)
    const auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 1600, spec)));
    EXPECT_EQ(r.status, StitchStatus::MatchFailed);

    // 失败帧不污染存储：导出回退为首帧内容（保留结果语义）
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, kVH);
    EXPECT_EQ(std::memcmp(out.data(), src.data(), out.size()), 0);
}

TEST(ScrollCaptureStitcherTest, PeriodicContentAmbiguousThenPriorDisambiguates) {
    // 为什么：重复纹理（等距列表/表格）下多个周期候选等价，无先验时报歧义
    // 让 Controller 重试/换步长，而不是随机挑一个；有注入先验时按先验消歧
    const int kPeriod = 100;
    const int kSrcH = 1000;
    auto src = scrolltest::MakeTexturedLongImage(kW, kSrcH, 7);
    const auto periodic = scrolltest::MakePeriodicRows(kW, 0, kSrcH, kPeriod, 8);
    std::memcpy(src.data(), periodic.data(), periodic.size());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    const auto f1 = scrolltest::CutFrame(src, kW, kSrcH, spec);
    spec.contentY = kPeriod + 25;  // 多个周期候选吻合，但两帧并不完全相同
    const auto f2 = scrolltest::CutFrame(src, kW, kSrcH, spec);

    {  // 无先验：最优−次优差距不足 → Ambiguous
        ScrollCaptureStitcher st(TestConfig());
        ASSERT_TRUE(st.StartSession(ViewOf(f1, kVH)));
        const auto r = st.ProcessFrame(MakeFrame(f2));
        EXPECT_EQ(r.status, StitchStatus::Ambiguous);
    }
    {  // 有先验 125：唯一最近候选 → 消歧成功
        ScrollCaptureStitcher st(TestConfig());
        ASSERT_TRUE(st.StartSession(ViewOf(f1, kVH)));
        const auto r = st.ProcessFrame(MakeFrameWithPrior(f2, kPeriod + 25));
        ASSERT_EQ(r.status, StitchStatus::Ok);
        EXPECT_EQ(r.dy, kPeriod + 25);
        EXPECT_EQ(st.ViewportY(), kPeriod + 25);
    }
}

TEST(ScrollCaptureStitcherTest, UnchangedPeriodicImageDoesNotAppendUsingPrior) {
    // 到边界时重复纹理仍有高置信候选，不能照注入先验重复追加。
    const auto frame = scrolltest::MakePeriodicRows(kW, 0, kVH, 25, 8);
    for (const int direction : {-1, 1}) {
        ScrollCaptureStitcher st;
        ASSERT_TRUE(st.StartSession(ViewOf(frame, kVH)));
        const auto result = st.ProcessFrame({ViewOf(frame, kVH), direction * 125,
                                              true, true, direction});
        EXPECT_EQ(result.status, StitchStatus::NoChange);
        EXPECT_EQ(result.dy, 0);
        EXPECT_EQ(st.GetStore()->Height(), 0);
    }
}

TEST(ScrollCaptureStitcherTest, BlankFrameYieldsInsufficientInfo) {
    // 为什么：大面积空白（空白页面/加载中）的匹配是任意的，
    // 必须报 InsufficientInfo 让 Controller 等下一帧，而不是输出误导性 dy
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    // 纯色帧：全部行空白 → 有效行 0 < minEffectiveRows
    std::vector<uint8_t> blank(static_cast<size_t>(kVH) * kRowBytes, 0x30);
    for (size_t i = 3; i < blank.size(); i += 4) blank[i] = 0xFF;
    const auto r = st.ProcessFrame(MakeFrame(blank));
    EXPECT_EQ(r.status, StitchStatus::InsufficientInfo);
}

TEST(ScrollCaptureStitcherTest, FixedHeaderFooterDetectedAndComposedOnce) {
    // 为什么：固定头/尾（导航栏/状态栏）若当作滚动内容会在长图中反复出现；
    // 检测后必须从内容带排除，且导出时头尾各只出现一次（设计 11.3）
    constexpr int kHeader = 48;
    constexpr int kFooter = 32;
    constexpr int kContentVH = kVH - kHeader - kFooter;  // 420，内容带高
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    const auto header = scrolltest::MakeFixedRegion(kW, kHeader, 0x1234u);
    const auto footer = scrolltest::MakeFixedRegion(kW, kFooter, 0x5678u);

    ScrollCaptureStitcher st(TestConfig());
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kContentVH;
    spec.fixedHeader = kHeader;
    spec.fixedFooter = kFooter;

    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    spec.contentY = 200;
    const auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, 200);
    EXPECT_EQ(st.FixedHeaderHeight(), kHeader);
    EXPECT_EQ(st.FixedFooterHeight(), kFooter);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, kHeader + 620 + kFooter);  // 头 + 内容 src[0,620) + 尾
    // 期望真值 = 头 + 源区间 + 尾，逐字节比对
    std::vector<uint8_t> truth;
    truth.insert(truth.end(), header.begin(), header.end());
    truth.insert(truth.end(), src.begin(), src.begin() + static_cast<long>(620) * kRowBytes);
    truth.insert(truth.end(), footer.begin(), footer.end());
    EXPECT_EQ(out, truth);
}

TEST(ScrollCaptureStitcherTest, NoiseToleratedStillExactShift) {
    // 为什么：采集噪声（有损压缩/抖动）是常态，容差内噪声必须不影响位移估计；
    // 追加进存储的带噪像素与源图差异不超过噪声幅度
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    constexpr int kAmp = 6;  // < 亮度容差 16
    spec.noiseSeed = 99;
    spec.noiseAmplitude = kAmp;
    spec.contentY = 200;
    auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, 200);  // 噪声不干扰位移估计
    spec.noiseSeed = 100;
    spec.contentY = 400;
    r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, 200);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    ASSERT_EQ(out.size(), src.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] != src[i]) {
            // 首帧段无噪声必须精确；追加段允许 ≤ 噪声幅度
            ASSERT_GE(i, static_cast<size_t>(kVH) * kRowBytes);
            ASSERT_LE(out[i] > src[i] ? out[i] - src[i] : src[i] - out[i], kAmp)
                << "偏差异于噪声幅度 @ " << i;
        }
    }
}

TEST(ScrollCaptureStitcherTest, DriftCorrectedByPeriodicVerify) {
    // 为什么：逐帧 dy 累加会累积 ±1/±2 级误差（亚像素滚动/周期内容错选），
    // 滚回已存储区域时用存储内容作绝对基准校正（设计 11.1 周期校验）
    // 构造：源图 [0,299) 强纹理 + [299,500) 周期 3 + [500,1000) 强纹理。
    // 帧 2 滚入周期段（先验 299 正确）；帧 3 滚回时周期段歧义 + 偏差先验
    // (-296 vs 真值 -299) → 消歧选中 -296 → viewport 偏 +3 → 滚回帧触发
    // 周期校验，强纹理段判别出 d=-3 并修正
    auto src = scrolltest::MakeTexturedLongImage(kW, 1000, 7);
    const auto periodic = scrolltest::MakePeriodicRows(kW, 299, 201, 3, 8);
    std::memcpy(src.data() + static_cast<size_t>(299) * kRowBytes, periodic.data(),
                periodic.size());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    const auto f1 = scrolltest::CutFrame(src, kW, 1000, spec);
    spec.contentY = 299;
    const auto f2 = scrolltest::CutFrame(src, kW, 1000, spec);
    spec.contentY = 0;
    const auto f3 = scrolltest::CutFrame(src, kW, 1000, spec);

    ScrollCaptureStitcher st(TestConfig());
    ASSERT_TRUE(st.StartSession(ViewOf(f1, kVH)));
    auto r = st.ProcessFrame(MakeFrameWithPrior(f2, 299));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(r.dy, 299);
    EXPECT_EQ(st.ViewportY(), 299);

    // 帧真值位移 -299；先验刻意偏差 -296 → 歧义消歧选错 → viewport 偏 +3
    r = st.ProcessFrame(MakeFrameWithPrior(f3, -296));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    // 滚回无追加触发周期校验，用存储强纹理段修正回 0
    EXPECT_TRUE(r.viewportCorrected);
    EXPECT_EQ(st.ViewportY(), 0);

    // 校验修正不破坏存储：导出仍逐字节等于源图 [0,799)
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 799);
    EXPECT_EQ(std::memcmp(out.data(), src.data(), out.size()), 0);
}

TEST(ScrollCaptureStitcherTest, DynamicContentToleratedAndExistingPixelsKept) {
    // 为什么：页面动态元素（头像/时钟）在重叠区只占少数行，90% 行容差
    // 保证 dy 估计不受影响；已写入存储的像素绝不被动态内容覆盖
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    spec.contentY = 299;
    auto f2 = scrolltest::CutFrame(src, kW, 900, spec);
    // 重叠区（帧内容带前 201 行）中前 30 行替换为动态内容（不同纹理种子）
    auto dynamic = scrolltest::MakeTexturedLongImage(kW, 30, 55);
    std::memcpy(f2.data() + spec.fixedHeader * kRowBytes, dynamic.data(), dynamic.size());

    const auto r = st.ProcessFrame(MakeFrame(f2));
    ASSERT_EQ(r.status, StitchStatus::Ok);  // 90% 行容差吸收 30/201 行动态内容
    EXPECT_EQ(r.dy, 299);

    // 导出不含动态内容：追加只发生在重叠区之外（已有像素优先）
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 799);
    EXPECT_EQ(std::memcmp(out.data(), src.data(), out.size()), 0);
}

TEST(ScrollCaptureStitcherTest, LimitReachedKeepsPartialResult) {
    // 为什么：达到上限（高度/内存）必须停止追加且保留已拼结果（设计 12.2），
    // 用户滚动很多仍能得到部分长图而不是失败
    StitcherConfig cfg;
    cfg.maxImageHeight = 700;
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(cfg);

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    ASSERT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

    spec.contentY = 200;  // 追加 200 行 → 高 700，恰好到顶
    auto r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    ASSERT_EQ(r.status, StitchStatus::Ok);
    EXPECT_EQ(st.GetStore()->Height(), 700);

    spec.contentY = 400;  // 再追加 200 行超限 → LimitReached
    r = st.ProcessFrame(MakeFrame(scrolltest::CutFrame(src, kW, 900, spec)));
    EXPECT_EQ(r.status, StitchStatus::LimitReached);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 700);  // 保留截断结果 == 源图 [0,700)
    EXPECT_EQ(std::memcmp(out.data(), src.data(), out.size()), 0);
    EXPECT_TRUE(st.GetLastErrorMessage().empty());
    EXPECT_EQ(st.GetLastErrorId(), TextId::Count);
    // 为什么：导出成功清除旧错误后，再处理帧仍须报告当前的上限状态。
    EXPECT_EQ(st.ProcessFrame({}).status, StitchStatus::LimitReached);
    EXPECT_EQ(st.GetLastErrorId(), TextId::ScrollLimitReached);
    EXPECT_FALSE(st.GetLastErrorMessage().empty());
}

TEST(ScrollCaptureStitcherTest, LimitIncludesFixedHeaderAndFooter) {
    StitcherConfig cfg;
    cfg.maxImageHeight = 700;
    constexpr int header = 40;
    constexpr int footer = 40;
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(cfg);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH - header - footer;
    spec.fixedHeader = header;
    spec.fixedFooter = footer;
    auto f1 = scrolltest::CutFrame(src, kW, 900, spec);
    ASSERT_TRUE(st.StartSession(ViewOf(f1, kVH)));
    spec.contentY = 200;
    auto f2 = scrolltest::CutFrame(src, kW, 900, spec);
    auto r = st.ProcessFrame({ViewOf(f2, kVH), 200, true, true});
    ASSERT_EQ(r.status, StitchStatus::Ok);
    spec.contentY = 300;
    auto f3 = scrolltest::CutFrame(src, kW, 900, spec);
    r = st.ProcessFrame({ViewOf(f3, kVH), 100, true, true});
    EXPECT_EQ(r.status, StitchStatus::LimitReached);
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));
    EXPECT_EQ(h, 700);
}

TEST(ScrollCaptureStitcherTest, IdenticalFrameYieldsNoChange) {
    // 为什么：注入未生效（页面到底）时帧不变，必须报 NoChange 让 Controller
    // 终止滚动（设计 11.6），而不是误判位移或报错
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);
    ScrollCaptureStitcher st(TestConfig());

    scrolltest::FrameSpec spec;
    spec.viewportHeight = kVH;
    spec.contentY = 0;
    const auto f1 = scrolltest::CutFrame(src, kW, 900, spec);
    ASSERT_TRUE(st.StartSession(ViewOf(f1, kVH)));

    const auto r = st.ProcessFrame(MakeFrame(f1));  // 完全相同的帧
    EXPECT_EQ(r.status, StitchStatus::NoChange);
    EXPECT_EQ(r.dy, 0);
    EXPECT_EQ(st.GetStore()->Height(), 0);  // 仍处于首帧暂存状态

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(st.ExportFinalImage(out, w, h));  // 导出兜底 = 首帧整帧
    EXPECT_EQ(h, kVH);
    EXPECT_EQ(std::memcmp(out.data(), f1.data(), out.size()), 0);
}

TEST(ScrollCaptureStitcherTest, UnstableFrameRequiresStricterConfidence) {
    // 为什么：稳定判定（设计 10.4）为 false 的帧更可能有运动模糊/撕裂，
    // 置信度门槛提高防止把中间态帧错拼进长图
    // 构造：重叠区 60/201 行不匹配 → ratio ≈ 0.70，落在 [0.60, 0.75) 区间
    const auto src = scrolltest::MakeTexturedLongImage(kW, 900, 7);

    auto MakeSession = [&](bool stable) {
        ScrollCaptureStitcher st(TestConfig());
        scrolltest::FrameSpec spec;
        spec.viewportHeight = kVH;
        spec.contentY = 0;
        EXPECT_TRUE(st.StartSession(ViewOf(scrolltest::CutFrame(src, kW, 900, spec), kVH)));

        spec.contentY = 299;
        auto f2 = scrolltest::CutFrame(src, kW, 900, spec);
        auto dynamic = scrolltest::MakeTexturedLongImage(kW, 60, 55);
        std::memcpy(f2.data(), dynamic.data(), dynamic.size());
        const auto r = st.ProcessFrame(FrameInput{ViewOf(f2, kVH), 0, false, stable});
        return std::make_pair(r.status, st.ViewportY());
    };

    const auto [okStatus, okY] = MakeSession(true);
    EXPECT_EQ(okStatus, StitchStatus::Ok);   // 0.70 ≥ 0.60：稳定帧接受
    EXPECT_EQ(okY, 299);

    const auto [failStatus, failY] = MakeSession(false);
    EXPECT_EQ(failStatus, StitchStatus::MatchFailed);  // 0.70 < 0.75：不稳定帧拒绝
    EXPECT_EQ(failY, 0);  // 拒绝帧不改变偏移状态
}

// 模拟等宽终端：固定行距、相同字符集合，每行仅排列顺序不同。
// 每个文字行的全宽亮度总和相同，但字符位置足以唯一确定滚动位移。
std::vector<uint8_t> MakeTerminalPage(int width, int height, int scale = 1) {
    constexpr uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}
    };
    std::vector<uint8_t> image(size_t(width) * height * 4, 24);
    for (size_t i = 3; i < image.size(); i += 4) image[i] = 255;
    for (int line = 0; line * 18 * scale < height; ++line) {
        int order[60];
        for (int i = 0; i < 60; ++i) order[i] = i % 10;
        for (int i = 59; i > 0; --i) {
            std::swap(order[i], order[scrolltest::Hash32(line + 1, i) % (i + 1)]);
        }
        for (int i = 0; i < 60; ++i) {
            for (int gy = 0; gy < 14; ++gy) {
                const int y = (line * 18 + 2 + gy) * scale;
                if (y >= height) continue;
                for (int gx = 0; gx < 5; ++gx) {
                    const int x = (8 + i * 10 + gx) * scale;
                    if (x >= width || !(digits[order[i]][gy / 2] & (1 << gx))) continue;
                    for (int sy = 0; sy < scale && y + sy < height; ++sy) {
                        for (int sx = 0; sx < scale && x + sx < width; ++sx) {
                            auto* pixel = image.data() + (size_t(y + sy) * width + x + sx) * 4;
                            pixel[0] = pixel[1] = pixel[2] = 220;
                        }
                    }
                }
            }
        }
    }
    return image;
}

TEST(ScrollCaptureStitcherTest, TerminalCharacterPositionsDisambiguateEqualRowBrightness) {
    for (const int scale : {1, 2}) {
        const int width = 640 * scale, height = 720 * scale;
        const int sourceHeight = 1800 * scale, shift = 198 * scale;
        const auto source = MakeTerminalPage(width, sourceHeight, scale);
        for (const int direction : {-1, 1}) {
            for (const bool prior : {false, true}) {
                scrolltest::FrameSpec spec;
                spec.viewportHeight = height;
                spec.contentY = direction > 0 ? 0 : shift;
                const auto first = scrolltest::CutFrame(source, width, sourceHeight, spec);
                spec.contentY += direction * shift;
                const auto next = scrolltest::CutFrame(source, width, sourceHeight, spec);
                ScrollCaptureStitcher stitcher;
                ASSERT_TRUE(stitcher.StartSession({first.data(), width, height, width * 4}));
                const auto result = stitcher.ProcessFrame({{next.data(), width, height, width * 4},
                    direction * 72 * scale, prior, true, direction});
                ASSERT_EQ(result.status, StitchStatus::Ok) << stitcher.GetLastErrorMessage();
                EXPECT_EQ(result.dy, direction * shift);
                std::vector<uint8_t> output;
                int outWidth = 0, outHeight = 0;
                ASSERT_TRUE(stitcher.ExportFinalImage(output, outWidth, outHeight));
                ASSERT_EQ(outHeight, height + shift);
                ASSERT_EQ(output.size(), size_t(outHeight) * width * 4);
                EXPECT_EQ(std::memcmp(output.data(), source.data(), output.size()), 0);
            }
        }
    }
}

TEST(ScrollCaptureStitcherTest, TerminalNoiseAndCursorPreserveExactShift) {
    constexpr int width = 640, height = 720;
    const auto source = MakeTerminalPage(width, 1800);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = height;
    const auto first = scrolltest::CutFrame(source, width, 1800, spec);
    spec.contentY = 198;
    spec.noiseSeed = 42;
    spec.noiseAmplitude = 4;
    auto next = scrolltest::CutFrame(source, width, 1800, spec);
    // 闪烁光标只改变局部像素，不能导致整幅文字画面匹配失败。
    for (int y = 200; y < 214; ++y) {
        for (int x = 320; x < 328; ++x) {
            auto* pixel = next.data() + (size_t(y) * width + x) * 4;
            pixel[0] = pixel[1] = pixel[2] = 220;
        }
    }
    ScrollCaptureStitcher stitcher;
    ASSERT_TRUE(stitcher.StartSession({first.data(), width, height, width * 4}));
    const auto result = stitcher.ProcessFrame({{next.data(), width, height, width * 4}, 72, true, true, 1});
    ASSERT_EQ(result.status, StitchStatus::Ok) << stitcher.GetLastErrorMessage();
    EXPECT_EQ(result.dy, 198);
}

TEST(ScrollCaptureStitcherTest, TerminalWithoutOverlapDoesNotAppendSimilarCharacters) {
    constexpr int width = 640, height = 720;
    const auto source = MakeTerminalPage(width, 2400);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = height;
    const auto first = scrolltest::CutFrame(source, width, 2400, spec);
    spec.contentY = 1080;
    const auto next = scrolltest::CutFrame(source, width, 2400, spec);
    ScrollCaptureStitcher stitcher;
    ASSERT_TRUE(stitcher.StartSession({first.data(), width, height, width * 4}));
    const auto result = stitcher.ProcessFrame({{next.data(), width, height, width * 4}, 198, true, true, 1});
    EXPECT_NE(result.status, StitchStatus::Ok);
    EXPECT_NE(result.status, StitchStatus::NoChange);
    EXPECT_EQ(stitcher.ViewportY(), 0);
    EXPECT_EQ(stitcher.GetStore()->Height(), 0);
}

}  // namespace
}  // namespace shotlet
