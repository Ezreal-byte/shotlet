#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "scrollcapture/ScrollCaptureTypes.h"

namespace shotlet {

// 追加结果：Controller 需程序化区分"达到上限（保留结果、文案特定）"与"参数非法（bug）"，
// 仅靠 bool 或错误字符串匹配太脆（设计文档 12.2 / 14）
enum class AppendResult {
    Ok,               // 追加成功
    LimitExceeded,    // 达到高度/内存上限：未写入任何像素、高度不变（保留已有结果）
    InvalidArgument,  // 参数非法（视图/行区间/宽度不匹配）
};

// 长图分块存储（设计文档 12.1）
//
// 以固定高度 tile 存储横向切片，front 为最顶部 tile：
// - 底部追加：补足尾 tile 空余后追加新 tile
// - 顶部追加：补足头 tile 空余后前插新 tile，不搬移已有 tile（前插 O(k·width)）
// - 上限触发时不写任何像素、不改高度
class LongImageStore {
public:
    // maxHeight / maxRgbaBytes 为上限，测试可调小
    LongImageStore(int width, int tileHeight,
                   int64_t maxHeight = ScrollCaptureLimits::kMaxImageHeight,
                   int64_t maxRgbaBytes = ScrollCaptureLimits::kMaxRgbaBytes);

    // 用首帧内容带初始化（仅允许调用一次，行区间由调用方裁好）
    AppendResult Initialize(const PixelView& band);

    // 把 frame 的 [startRow, startRow+rowCount) 追加到长图底部
    AppendResult AppendBottom(const PixelView& frame, int startRow, int rowCount);

    // 把 frame 的 [startRow, startRow+rowCount) 追加到长图顶部
    // （该区间的最后一行将紧贴原顶部内容）
    AppendResult AppendTop(const PixelView& frame, int startRow, int rowCount);

    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }  // 当前内容总高（像素行）

    // 追加 additionalRows 是否会触发上限（写入前预检）
    [[nodiscard]] bool WouldExceedLimits(int additionalRows) const;
    // 追加后还需为固定头/尾预留的行数；用于保证最终合成图也不越过上限。
    [[nodiscard]] bool WouldExceedLimits(int additionalRows, int reservedRows) const;
    [[nodiscard]] int64_t MaxHeight() const { return m_limitMaxHeight; }
    [[nodiscard]] int64_t MaxRgbaBytes() const { return m_limitMaxRgbaBytes; }

    // 拷贝长图 [row, row+count) 到 dst（支持自定义 dstPitch），跨 tile 边界自动拼接
    bool ReadRows(int row, int count, uint8_t* dst, int dstPitch) const;

    // 快照冻结块的可见行区间；追加只写区间外未发布的空间，旧像素不改写。
    [[nodiscard]] std::shared_ptr<const LongImageStore> Snapshot() const {
        return std::shared_ptr<const LongImageStore>(new LongImageStore(*this));
    }

    // 导出为单张连续紧凑 RGBA（v1 上限内可行；分块导出留待 v2）
    bool ExportRGBA(std::vector<uint8_t>& out, int& outWidth, int& outHeight) const;

    [[nodiscard]] const std::string& GetLastErrorMessage() const { return m_lastError; }

private:
    // 共享写入空间只能有一个生产者；复制仅用于不可变快照。
    LongImageStore(const LongImageStore&) = default;
    LongImageStore& operator=(const LongImageStore&) = delete;
    struct Tile {
        int height = 0;
        int begin = 0, capacity = 0; // 可见区间为 [begin, begin+height)，其余空间尚未发布
        std::shared_ptr<uint8_t[]> data;
    };

    // 参数校验：失败时写入 m_lastError
    bool ValidateFrameRows(const PixelView& frame, int startRow, int rowCount);

    // 实际写入（调用前已完成校验与上限检查）：把 frame 的 [startRow, startRow+rowCount)
    // 追加到底部/顶部
    void AppendTiles(const PixelView& frame, int startRow, int rowCount, bool toTop);

    // 定位长图第 row 行所在 tile：返回 tile 指针与 tile 内行号
    void LocateRow(int row, const Tile*& outTile, int& outRowInTile) const;

    [[nodiscard]] size_t RowBytes() const {
        return static_cast<size_t>(m_width) * 4;
    }

    int m_width = 0;
    int m_tileHeight = 0;
    int m_height = 0;             // 逻辑总高（像素行）
    int64_t m_limitMaxHeight = 0;
    int64_t m_limitMaxRgbaBytes = 0;
    std::deque<Tile> m_tiles;     // front = 最顶部 tile
    mutable std::string m_lastError;  // const 查询失败时也可读取原因
};

// 最终合成：固定头 + 内容带 + 固定尾，头尾各出现一次（设计文档 11.3）
// header / footer 允许为空（height == 0）
bool ComposeFinalImage(const LongImageStore& store,
                       const PixelView& fixedHeader, const PixelView& fixedFooter,
                       std::vector<uint8_t>& outRGBA, int& outWidth, int& outHeight);

}  // namespace shotlet
