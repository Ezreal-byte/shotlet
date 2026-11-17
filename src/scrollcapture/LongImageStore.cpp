#include "scrollcapture/LongImageStore.h"

#include <algorithm>
#include <cstring>

namespace shotlet {

namespace {
constexpr int kBytesPerPixel = 4;
}  // namespace

LongImageStore::LongImageStore(int width, int tileHeight, int64_t maxHeight, int64_t maxRgbaBytes)
    : m_width(width),
      m_tileHeight(tileHeight),
      m_limitMaxHeight(maxHeight),
      m_limitMaxRgbaBytes(maxRgbaBytes) {
    if (width <= 0 || tileHeight <= 0) {
        m_lastError = "LongImageStore 构造参数非法：width 与 tileHeight 必须为正";
    }
}

bool LongImageStore::ValidateFrameRows(const PixelView& frame, int startRow, int rowCount) {
    if (m_width <= 0 || m_tileHeight <= 0) {
        return false;  // 构造错误信息已记录
    }
    if (frame.data == nullptr) {
        m_lastError = "像素数据为空";
        return false;
    }
    if (frame.width != m_width || frame.height <= 0) {
        m_lastError = "帧尺寸与存储宽度不匹配";
        return false;
    }
    if (frame.pitch < frame.width * kBytesPerPixel) {
        m_lastError = "帧行距 pitch 小于 width*4";
        return false;
    }
    if (startRow < 0 || rowCount <= 0 || startRow + rowCount > frame.height) {
        m_lastError = "行区间越界";
        return false;
    }
    return true;
}

AppendResult LongImageStore::Initialize(const PixelView& band) {
    if (m_height > 0) {
        m_lastError = "存储已初始化，不允许重复 Initialize";
        return AppendResult::InvalidArgument;
    }
    if (!ValidateFrameRows(band, 0, band.height)) {
        return AppendResult::InvalidArgument;
    }
    if (WouldExceedLimits(band.height)) {
        m_lastError = "达到最大长图高度或内存限制";
        return AppendResult::LimitExceeded;
    }
    AppendTiles(band, 0, band.height, /*toTop=*/false);
    return AppendResult::Ok;
}

AppendResult LongImageStore::AppendBottom(const PixelView& frame, int startRow, int rowCount) {
    if (!ValidateFrameRows(frame, startRow, rowCount)) {
        return AppendResult::InvalidArgument;
    }
    if (WouldExceedLimits(rowCount)) {
        m_lastError = "达到最大长图高度或内存限制";
        return AppendResult::LimitExceeded;
    }
    AppendTiles(frame, startRow, rowCount, /*toTop=*/false);
    return AppendResult::Ok;
}

AppendResult LongImageStore::AppendTop(const PixelView& frame, int startRow, int rowCount) {
    if (m_height == 0) {
        // 空存储也必须尊重调用方给出的行区间，不能把整帧误当成追加内容。
        if (!ValidateFrameRows(frame, startRow, rowCount)) {
            return AppendResult::InvalidArgument;
        }
        return Initialize(PixelView{frame.data + static_cast<size_t>(startRow) * frame.pitch,
                                    frame.width, rowCount, frame.pitch});
    }
    if (!ValidateFrameRows(frame, startRow, rowCount)) {
        return AppendResult::InvalidArgument;
    }
    if (WouldExceedLimits(rowCount)) {
        m_lastError = "达到最大长图高度或内存限制";
        return AppendResult::LimitExceeded;
    }
    AppendTiles(frame, startRow, rowCount, /*toTop=*/true);
    return AppendResult::Ok;
}

void LongImageStore::AppendTiles(const PixelView& frame, int startRow, int rowCount, bool toTop) {
    const size_t rowBytes = RowBytes();
    // 活动头尾最多各预留一个块；容量不超过单张结果允许的行数。
    const int capacity = static_cast<int>(std::min<int64_t>({m_tileHeight, m_limitMaxHeight,
        m_limitMaxRgbaBytes / static_cast<int64_t>(rowBytes)}));
    int remaining = rowCount;
    int cursor = toTop ? startRow + rowCount : startRow;
    while (remaining > 0) {
        Tile* tile = m_tiles.empty() ? nullptr : (toTop ? &m_tiles.front() : &m_tiles.back());
        int spare = tile ? (toTop ? tile->begin : tile->capacity - tile->begin - tile->height) : 0;
        if (spare == 0) {
            Tile fresh;
            fresh.capacity = capacity;
            fresh.begin = toTop ? capacity : 0;
            fresh.data.reset(new uint8_t[static_cast<size_t>(capacity) * rowBytes]);
            if (toTop) m_tiles.push_front(std::move(fresh));
            else m_tiles.push_back(std::move(fresh));
            tile = toTop ? &m_tiles.front() : &m_tiles.back();
            spare = capacity;
        }
        const int take = std::min(spare, remaining);
        const int destination = toTop ? tile->begin - take : tile->begin + tile->height;
        const int source = toTop ? cursor - take : cursor;
        // 快照复制了 begin/height，仅会读旧区间。共享块也可写未发布的独立字节，
        // 不需要重拷旧行；每个位置在单生产者生命周期中只写一次。
        for (int i = 0; i < take; ++i) {
            std::memcpy(tile->data.get() + static_cast<size_t>(destination + i) * rowBytes,
                        frame.Row(source + i), rowBytes);
        }
        if (toTop) tile->begin = destination;
        tile->height += take;
        cursor += toTop ? -take : take;
        remaining -= take;
    }
    m_height += rowCount;
}

bool LongImageStore::WouldExceedLimits(int additionalRows) const {
    return WouldExceedLimits(additionalRows, 0);
}

bool LongImageStore::WouldExceedLimits(int additionalRows, int reservedRows) const {
    if (additionalRows < 0 || reservedRows < 0) {
        return true;
    }
    const int64_t newHeight = static_cast<int64_t>(m_height) + additionalRows + reservedRows;
    if (newHeight > m_limitMaxHeight) {
        return true;
    }
    return newHeight * static_cast<int64_t>(m_width) * kBytesPerPixel > m_limitMaxRgbaBytes;
}

void LongImageStore::LocateRow(int row, const Tile*& outTile, int& outRowInTile) const {
    int start = 0;
    for (const Tile& tile : m_tiles) {
        if (row < start + tile.height) {
            outTile = &tile;
            outRowInTile = row - start;
            return;
        }
        start += tile.height;
    }
    outTile = nullptr;  // 不应到达：调用方已做边界检查
    outRowInTile = 0;
}

bool LongImageStore::ReadRows(int row, int count, uint8_t* dst, int dstPitch) const {
    if (row < 0 || count < 0 || row + count > m_height) {
        m_lastError = "ReadRows 行区间越界";
        return false;
    }
    if (count == 0) {
        return true;
    }
    if (dst == nullptr || dstPitch < static_cast<int>(RowBytes())) {
        m_lastError = "ReadRows 目标缓冲非法";
        return false;
    }
    // 逐 tile 拷贝重叠段
    int start = 0;
    for (const Tile& tile : m_tiles) {
        const int tileEnd = start + tile.height;
        if (tileEnd <= row) {
            start = tileEnd;
            continue;
        }
        if (start >= row + count) {
            break;
        }
        const int overlapStart = std::max(start, row);
        const int overlapEnd = std::min(tileEnd, row + count);
        const int rowsToCopy = overlapEnd - overlapStart;
        for (int i = 0; i < rowsToCopy; ++i) {
            std::memcpy(dst + static_cast<size_t>(overlapStart - row + i) * dstPitch,
                        tile.data.get() + static_cast<size_t>(tile.begin + overlapStart - start + i) * RowBytes(),
                        RowBytes());
        }
        start = tileEnd;
    }
    return true;
}

bool LongImageStore::ExportRGBA(std::vector<uint8_t>& out, int& outWidth, int& outHeight) const {
    if (m_height <= 0) {
        m_lastError = "存储为空，无可导出内容";
        return false;
    }
    const size_t rowBytes = RowBytes();
    out.assign(static_cast<size_t>(m_height) * rowBytes, 0);
    int start = 0;
    for (const Tile& tile : m_tiles) {
        std::memcpy(out.data() + static_cast<size_t>(start) * rowBytes, tile.data.get() + static_cast<size_t>(tile.begin) * rowBytes,
                    static_cast<size_t>(tile.height) * rowBytes);
        start += tile.height;
    }
    outWidth = m_width;
    outHeight = m_height;
    return true;
}

bool ComposeFinalImage(const LongImageStore& store,
                       const PixelView& fixedHeader, const PixelView& fixedFooter,
                       std::vector<uint8_t>& outRGBA, int& outWidth, int& outHeight) {
    if (store.Height() <= 0) {
        return false;
    }
    const int width = store.Width();
    const auto validate = [&](const PixelView& v) {
        if (v.height < 0 || (v.height > 0 && v.data == nullptr)) {
            return false;
        }
        if (v.height > 0 && (v.width != width || v.pitch < width * kBytesPerPixel)) {
            return false;
        }
        return true;
    };
    if (!validate(fixedHeader) || !validate(fixedFooter)) {
        return false;
    }
    if (store.WouldExceedLimits(0, fixedHeader.height + fixedFooter.height)) {
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(width) * kBytesPerPixel;
    outHeight = fixedHeader.height + store.Height() + fixedFooter.height;
    outWidth = width;
    outRGBA.assign(static_cast<size_t>(outHeight) * rowBytes, 0);

    // 固定头（取首帧）→ 内容带 → 固定尾（取最新帧），各出现一次
    for (int y = 0; y < fixedHeader.height; ++y) {
        std::memcpy(outRGBA.data() + y * rowBytes, fixedHeader.Row(y), rowBytes);
    }
    if (!store.ReadRows(0, store.Height(),
                        outRGBA.data() + static_cast<size_t>(fixedHeader.height) * rowBytes,
                        static_cast<int>(rowBytes))) {
        return false;
    }
    const size_t footerOffset =
        static_cast<size_t>(fixedHeader.height + store.Height()) * rowBytes;
    for (int y = 0; y < fixedFooter.height; ++y) {
        std::memcpy(outRGBA.data() + footerOffset + y * rowBytes, fixedFooter.Row(y), rowBytes);
    }
    return true;
}

}  // namespace shotlet
