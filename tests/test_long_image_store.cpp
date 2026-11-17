// LongImageStore 单元测试
// 对应设计文档 16.1 的"分块存储的前插/后插与导出一致性"与"超过高度或内存限制停止追加并保留结果"。
// 每个测试顶部的"为什么"注释说明该行为存在的业务理由（CLAUDE.md Rule 9）。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "scrollcapture/LongImageStore.h"

namespace shotlet {
namespace {

constexpr int kW = 32;
constexpr int kRowBytes = kW * 4;

// 生成分块测试数据：第 y 行（全局标签 tag）像素 R/G 通道存标签高低字节，
// 行内容互异，使"行序错乱/重复"能被逐字节断言捕获
std::vector<uint8_t> MakeTaggedRows(int firstTag, int rowCount) {
    std::vector<uint8_t> v(static_cast<size_t>(rowCount) * kRowBytes, 0);
    for (int y = 0; y < rowCount; ++y) {
        const int tag = firstTag + y;
        uint8_t* row = v.data() + static_cast<size_t>(y) * kRowBytes;
        for (int x = 0; x < kW; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>(tag & 0xFF);
            row[x * 4 + 1] = static_cast<uint8_t>((tag >> 8) & 0xFF);
            row[x * 4 + 2] = 0x40;
            row[x * 4 + 3] = 0xFF;
        }
    }
    return v;
}

PixelView MakeView(const std::vector<uint8_t>& rows, int height) {
    return PixelView{rows.data(), kW, height, kRowBytes};
}

// 校验导出数据第 row 行的标签
void ExpectRowTag(const std::vector<uint8_t>& rgba, int row, int expectedTag) {
    const uint8_t* p = rgba.data() + static_cast<size_t>(row) * kRowBytes;
    const int tag = p[0] | (p[1] << 8);
    EXPECT_EQ(tag, expectedTag) << "行 " << row << " 标签不符（期望 " << expectedTag << "）";
}

TEST(LongImageStoreTest, InitializeSetsDimensionsAndRoundTrips) {
    // 为什么：初始化是所有后续追加的基准；若往返都不一致，拼接结果必然错误
    LongImageStore store(kW, 16);
    const auto band = MakeTaggedRows(0, 40);
    ASSERT_EQ(store.Initialize(MakeView(band, 40)), AppendResult::Ok);

    EXPECT_EQ(store.Width(), kW);
    EXPECT_EQ(store.Height(), 40);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(out, w, h));
    EXPECT_EQ(w, kW);
    EXPECT_EQ(h, 40);
    EXPECT_EQ(out.size(), band.size());
    EXPECT_EQ(std::memcmp(out.data(), band.data(), band.size()), 0);
}

TEST(LongImageStoreTest, InitializeAcceptsPaddedPitch) {
    // 为什么：ScreenCaptureKit 帧的行距可能带对齐填充，pitch 处理错误会读错行
    const auto band = MakeTaggedRows(0, 20);
    std::vector<uint8_t> padded(static_cast<size_t>(20) * (kRowBytes + 16), 0);
    for (int y = 0; y < 20; ++y) {
        std::memcpy(padded.data() + static_cast<size_t>(y) * (kRowBytes + 16),
                    band.data() + static_cast<size_t>(y) * kRowBytes, kRowBytes);
    }
    LongImageStore store(kW, 16);
    const PixelView view{padded.data(), kW, 20, kRowBytes + 16};
    ASSERT_EQ(store.Initialize(view), AppendResult::Ok);

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(out, w, h));
    EXPECT_EQ(std::memcmp(out.data(), band.data(), band.size()), 0);
}

TEST(LongImageStoreTest, PublishedSnapshotSurvivesHeadTailReplacementAndStoreDestruction) {
    // 预览线程读取旧快照时，拼接线程可能补满头尾块并销毁原存储。
    std::shared_ptr<const LongImageStore> snapshot;
    const auto original = MakeTaggedRows(100, 10);
    {
        LongImageStore store(kW, 32);
        ASSERT_EQ(store.Initialize(MakeView(original, 10)), AppendResult::Ok);
        snapshot = store.Snapshot();
        const auto extra = MakeTaggedRows(200, 8);
        ASSERT_EQ(store.AppendBottom(MakeView(extra, 8), 0, 8), AppendResult::Ok);
        ASSERT_EQ(store.AppendTop(MakeView(extra, 8), 0, 8), AppendResult::Ok);
        EXPECT_EQ(store.Height(), 26);
    }
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(snapshot->ExportRGBA(out, w, h));
    EXPECT_EQ(h, 10);
    EXPECT_EQ(out, original);
}

TEST(LongImageStoreTest, AppendBottomAppendsRowsInOrder) {
    // 为什么：底追加是向下滚动的主路径，行序错乱即内容重复/丢失
    LongImageStore store(kW, 16);
    const auto band0 = MakeTaggedRows(0, 40);
    ASSERT_EQ(store.Initialize(MakeView(band0, 40)), AppendResult::Ok);

    const auto band1 = MakeTaggedRows(100, 20);
    const auto band2 = MakeTaggedRows(200, 30);
    ASSERT_EQ(store.AppendBottom(MakeView(band1, 20), 0, 20), AppendResult::Ok);
    ASSERT_EQ(store.AppendBottom(MakeView(band2, 30), 5, 25), AppendResult::Ok);

    EXPECT_EQ(store.Height(), 40 + 20 + 25);
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(out, w, h));
    for (int y = 0; y < 40; ++y) ExpectRowTag(out, y, y);
    for (int y = 0; y < 20; ++y) ExpectRowTag(out, 40 + y, 100 + y);
    for (int y = 0; y < 25; ++y) ExpectRowTag(out, 60 + y, 200 + 5 + y);
}

TEST(LongImageStoreTest, SmallAppendsKeepEveryPublishedSnapshotImmutable) {
    // 为什么：两端连续的小步追加不能改写预览线程正在读取的旧像素，前插亦然。
    LongImageStore store(kW, 64);
    auto first = MakeTaggedRows(100, 7);
    ASSERT_EQ(store.Initialize(MakeView(first, 7)), AppendResult::Ok);
    std::vector<std::shared_ptr<const LongImageStore>> snapshots;
    std::vector<std::vector<uint8_t>> truths;
    auto truth = first;
    for (int i = 0; i < 40; ++i) {
        snapshots.push_back(store.Snapshot());
        truths.push_back(truth);
        auto rows = MakeTaggedRows(200 + i * 3, 3);
        if (i % 2 == 0) {
            ASSERT_EQ(store.AppendBottom(MakeView(rows, 3), 0, 3), AppendResult::Ok);
            truth.insert(truth.end(), rows.begin(), rows.end());
        } else {
            ASSERT_EQ(store.AppendTop(MakeView(rows, 3), 0, 3), AppendResult::Ok);
            truth.insert(truth.begin(), rows.begin(), rows.end());
        }
    }
    std::vector<uint8_t> output;
    int width = 0, height = 0;
    ASSERT_TRUE(store.ExportRGBA(output, width, height));
    EXPECT_EQ(output, truth);
    for (size_t i = 0; i < snapshots.size(); ++i) {
        ASSERT_TRUE(snapshots[i]->ExportRGBA(output, width, height));
        EXPECT_EQ(output, truths[i]);
    }
}

TEST(LongImageStoreTest, PublishedSnapshotCanBeReadWhileProducerFillsReservedSpace) {
    // 为什么：共享同一分配块时，后台预览和小步写入必须访问互不重叠的可见区间。
    LongImageStore store(kW, 64);
    const auto original = MakeTaggedRows(100, 7);
    ASSERT_EQ(store.Initialize(MakeView(original, 7)), AppendResult::Ok);
    const auto snapshot = store.Snapshot();
    std::promise<void> ready;
    auto started = ready.get_future();
    std::atomic<bool> done{false}, correct{true};
    std::thread reader([&] {
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        ready.set_value();
        do {
            if (!snapshot->ExportRGBA(pixels, width, height) || pixels != original) correct = false;
        } while (!done.load());
    });
    started.wait();
    bool appended = true;
    for (int i = 0; i < 512; ++i) {
        const auto row = MakeTaggedRows(200 + i, 1);
        const auto result = i % 2 == 0 ? store.AppendTop(MakeView(row, 1), 0, 1)
                                      : store.AppendBottom(MakeView(row, 1), 0, 1);
        appended = appended && result == AppendResult::Ok;
    }
    done = true;
    reader.join();
    EXPECT_TRUE(appended);
    EXPECT_TRUE(correct.load());
}

TEST(LongImageStoreTest, AppendTopPrependsWithoutTouchingExistingTiles) {
    // 为什么：前插不搬移已有内容是选择分块存储的动机（单一缓冲前插是 O(n²)）；
    // 已写入长图的像素绝不能被顶部追加改动（拼接缝"已有内容优先"的基础）
    LongImageStore store(kW, 100);
    const auto band = MakeTaggedRows(0, 2000);
    ASSERT_EQ(store.Initialize(MakeView(band, 2000)), AppendResult::Ok);
    std::vector<uint8_t> before;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(before, w, h));

    const auto top1 = MakeTaggedRows(5000, 300);
    const auto top2 = MakeTaggedRows(6000, 250);
    ASSERT_EQ(store.AppendTop(MakeView(top1, 300), 0, 300), AppendResult::Ok);
    ASSERT_EQ(store.AppendTop(MakeView(top2, 250), 10, 230), AppendResult::Ok);

    EXPECT_EQ(store.Height(), 2000 + 300 + 230);
    // 原有 2000 行逐字节不变
    std::vector<uint8_t> after;
    ASSERT_TRUE(store.ExportRGBA(after, w, h));
    EXPECT_EQ(std::memcmp(after.data() + static_cast<size_t>(530) * kRowBytes,
                          before.data(), before.size()),
              0);
    // 第二次顶部追加（tags 6010..）位于最顶部，把第一次追加（tags 5000..）压到其下
    for (int y = 0; y < 230; ++y) ExpectRowTag(after, y, 6000 + 10 + y);
    for (int y = 0; y < 300; ++y) ExpectRowTag(after, 230 + y, 5000 + y);
}

TEST(LongImageStoreTest, InterleavedTopBottomKeepsGlobalRowOrder) {
    // 为什么：上下交替滚动时顶/底追加交错发生，全局行序是长图正确性的底线
    LongImageStore store(kW, 32);
    const auto band = MakeTaggedRows(1000, 50);
    ASSERT_EQ(store.Initialize(MakeView(band, 50)), AppendResult::Ok);

    // 期望的全局行标签序列：底追加接尾、顶追加接头
    std::vector<int> expected;
    for (int y = 0; y < 50; ++y) expected.push_back(1000 + y);

    int nextTag = 2000;  // 每批标签唯一，方向无关
    for (int i = 0; i < 7; ++i) {
        const int rows = 10 + i * 3;
        const auto rowsData = MakeTaggedRows(nextTag, rows);
        if (i % 2 == 0) {
            ASSERT_EQ(store.AppendBottom(MakeView(rowsData, rows), 0, rows), AppendResult::Ok);
            for (int y = 0; y < rows; ++y) expected.push_back(nextTag + y);
        } else {
            ASSERT_EQ(store.AppendTop(MakeView(rowsData, rows), 0, rows), AppendResult::Ok);
            std::vector<int> tags(rows);
            for (int y = 0; y < rows; ++y) tags[y] = nextTag + y;
            expected.insert(expected.begin(), tags.begin(), tags.end());
        }
        nextTag += rows;
    }

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(out, w, h));
    EXPECT_EQ(h, static_cast<int>(expected.size()));
    for (int y = 0; y < h; ++y) ExpectRowTag(out, y, expected[y]);
}

TEST(LongImageStoreTest, AppendAcrossMultipleTilesMatchesSingleBuffer) {
    // 为什么：分块是实现细节，导出必须与单缓冲结果逐字节一致
    LongImageStore store(kW, 100);
    const auto band = MakeTaggedRows(0, 100);
    ASSERT_EQ(store.Initialize(MakeView(band, 100)), AppendResult::Ok);
    const auto extra = MakeTaggedRows(100, 260);
    ASSERT_EQ(store.AppendBottom(MakeView(extra, 260), 0, 260), AppendResult::Ok);
    EXPECT_EQ(store.Height(), 360);

    // 直接拼接的单缓冲真值
    std::vector<uint8_t> truth;
    truth.insert(truth.end(), band.begin(), band.end());
    truth.insert(truth.end(), extra.begin(), extra.end());

    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(out, w, h));
    EXPECT_EQ(h, 360);
    EXPECT_EQ(out.size(), truth.size());
    EXPECT_EQ(std::memcmp(out.data(), truth.data(), truth.size()), 0);
}

TEST(LongImageStoreTest, ReadRowsSpansTileBoundaries) {
    // 为什么：预览缩略图与周期校验按行区间读取，跨 tile 边界读取错误会污染校验结果
    LongImageStore store(kW, 100);
    const auto band = MakeTaggedRows(0, 300);
    ASSERT_EQ(store.Initialize(MakeView(band, 300)), AppendResult::Ok);

    std::vector<uint8_t> dst(static_cast<size_t>(50) * (kRowBytes + 8), 0xAB);
    ASSERT_TRUE(store.ReadRows(80, 50, dst.data(), kRowBytes + 8));
    for (int y = 0; y < 50; ++y) {
        const uint8_t* row = dst.data() + static_cast<size_t>(y) * (kRowBytes + 8);
        const int tag = row[0] | (row[1] << 8);
        EXPECT_EQ(tag, 80 + y);
        // pitch 填充字节不被写入
        EXPECT_EQ(row[kRowBytes], 0xAB);
    }

    // 越界读取必须失败
    EXPECT_FALSE(store.ReadRows(290, 20, dst.data(), kRowBytes));
}

TEST(LongImageStoreTest, AppendExceedingMaxHeightReturnsLimitExceededAndKeepsResult) {
    // 为什么：达到上限时"停止追加但保留已拼结果"是设计 12.2 的硬性要求，
    // 上限触发丢内容属于数据丢失事故
    LongImageStore store(kW, 16, /*maxHeight=*/500);
    const auto band = MakeTaggedRows(0, 300);
    ASSERT_EQ(store.Initialize(MakeView(band, 300)), AppendResult::Ok);
    const auto more = MakeTaggedRows(300, 200);
    ASSERT_EQ(store.AppendBottom(MakeView(more, 200), 0, 200), AppendResult::Ok);
    EXPECT_EQ(store.Height(), 500);

    std::vector<uint8_t> before;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(before, w, h));

    const auto overflow = MakeTaggedRows(999, 100);
    EXPECT_EQ(store.AppendBottom(MakeView(overflow, 100), 0, 100), AppendResult::LimitExceeded);
    EXPECT_EQ(store.AppendTop(MakeView(overflow, 100), 0, 100), AppendResult::LimitExceeded);
    EXPECT_FALSE(store.GetLastErrorMessage().empty());

    // 高度与像素均保持不变
    EXPECT_EQ(store.Height(), 500);
    std::vector<uint8_t> after;
    ASSERT_TRUE(store.ExportRGBA(after, w, h));
    EXPECT_EQ(after, before);
    EXPECT_TRUE(store.WouldExceedLimits(1));
}

TEST(LongImageStoreTest, AppendExceedingMemoryLimitReturnsLimitExceeded) {
    // 为什么：内存上限与高度上限独立触发（窄而长的图先碰内存、宽图先碰高度）
    constexpr int64_t kMaxBytes = static_cast<int64_t>(kRowBytes) * 600;
    LongImageStore store(kW, 16, ScrollCaptureLimits::kMaxImageHeight, kMaxBytes);
    const auto band = MakeTaggedRows(0, 400);
    ASSERT_EQ(store.Initialize(MakeView(band, 400)), AppendResult::Ok);

    const auto overflow = MakeTaggedRows(999, 300);
    EXPECT_EQ(store.AppendBottom(MakeView(overflow, 300), 0, 300), AppendResult::LimitExceeded);
    EXPECT_EQ(store.Height(), 400);
}

TEST(LongImageStoreTest, EmptyAppendTopHonorsRequestedRows) {
    LongImageStore store(kW, 16);
    const auto rows = MakeTaggedRows(100, 40);
    ASSERT_EQ(store.AppendTop(MakeView(rows, 40), 10, 20), AppendResult::Ok);
    EXPECT_EQ(store.Height(), 20);
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(store.ExportRGBA(out, w, h));
    for (int y = 0; y < 20; ++y) ExpectRowTag(out, y, 110 + y);
}

TEST(LongImageStoreTest, ReservedFixedRowsAreIncludedInLimitCheck) {
    LongImageStore store(kW, 16, 100, static_cast<int64_t>(kRowBytes) * 100);
    const auto rows = MakeTaggedRows(0, 80);
    ASSERT_EQ(store.Initialize(MakeView(rows, 80)), AppendResult::Ok);
    EXPECT_TRUE(store.WouldExceedLimits(10, 11));
    EXPECT_FALSE(store.WouldExceedLimits(10, 10));
}

TEST(LongImageStoreTest, InvalidRowsOrWidthReturnsInvalidArgument) {
    // 为什么：InvalidArgument 与 LimitExceeded 语义不同（bug vs 预期业务事件），
    // Controller 需按设计 14 节分提示文案
    LongImageStore store(kW, 16);
    const auto band = MakeTaggedRows(0, 40);
    ASSERT_EQ(store.Initialize(MakeView(band, 40)), AppendResult::Ok);

    EXPECT_EQ(store.AppendBottom(MakeView(band, 40), -1, 10), AppendResult::InvalidArgument);
    EXPECT_EQ(store.AppendBottom(MakeView(band, 40), 30, 20), AppendResult::InvalidArgument);
    EXPECT_EQ(store.AppendBottom(MakeView(band, 40), 0, 0), AppendResult::InvalidArgument);

    // 宽度不匹配
    std::vector<uint8_t> wrongW(static_cast<size_t>(10) * 24 * 4, 0);
    const PixelView wrongView{wrongW.data(), 24, 10, 24 * 4};
    EXPECT_EQ(store.AppendBottom(wrongView, 0, 10), AppendResult::InvalidArgument);

    // 空数据
    const PixelView nullView{nullptr, kW, 10, kRowBytes};
    EXPECT_EQ(store.AppendBottom(nullView, 0, 10), AppendResult::InvalidArgument);

    // 重复初始化
    EXPECT_EQ(store.Initialize(MakeView(band, 40)), AppendResult::InvalidArgument);

    // 构造参数非法的 store：一切追加失败
    LongImageStore badStore(0, 16);
    EXPECT_EQ(badStore.Initialize(MakeView(band, 40)), AppendResult::InvalidArgument);
    EXPECT_EQ(store.Height(), 40);
}

TEST(LongImageStoreTest, ComposeFinalImagePlacesHeaderAndFooterOnce) {
    // 为什么：固定头/尾各只出现一次是本算法存在的理由（11.3）——
    // 头尾随帧重复出现会在长图中反复出现同一内容
    LongImageStore store(kW, 16);
    const auto content = MakeTaggedRows(100, 100);
    ASSERT_EQ(store.Initialize(MakeView(content, 100)), AppendResult::Ok);

    const auto header = MakeTaggedRows(9000, 10);
    const auto footer = MakeTaggedRows(8000, 8);
    std::vector<uint8_t> out;
    int w = 0, h = 0;
    ASSERT_TRUE(ComposeFinalImage(store, MakeView(header, 10), MakeView(footer, 8), out, w, h));
    EXPECT_EQ(w, kW);
    EXPECT_EQ(h, 10 + 100 + 8);

    // 头在顶部、内容在中、尾在底部
    for (int y = 0; y < 10; ++y) ExpectRowTag(out, y, 9000 + y);
    for (int y = 0; y < 100; ++y) ExpectRowTag(out, 10 + y, 100 + y);
    for (int y = 0; y < 8; ++y) ExpectRowTag(out, 110 + y, 8000 + y);

    // 空头/尾允许：输出即内容带
    std::vector<uint8_t> out2;
    ASSERT_TRUE(ComposeFinalImage(store, PixelView{}, PixelView{}, out2, w, h));
    EXPECT_EQ(h, 100);
    EXPECT_EQ(out2, content);
}

}  // namespace
}  // namespace shotlet
