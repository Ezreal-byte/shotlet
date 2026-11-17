#include "scrollcapture/ScrollCaptureStitcher.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace shotlet {

namespace {

// Rec.601 整数 luma 近似，避免热路径浮点
inline int Luma(const uint8_t* p) {
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

// 行级/像素级亮度容差（容忍噪声与动态内容，设计 11.4）
constexpr int kLumaTol = 16;

// 像素验证保留的候选数上限
constexpr int kMaxCandidates = 8;

}  // namespace

ScrollCaptureStitcher::ScrollCaptureStitcher(const StitcherConfig& config)
    : m_config(config) {}

bool ScrollCaptureStitcher::StartSession(const PixelView& firstFrame) {
    ClearLastError();
    if (m_started) {
        m_lastErrorId = TextId::StitchAlreadyStarted;
        m_lastError = "会话已开始";
        return false;
    }
    if (firstFrame.data == nullptr || firstFrame.width <= 0 || firstFrame.height <= 0 ||
        firstFrame.pitch < firstFrame.width * 4) {
        m_lastErrorId = TextId::StitchInvalidFrame;
        m_lastError = "首帧参数非法";
        return false;
    }
    m_frameW = firstFrame.width;
    m_frameH = firstFrame.height;
    m_store = std::make_unique<LongImageStore>(m_frameW, m_config.tileHeight,
                                               m_config.maxImageHeight, m_config.maxRgbaBytes);
    // 首帧与上一帧紧凑拷贝（pitch 归一）
    const size_t rowBytes = static_cast<size_t>(m_frameW) * 4;
    m_firstFrame.assign(static_cast<size_t>(m_frameH) * rowBytes, 0);
    for (int y = 0; y < m_frameH; ++y) {
        std::memcpy(m_firstFrame.data() + y * rowBytes, firstFrame.Row(y), rowBytes);
    }
    m_prevFrame = m_firstFrame;  // 首帧即首对的前项
    ComputeGray(firstFrame, m_prevGray);
    m_started = true;
    m_storeInitialized = false;
    m_limitReached = false;
    m_driftRisk = false;
    m_viewportY = 0;
    m_headerH = 0;
    m_footerH = 0;
    m_okFrames = 0;
    m_displacedPairs = 0;
    return true;
}

PixelView ScrollCaptureStitcher::BandOf(const PixelView& frame) const {
    // 尚未确认固定区域时使用全帧，同屏静止结构由位移评分排除。
    // 两帧使用相同切片，位移与切片位置无关。
    const int height = frame.height - m_headerH - m_footerH;
    return PixelView{frame.data + static_cast<size_t>(m_headerH) * frame.pitch, frame.width,
                     height, frame.pitch};
}

StitchResult ScrollCaptureStitcher::ProcessFrame(const FrameInput& frame) {
    ClearLastError();
    StitchResult result;
    if (!m_started) {
        m_lastErrorId = TextId::StitchNotStarted;
        m_lastError = "会话未开始";
        result.status = StitchStatus::MatchFailed;
        return result;
    }
    if (m_limitReached) {
        m_lastErrorId = TextId::ScrollLimitReached;
        m_lastError = "长图已达到存储上限";
        result.status = StitchStatus::LimitReached;
        return result;
    }
    const PixelView f = frame.pixels;
    if (f.data == nullptr || f.width != m_frameW || f.height != m_frameH ||
        f.pitch < m_frameW * 4) {
        m_lastErrorId = TextId::StitchInvalidFrame;
        m_lastError = "帧尺寸与首帧不一致";
        result.status = StitchStatus::MatchFailed;
        return result;
    }

    const PixelView prevFull{m_prevFrame.data(), m_frameW, m_frameH, m_frameW * 4};
    const PixelView curFull = f;
    ComputeGray(curFull, m_curGray);
    m_curFeatures.beginY = -1;
    SelectMatchColumns();
    ShiftEstimate est = EstimateShift(BandOf(prevFull), BandOf(curFull), frame);
    if (!est.ok && est.status == StitchStatus::MatchFailed && frame.hasPrior) {
        // 先验不是硬约束：迟到的滚动可能超出本步窗口。兜底不使用先验消歧，
        // 并提高验证门槛，只接受具有足够重叠的唯一可靠候选。
        FrameInput fallback = frame;
        fallback.hasPrior = false;
        fallback.stable = false;
        est = EstimateShift(BandOf(prevFull), BandOf(curFull), fallback, true);
        result.usedFallbackSearch = true;
    }
    result.usedAnchorSearch = est.usedAnchorSearch;
    result.usedColumnExclusion = est.usedColumnExclusion;
    result.confidence = est.confidence;  // 失败时也保留真实最佳分数。
    if (!est.ok) {
        if (est.status == StitchStatus::InsufficientInfo) {
            // 比较整帧与内容带，区分采集空白和固定区域裁剪导致的特征丢失。
            MatchFeatures full;
            ComputeFeatures(m_curGray, 0, m_frameH, full);
            const auto fullEffective = std::count(full.blank.begin(), full.blank.end(), uint8_t{0});
            m_lastError += "，整帧有效行=" + std::to_string(fullEffective) + "/" +
                           std::to_string(m_frameH) + "，固定头/尾=" +
                           std::to_string(m_headerH) + "/" + std::to_string(m_footerH);
        }
        result.status = est.status;
        return result;
    }
    ClearLastError();
    result.dy = est.dy;

    if (est.dy == 0) {
        // 画面无变化：边界/不可滚动（设计 11.6），非致命，不更新任何状态
        result.status = StitchStatus::NoChange;
        return result;
    }

    // 位移帧对：更新固定头/尾估计（min 收敛），必要时向存储补顶部行
    UpdateFixedRegions(est.dy);
    if (m_limitReached || m_lastErrorId != TextId::Count) {
        result.status = m_limitReached ? StitchStatus::LimitReached : StitchStatus::MatchFailed;
        return result;
    }
    if (!m_storeInitialized) {
        // 首个位移对：用当前估计把首帧内容带提交进存储（首帧暂存结束，设计 11.3）
        const PixelView firstFull{m_firstFrame.data(), m_frameW, m_frameH, m_frameW * 4};
        const AppendResult r = m_store->Initialize(BandOf(firstFull));
        if (r != AppendResult::Ok) {
            m_lastErrorId = TextId::StitchStorageFailed;
            m_lastError = m_store->GetLastErrorMessage();
            m_limitReached = (r == AppendResult::LimitExceeded);
            result.status = m_limitReached ? StitchStatus::LimitReached : StitchStatus::MatchFailed;
            return result;
        }
        m_storeInitialized = true;
        m_viewportY = 0;
    }

    // 偏移状态机（设计 11.1）。dy 是切片不变量（两帧同一切片），估计更新后
    // 重新切片不影响本步 dy
    m_viewportY += est.dy;
    const PixelView curBand = BandOf(curFull);
    const int bh = curBand.height;
    {
        const int newBottom = m_viewportY + bh - m_store->Height();
        if (newBottom > 0) {
            if (m_store->WouldExceedLimits(newBottom, m_headerH + m_footerH)) {
                m_lastErrorId = TextId::ScrollLimitReached;
                m_lastError = "追加后固定头尾将超过长图限制";
                m_limitReached = true;
                result.status = StitchStatus::LimitReached;
                return result;
            }
            const AppendResult r = m_store->AppendBottom(curBand, bh - newBottom, newBottom);
            if (r != AppendResult::Ok) {
                m_lastErrorId = TextId::StitchStorageFailed;
                m_lastError = m_store->GetLastErrorMessage();
                m_limitReached = (r == AppendResult::LimitExceeded);
                result.status = m_limitReached ? StitchStatus::LimitReached
                                               : StitchStatus::MatchFailed;
                return result;
            }
            result.appendedBottomRows = newBottom;
        }
        if (m_viewportY < 0) {
            // 向上超出：追加到顶部并重定基（top 归零，viewportY 同步平移）
            const int k = -m_viewportY;
            if (m_store->WouldExceedLimits(k, m_headerH + m_footerH)) {
                m_lastErrorId = TextId::ScrollLimitReached;
                m_lastError = "追加后固定头尾将超过长图限制";
                m_limitReached = true;
                result.status = StitchStatus::LimitReached;
                return result;
            }
            const AppendResult r = m_store->AppendTop(curBand, 0, k);
            if (r != AppendResult::Ok) {
                m_lastErrorId = TextId::StitchStorageFailed;
                m_lastError = m_store->GetLastErrorMessage();
                m_limitReached = (r == AppendResult::LimitExceeded);
                result.status = m_limitReached ? StitchStatus::LimitReached
                                               : StitchStatus::MatchFailed;
                return result;
            }
            result.appendedTopRows = k;
            m_viewportY = 0;
        }
    }

    // 周期校验：每 verifyInterval 帧或滚回已有区域（本帧无追加）时（设计 11.1）
    const bool scrolledBack = (result.appendedTopRows == 0 && result.appendedBottomRows == 0);
    // 单向采集始终追加新行，不执行滚回已有区域的偏移纠正。
    if (frame.allowedDirection == 0 &&
        (scrolledBack || (m_okFrames > 0 && m_okFrames % m_config.verifyInterval == 0))) {
        if (PeriodicVerify(curBand)) {
            result.viewportCorrected = true;
        }
    }
    m_okFrames++;

    // 更新成功帧拷贝（匹配基准与固定尾来源）
    const size_t rowBytes = static_cast<size_t>(m_frameW) * 4;
    for (int y = 0; y < m_frameH; ++y) {
        std::memcpy(m_prevFrame.data() + y * rowBytes, f.Row(y), rowBytes);
    }
    ComputeFeatures(m_curGray, m_headerH, m_frameH - m_footerH, m_curFeatures);
    m_prevGray.swap(m_curGray);
    std::swap(m_prevFeatures, m_curFeatures);
    result.status = StitchStatus::Ok;
    return result;
}

uint64_t ScrollCaptureStitcher::SignatureDistance(const RowSignature& a, const RowSignature& b) {
    uint64_t distance = 0;
    for (int i = 0; i < kSignatureSegments; ++i) {
        distance += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
    }
    return distance;
}

void ScrollCaptureStitcher::ComputeGray(const PixelView& frame, std::vector<uint8_t>& gray) const {
    gray.resize(static_cast<size_t>(frame.width) * frame.height);
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            gray[static_cast<size_t>(y) * frame.width + x] = static_cast<uint8_t>(Luma(frame.Row(y) + x * 4));
        }
    }
}

void ScrollCaptureStitcher::SelectMatchColumns() {
    const int usableW = std::clamp(m_frameW - m_config.matchRightMargin, 1, m_frameW);
    const auto unchanged = [&](int x) {
        for (int y = m_headerH; y < m_frameH - m_footerH; ++y) {
            const size_t index = static_cast<size_t>(y) * m_frameW + x;
            if (std::abs(int(m_prevGray[index]) - int(m_curGray[index])) > kLumaTol) return false;
        }
        return true;
    };
    int left = 0, right = usableW;
    while (left < right && unchanged(left)) ++left;
    while (right > left && unchanged(right - 1)) --right;
    const auto structured = [&](int begin, int end) {
        int samples = 0;
        const int height = m_frameH - m_headerH - m_footerH;
        // 只看内容带中部的静止结构，避免固定头尾把正文两侧留白伪装成侧栏。
        for (int part = 2; part <= 6; ++part) {
            const int y = m_headerH + (height - 1) * part / 8;
            for (int x = begin; x < end; ++x) {
                const size_t index = static_cast<size_t>(y) * m_frameW + x;
                const int l = m_curGray[index];
                if ((x + 1 < end && std::abs(l - int(m_curGray[index + 1])) > kLumaTol) ||
                    (y + 1 < m_frameH - m_footerH &&
                     std::abs(l - int(m_curGray[index + m_frameW])) > kLumaTol)) ++samples;
            }
        }
        return samples >= m_config.minEvidenceSamples;
    };
    // 每对帧临时排除边缘静止列，不永久认定纯色是侧栏，也不改变输出宽度。
    // 无变化时保留全宽；有变化时留出梯度边界，正文仍须通过独立位移验证。
    const int padding = std::max(1, usableW / 512);
    m_matchLeft = left < right && structured(0, left) ? std::max(0, left - padding) : 0;
    m_matchRight = left < right && structured(right, usableW) ? std::min(usableW, right + padding) : usableW;
}

void ScrollCaptureStitcher::ComputeFeatures(const std::vector<uint8_t>& gray, int beginY, int endY,
                                            MatchFeatures& features) const {
    if (features.beginY == beginY && features.endY == endY &&
        features.left == m_matchLeft && features.right == m_matchRight) return;
    features.beginY = beginY; features.endY = endY;
    features.left = m_matchLeft; features.right = m_matchRight;
    const int usableW = m_matchRight - m_matchLeft;
    const int step = std::max(1, usableW / 512);
    const int columns = (usableW + step - 1) / step;
    const int height = endY - beginY;
    features.columns = columns;
    features.gray.resize(static_cast<size_t>(height) * columns);
    features.mask.assign(features.gray.size(), 0);
    features.signatures.assign(height, RowSignature{});
    features.blank.assign(height, 1);
    for (int y = 0; y < height; ++y) for (int c = 0; c < columns; ++c) {
        features.gray[static_cast<size_t>(y) * columns + c] =
            gray[static_cast<size_t>(y + beginY) * m_frameW + m_matchLeft + c * step];
    }
    for (int y = 0; y < height; ++y) for (int c = 0; c < columns; ++c) {
        const size_t index = static_cast<size_t>(y) * columns + c;
        const int l = features.gray[index];
        const int segment = static_cast<int>(static_cast<int64_t>(c * step) * kSignatureSegments / usableW);
        features.signatures[y][segment] += static_cast<uint32_t>(l);
        // 边缘只采匹配区域内邻居，保留固定栏/留白的原有边界语义。
        const int gradient = std::max({
            c > 0 ? std::abs(l - int(features.gray[index - 1])) : 0,
            c + 1 < columns ? std::abs(l - int(features.gray[index + 1])) : 0,
            y > 0 ? std::abs(l - int(features.gray[index - columns])) : 0,
            y + 1 < height ? std::abs(l - int(features.gray[index + columns])) : 0});
        features.mask[index] = gradient > kLumaTol ? 2 : (gradient > kLumaTol / 2 ? 1 : 0);
        if (features.mask[index] == 2) features.blank[y] = 0;
    }
}

ScrollCaptureStitcher::ShiftEstimate ScrollCaptureStitcher::EstimateShift(
    const PixelView& prevBand, const PixelView& curBand, const FrameInput& input, bool fullSearch) {
    ShiftEstimate est;
    const int h = curBand.height;

    // 完全相同的画面优先判无变化，避免重复纹理被滚动先验误判为新内容。
    bool identical = true;
    for (int y = 0; y < h && identical; ++y) {
        identical = std::memcmp(prevBand.Row(y), curBand.Row(y),
                                static_cast<size_t>(curBand.width) * 4) == 0;
    }
    if (identical) {
        est.ok = true;
        est.dy = 0;
        est.confidence = 1.0f;
        return est;
    }

    ComputeFeatures(m_prevGray, m_headerH, m_headerH + h, m_prevFeatures);
    ComputeFeatures(m_curGray, m_headerH, m_headerH + h, m_curFeatures);
    m_stationaryRows.assign(h, 0);

    // 固定结构不提供滚动证据，少量独特内容仍进入后续候选验证。
    int effective = 0;
    bool changed = false;
    for (int y = 0; y < h; ++y) {
        const bool differs = RowDiffers(y + m_headerH);
        changed = changed || differs;
        m_stationaryRows[y] = !m_prevFeatures.blank[y] && !m_curFeatures.blank[y] &&
                             !differs;
        if (!m_curFeatures.blank[y] && !m_stationaryRows[y]) ++effective;
    }
    // 页面到边界时可能仍有容差内的采集噪声，不能因此误触发空白推进恢复。
    if (!changed) {
        est.ok = true;
        est.confidence = 1.0f;
        return est;
    }
    if (effective == 0) {
        m_lastErrorId = TextId::StitchInsufficientContent;
        m_lastError = "画面有效内容行不足（" + std::to_string(effective) + "/" +
                      std::to_string(h) + " 行）";
        est.status = StitchStatus::InsufficientInfo;
        return est;
    }

    // 搜索范围：先验窗口（Planner 的注入量×增益）或全范围；
    // 始终覆盖 dy=0，使注入未生效（页面到底）时报 NoChange 而非 MatchFailed（设计 11.6）。
    // 先验窗口带增益未标定冗余：增益初值 1.0，而注入"pixel"滚轮与捕获物理像素
    // 的口径在 Retina 上可能差 2x——窗口不含真值会让每步 MatchFailed（spike 实测教训）；
    // 增益收敛后窗口随先验量比例收紧
    int lo;
    int hi;
    if (input.hasPrior) {
        const int slack = std::max(
            m_config.priorWindowRows,
            static_cast<int>(std::llround(1.5 * static_cast<double>(std::abs(input.expectedShift)))));
        lo = input.expectedShift - slack;
        hi = input.expectedShift + slack;
    } else {
        lo = -m_config.searchWindowRows;
        hi = m_config.searchWindowRows;
    }
    const int maxAbs = input.maxShiftRows > 0
        ? std::min(h - m_config.minOverlapRows, input.maxShiftRows)
        : h - m_config.minOverlapRows;
    if (maxAbs <= 0) {
        m_lastErrorId = TextId::StitchOverlapTooSmall;
        m_lastError = "内容带高度不足最小重叠行数";
        est.status = StitchStatus::MatchFailed;
        return est;
    }
    if (fullSearch) {
        lo = -maxAbs;
        hi = maxAbs;
    }
    lo = std::max(std::min(lo, 0), -maxAbs);
    hi = std::min(std::max(hi, 0), maxAbs);
    // 单向会话只搜索该方向和零位移，兜底搜索也遵守同一约束。
    if (input.allowedDirection > 0) lo = 0;
    if (input.allowedDirection < 0) hi = 0;

    if (effective >= m_config.minEffectiveRows) {
        const auto anchors = AnchorCandidates(lo, hi);
        // 一个位移候选加零位移；多个竞争位移一律交回完整搜索进行消歧。
        if (anchors.size() == 2) {
            auto anchored = SelectCandidate(anchors, input, effective);
            if (anchored.ok && anchored.dy != 0 && anchored.confidence >= 0.99f &&
                anchored.evidenceRows >= m_config.minEffectiveRows &&
                !HasCompetingCandidate(anchored.dy, anchored.confidence, lo, hi)) {
                anchored.usedAnchorSearch = true;
                return anchored;
            }
            ClearLastError(); // 噪声、重复或稀疏锚点回退到原有完整搜索。
        }
    }

    // 1D 位移搜索：cur[y] ≈ prev[y+dy]，分段亮度差的总和作代价。
    // 各段先求差再相加，左右字符的亮度变化不会相互抵消。
    const int range = hi - lo + 1;
    std::vector<float> costs(range, 0.0f);
    for (int dy = lo; dy <= hi; ++dy) {
        const int yStart = dy > 0 ? 0 : -dy;
        const int yEnd = dy > 0 ? h - dy : h;
        uint64_t sum = 0;
        int evidenceRows = 0;
        for (int y = yStart; y < yEnd; ++y) {
            if (m_stationaryRows[y] || m_stationaryRows[y + dy]) continue;
            if (m_prevFeatures.blank[y + dy] && m_curFeatures.blank[y]) continue;
            sum += SignatureDistance(m_prevFeatures.signatures[y + dy], m_curFeatures.signatures[y]);
            ++evidenceRows;
        }
        costs[dy - lo] = evidenceRows > 0
            ? static_cast<float>(static_cast<double>(sum) / evidenceRows)
            : std::numeric_limits<float>::max();
    }

    // 候选收集：低于自适应阈值（2×中位）的局部极小 + 全局最小
    std::vector<float> sortedCosts(costs);
    const auto midIt = sortedCosts.begin() + static_cast<long>(sortedCosts.size() / 2);
    std::nth_element(sortedCosts.begin(), midIt, sortedCosts.end());
    const float threshold = 2.0f * (*midIt);
    int globalMinDy = lo;
    for (int dy = lo + 1; dy <= hi; ++dy) {
        if (costs[dy - lo] < costs[globalMinDy - lo]) globalMinDy = dy;
    }
    std::vector<int> candDys;
    for (int dy = lo; dy <= hi; ++dy) {
        const bool isLocalMin = (dy == lo || costs[dy - lo] <= costs[dy - 1 - lo]) &&
                                (dy == hi || costs[dy - lo] <= costs[dy + 1 - lo]);
        if ((isLocalMin && costs[dy - lo] <= threshold) || dy == globalMinDy) {
            candDys.push_back(dy);
        }
    }
    // 排序取前 K：代价升序；并列时优先靠近先验（重复纹理下保证真值邻域进入验证）
    std::sort(candDys.begin(), candDys.end(), [&](int a, int b) {
        const float ca = costs[a - lo];
        const float cb = costs[b - lo];
        if (ca != cb) return ca < cb;
        if (input.hasPrior) {
            const int da = std::abs(a - input.expectedShift);
            const int db = std::abs(b - input.expectedShift);
            if (da != db) return da < db;
        }
        return a < b;
    });
    if (static_cast<int>(candDys.size()) > kMaxCandidates) {
        candDys.resize(kMaxCandidates);
    }

    return SelectCandidate(candDys, input, effective);
}

std::vector<int> ScrollCaptureStitcher::AnchorCandidates(int lo, int hi) const {
    const int height = m_curFeatures.endY - m_curFeatures.beginY;
    std::vector<int> anchors;
    // 在不同纵向区间各选一个有结构且不与相邻行重复的锚点。
    for (int bucket = 0; bucket < 8; ++bucket) {
        for (int y = bucket * height / 8; y < (bucket + 1) * height / 8; ++y) {
            if (m_stationaryRows[y] || m_curFeatures.blank[y]) continue;
            const auto& sig = m_curFeatures.signatures[y];
            if ((y > 0 && sig == m_curFeatures.signatures[y - 1]) ||
                (y + 1 < height && sig == m_curFeatures.signatures[y + 1])) continue;
            anchors.push_back(y);
            break;
        }
    }
    if (anchors.size() < 3) return {};
    std::vector<int> candidates;
    int bestVotes = 0;
    for (int dy = lo; dy <= hi; ++dy) {
        int votes = 0;
        for (int y : anchors) {
            const int py = y + dy;
            if (py < 0 || py >= height || m_stationaryRows[py]) continue;
            if (m_curFeatures.signatures[y] == m_prevFeatures.signatures[py]) ++votes;
        }
        bestVotes = std::max(bestVotes, votes);
        if (votes >= 2) candidates.push_back(dy);
    }
    // 候选过多代表重复结构，不能截断竞争者后宣称唯一。
    if (bestVotes < 3 || candidates.size() > kMaxCandidates - 1) return {};
    if (std::find(candidates.begin(), candidates.end(), 0) == candidates.end()) candidates.push_back(0);
    return candidates;
}

bool ScrollCaptureStitcher::HasCompetingCandidate(int bestDy, float confidence, int lo, int hi) const {
    const int height = m_curFeatures.endY - m_curFeatures.beginY;
    const int columns = m_curFeatures.columns;
    const float threshold = confidence - m_config.ambiguityMargin;
    // 独特锚点仍可能位于大量重复正文中。对所有其余位移证明置信度上界
    // 低于歧义窗口，证明不足就回退完整搜索，不能只比较锚点命中的候选。
    for (int dy = lo; dy <= hi; ++dy) {
        if (dy == bestDy) continue;
        const int begin = std::max(0, -dy), end = std::min(height, height - dy);
        const auto hasEvidence = [&](int y) {
            return !m_stationaryRows[y] && !m_stationaryRows[y + dy] &&
                (!m_prevFeatures.blank[y + dy] || !m_curFeatures.blank[y]);
        };
        int rows = 0;
        for (int y = begin; y < end; ++y) if (hasEvidence(y)) ++rows;
        if (rows < m_config.minEvidenceRows) continue;
        int failed = 0;
        bool excluded = false;
        for (int y = begin; y < end; ++y) {
            if (!hasEvidence(y)) continue;
            int samples = 0, bad = 0;
            const size_t pr = static_cast<size_t>(y + dy) * columns;
            const size_t cr = static_cast<size_t>(y) * columns;
            for (int block = 0; block < columns; block += 64) {
                // 分组累加便于编译器向量化，同时保留行失败后的提前终止。
                for (int c = block; c < std::min(columns, block + 64); ++c) {
                    const auto pm = m_prevFeatures.mask[pr + c], cm = m_curFeatures.mask[cr + c];
                    const bool strong = pm >= 2 || cm >= 2;
                    samples += strong;
                    bad += strong && (!pm || !cm ||
                        std::abs(int(m_prevFeatures.gray[pr + c]) - int(m_curFeatures.gray[cr + c])) > kLumaTol);
                }
                // 总有效样本至多 columns；超过这个失败预算后该行已不可能匹配。
                if (bad * 10 > columns) break;
            }
            if (bad * 10 > samples) ++failed;
            const float upper = static_cast<float>(rows - failed) / rows;
            if (upper + 1e-6f < threshold) { excluded = true; break; }
        }
        if (!excluded) return true;
    }
    return false;
}

ScrollCaptureStitcher::ShiftEstimate ScrollCaptureStitcher::SelectCandidate(
    const std::vector<int>& candDys, const FrameInput& input, int effective) {
    ShiftEstimate est;
    const int h = m_curFeatures.endY - m_curFeatures.beginY;
    // 验证两帧有效采样点的并集，背景相似不能掩盖文字或边界缺失。
    std::vector<Candidate> cands;
    cands.reserve(candDys.size());
    const auto enoughEvidence = [&](const MatchEvidence& evidence) {
        return evidence.samples >= m_config.minEvidenceSamples &&
               evidence.rows >= m_config.minEvidenceRows;
    };
    for (const int dy : candDys) {
        if (h - std::abs(dy) < m_config.minOverlapRows) continue;
        const auto evidence = VerifyCandidate(m_prevFeatures, m_curFeatures, dy, true);
        if (!enoughEvidence(evidence)) continue;
        cands.push_back({dy, evidence});
    }
    if (cands.empty()) {
        m_lastErrorId = TextId::StitchInsufficientEvidence;
        m_lastError = "候选重叠区有效证据不足";
        est.status = StitchStatus::InsufficientInfo;
        return est;
    }
    const auto byRatio = [](const Candidate& a, const Candidate& b) {
        if (a.evidence.matchRatio != b.evidence.matchRatio) {
            return a.evidence.matchRatio > b.evidence.matchRatio;
        }
        return a.dy < b.dy;
    };
    std::sort(cands.begin(), cands.end(), byRatio);

    // 不稳定帧/漂移风险后使用更严格置信度（设计 10.4 / 11.1）
    const bool strict = !input.stable || m_driftRisk;
    const float minConf = strict ? m_config.strictConfidence : m_config.minConfidence;
    if (cands[0].evidence.matchRatio < minConf) {
        // 悬浮滚动条、悬浮按钮等窄竖条叠加层会让稀疏行整行验证失败。常规验证均不足时，
        // 每个候选按同一规则排除自身的局部失配竖条后重算，阈值和歧义判定保持不变。
        for (Candidate& c : cands) {
            const auto excluded = LocalMismatchColumns(c.dy);
            if (excluded.empty()) continue;
            const auto evidence = VerifyCandidate(m_prevFeatures, m_curFeatures, c.dy, true, &excluded);
            if (enoughEvidence(evidence) && evidence.matchRatio > c.evidence.matchRatio) {
                c.evidence = evidence;
                c.columnsExcluded = true;
            }
        }
        std::sort(cands.begin(), cands.end(), byRatio);
    }
    est.confidence = cands[0].evidence.matchRatio;
    est.evidenceRows = cands[0].evidence.rows;
    est.usedColumnExclusion = cands[0].columnsExcluded;
    if (cands[0].evidence.matchRatio < minConf && UnchangedOutsideNarrowColumns()) {
        // 页面静止、仅悬浮滚动条淡出等窄竖条变化：判为无变化，避免失败后锁死输入。
        est = {};
        est.ok = true;
        est.confidence = 1.0f;
        est.usedColumnExclusion = true;
        return est;
    }
    if (cands[0].evidence.matchRatio < minConf) {
        m_lastErrorId = TextId::StitchLowConfidence;
        m_lastError = "最佳候选置信度不足";
        est.status = StitchStatus::MatchFailed;
        return est;
    }

    // 稀疏证据有歧义时禁止靠注入先验决定留白长度；密集纹理保留现有消歧规则。
    if (cands.size() > 1 &&
        cands[0].evidence.matchRatio - cands[1].evidence.matchRatio < m_config.ambiguityMargin) {
        if (input.hasPrior && effective >= m_config.minEffectiveRows &&
            cands[0].evidence.rows >= m_config.minEffectiveRows) {
            int bestDy = 0;
            int bestDist = INT_MAX;
            int tiedCount = 0;
            float bestRatio = 0.0f;
            bool bestExcluded = false;
            for (const Candidate& c : cands) {
                if (c.evidence.matchRatio < cands[0].evidence.matchRatio - m_config.ambiguityMargin) continue;
                if (c.evidence.matchRatio < minConf || c.evidence.rows < m_config.minEffectiveRows) continue;
                const int dist = std::abs(c.dy - input.expectedShift);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestDy = c.dy;
                    bestRatio = c.evidence.matchRatio;
                    bestExcluded = c.columnsExcluded;
                    tiedCount = 1;
                } else if (dist == bestDist) {
                    ++tiedCount;
                }
            }
            if (tiedCount == 1) {
                est.ok = true;
                est.dy = bestDy;
                est.confidence = bestRatio;
                est.usedColumnExclusion = bestExcluded;
                return est;
            }
        }
        est.status = StitchStatus::Ambiguous;
        m_lastErrorId = TextId::StitchAmbiguous;
        m_lastError = "重复纹理导致位移歧义";
        return est;
    }

    est.ok = true;
    est.dy = cands[0].dy;
    est.confidence = cands[0].evidence.matchRatio;
    return est;
}

std::vector<uint8_t> ScrollCaptureStitcher::LocalMismatchColumns(int dy) const {
    const int h = m_curFeatures.endY - m_curFeatures.beginY;
    const int columns = m_curFeatures.columns;
    const int yStart = dy > 0 ? 0 : -dy;
    const int yEnd = dy > 0 ? h - dy : h;
    std::vector<int> samples(columns, 0), bad(columns, 0);
    for (int y = yStart; y < yEnd; ++y) {
        if (m_stationaryRows[y] || m_stationaryRows[y + dy]) continue;
        const size_t pr = static_cast<size_t>(y + dy) * columns;
        const size_t cr = static_cast<size_t>(y) * columns;
        for (int c = 0; c < columns; ++c) {
            const uint8_t pm = m_prevFeatures.mask[pr + c], cm = m_curFeatures.mask[cr + c];
            if (pm < 2 && cm < 2) continue;
            ++samples[c];
            bad[c] += !pm || !cm ||
                std::abs(int(m_prevFeatures.gray[pr + c]) - int(m_curFeatures.gray[cr + c])) > kLumaTol;
        }
    }
    // 至少三分之一样本失配的列视为叠加层；两侧各扩 2 列覆盖抗锯齿边缘。
    constexpr int kPad = 2;
    std::vector<uint8_t> excluded(columns, 0);
    for (int c = 0; c < columns; ++c) {
        if (bad[c] < 2 || bad[c] * 3 < samples[c]) continue;
        for (int k = std::max(0, c - kPad); k <= std::min(columns - 1, c + kPad); ++k) excluded[k] = 1;
    }
    int64_t total = 0, removed = 0;
    int width = 0;
    for (int c = 0; c < columns; ++c) {
        total += samples[c];
        if (!excluded[c]) continue;
        ++width;
        removed += samples[c];
    }
    // 错误位移的失配遍布正文；只有窄且只占少数证据的竖条才可排除，
    // 否则可能删掉唯一的真实内容后靠残余背景通过验证。
    if (width == 0 || width > columns * m_config.maxExcludedColumnRatio ||
        removed > total * m_config.maxExcludedSampleRatio) return {};
    return excluded;
}

bool ScrollCaptureStitcher::UnchangedOutsideNarrowColumns() const {
    // 零位移时差异行全部落在叠加层内，无法用样本占比区分，改为直接比较两帧灰度：
    // 差异只能出现在极窄的竖条内，其余列逐点一致。窄内容列真实滚动也会触发，
    // 但判为无变化不追加像素，下一帧仍与同一基准匹配。
    // 边缘静止列裁剪可能把匹配区收窄到变化区本身，必须按内容带全宽统计。
    std::vector<uint8_t> differs(m_frameW, 0);
    for (int y = m_headerH; y < m_frameH - m_footerH; ++y) {
        const size_t row = static_cast<size_t>(y) * m_frameW;
        for (int x = 0; x < m_frameW; ++x) {
            differs[x] |= std::abs(int(m_prevGray[row + x]) - int(m_curGray[row + x])) > kLumaTol;
        }
    }
    const auto width = std::count(differs.begin(), differs.end(), uint8_t{1});
    if (width == 0 || width > m_frameW * m_config.maxStaticOverlayColumnRatio) return false;
    // 竖条外必须有足够的静止结构证明页面没动；纯背景无法区分"未滚动"与"窄内容已换"，
    // 后者若判为无变化，连续输入会继续滚动并静默跳过内容。
    int structuredRows = 0;
    for (int y = m_headerH; y < m_frameH - m_footerH; ++y) {
        const size_t row = static_cast<size_t>(y) * m_frameW;
        for (int x = 0; x + 1 < m_frameW; ++x) {
            if (differs[x] || differs[x + 1]) continue;
            if (std::abs(int(m_curGray[row + x]) - int(m_curGray[row + x + 1])) > kLumaTol) {
                ++structuredRows;
                break;
            }
        }
    }
    return structuredRows >= m_config.minEffectiveRows;
}

ScrollCaptureStitcher::MatchEvidence ScrollCaptureStitcher::VerifyCandidate(
    const MatchFeatures& prev, const MatchFeatures& cur, int dy, bool ignoreStationary,
    const std::vector<uint8_t>* excludedColumns) const {
    const int h = cur.endY - cur.beginY;
    const int columns = cur.columns;
    const int yStart = dy > 0 ? 0 : -dy;
    const int yEnd = dy > 0 ? h - dy : h;
    int matched = 0;
    MatchEvidence evidence;
    for (int y = yStart; y < yEnd; ++y) {
        if (ignoreStationary && (m_stationaryRows[y] || m_stationaryRows[y + dy])) continue;
        int okSamples = 0, samples = 0;
        for (int c = 0; c < columns; ++c) {
            if (excludedColumns && (*excludedColumns)[c]) continue;
            const size_t pi = static_cast<size_t>(y + dy) * columns + c;
            const size_t ci = static_cast<size_t>(y) * columns + c;
            const uint8_t prevInfo = prev.mask[pi], curInfo = cur.mask[ci];
            const bool strong = prevInfo >= 2 || curInfo >= 2;
            samples += strong;
            okSamples += strong && prevInfo && curInfo &&
                std::abs(int(prev.gray[pi]) - int(cur.gray[ci])) <= kLumaTol;
        }
        if (samples == 0) continue;
        ++evidence.rows;
        evidence.samples += samples;
        if (okSamples * 10 >= samples * 9) ++matched;
    }
    evidence.matchRatio = evidence.rows > 0 ? static_cast<float>(matched) / evidence.rows : 0.0f;
    return evidence;
}

bool ScrollCaptureStitcher::RowDiffers(int y) const {
    const int step = std::max(1, (m_matchRight - m_matchLeft) / 512);
    for (int x = m_matchLeft; x < m_matchRight; x += step) {
        const size_t index = static_cast<size_t>(y) * m_frameW + x;
        if (std::abs(int(m_prevGray[index]) - int(m_curGray[index])) > kLumaTol) return true;
    }
    return false;
}

void ScrollCaptureStitcher::UpdateFixedRegions(int dy) {
    // 自顶向下第一处差异行之前为固定头候选；自底向上同理为固定尾候选（设计 11.3）
    int firstDiff = -1;
    int lastDiff = -1;
    for (int y = 0; y < m_frameH; ++y) {
        if (RowDiffers(y)) {
            if (firstDiff < 0) firstDiff = y;
            lastDiff = y;
        }
    }
    if (firstDiff < 0) return;  // 全帧一致：非位移对（调用方已确认 dy≠0，防御性返回）
    // 固定栏使用全帧特征，不覆盖已缓存的成功内容带特征。
    MatchFeatures fullPrev, fullCur;
    const bool whole = m_headerH == 0 && m_footerH == 0;
    if (!whole) {
        ComputeFeatures(m_prevGray, 0, m_frameH, fullPrev);
        ComputeFeatures(m_curGray, 0, m_frameH, fullCur);
    }
    const auto& prevInfo = whole ? m_prevFeatures : fullPrev;
    const auto& curInfo = whole ? m_curFeatures : fullCur;
    const int usableW = m_matchRight - m_matchLeft;
    const int step = std::max(1, usableW / 512);
    const int columns = (usableW + step - 1) / step;
    const auto confirmedExtent = [&](int begin, int end, bool header) {
        int staticSamples = 0;
        int scrollingSamples = 0;
        int extent = 0;
        for (int y = begin; y < end; ++y) {
            int rowSamples = 0;
            int minLuma = 255;
            int maxLuma = 0;
            for (int x = 0; x < usableW; x += step) {
                const int luma = m_curGray[static_cast<size_t>(y) * m_frameW + m_matchLeft + x];
                minLuma = std::min(minLuma, luma);
                maxLuma = std::max(maxLuma, luma);
                const size_t index = static_cast<size_t>(y) * columns + x / step;
                if (prevInfo.mask[index] < 2 || curInfo.mask[index] < 2) continue;
                ++rowSamples;
                const int py = y + dy;
                if (py >= 0 && py < m_frameH &&
                    prevInfo.mask[static_cast<size_t>(py) * columns + x / step] &&
                    std::abs(int(m_prevGray[static_cast<size_t>(py) * m_frameW + m_matchLeft + x]) - luma) <= kLumaTol) {
                    ++scrollingSamples;
                }
            }
            staticSamples += rowSamples;
            // 相邻固定栏会让内容侧的纯色行也产生纵向梯度，不能因此多裁一行内容。
            if (rowSamples > 0 && maxLuma - minLuma > kLumaTol) {
                extent = header ? y + 1 : std::max(extent, m_frameH - y);
            }
        }
        // 纯色不提供静止证据；结构在内容位移下仍吻合时也不能确认固定。
        return staticSamples >= m_config.minEvidenceSamples &&
                       scrollingSamples * 2 < staticSamples ? extent : 0;
    };
    const int pairHeader = confirmedExtent(0, firstDiff, true);
    const int pairFooter = confirmedExtent(lastDiff + 1, m_frameH, false);
    ++m_displacedPairs;
    // min 收敛：某行只要在任一位移对中变化即为内容；估计只收缩、单调不增，
    // 永远不需要从存储中删除已提交行
    const int newHeader =
        (m_displacedPairs == 1) ? pairHeader : std::min(m_headerH, pairHeader);
    const int newFooter =
        (m_displacedPairs == 1) ? pairFooter : std::min(m_footerH, pairFooter);

    const int dHeader = m_headerH - newHeader;  // 估计收缩量（仅首对之后可能 >0）
    if (dHeader > 0 && m_storeInitialized) {
        // 收缩暴露出的首帧行属于顶部缺失内容（页面静态，首帧见过这些行）：
        // 前插首帧对应行；重定基恰好抵消切片映射偏移，viewportY 不变
        const PixelView firstFull{m_firstFrame.data(), m_frameW, m_frameH, m_frameW * 4};
        if (m_store->WouldExceedLimits(dHeader, newHeader + m_footerH)) {
            m_lastErrorId = TextId::ScrollLimitReached;
            m_lastError = "固定区域调整后将超过长图限制";
            m_limitReached = true;
            return;
        }
        const AppendResult r = m_store->AppendTop(firstFull, newHeader, dHeader);
        if (r != AppendResult::Ok) {
            m_lastErrorId = TextId::StitchStorageFailed;
            m_lastError = m_store->GetLastErrorMessage();
            if (r == AppendResult::LimitExceeded) m_limitReached = true;
        }
    }
    m_headerH = newHeader;
    m_footerH = newFooter;
    // 底部收缩无需补行：本帧内容带随之变宽，偏移状态机自然追加超出行
}

bool ScrollCaptureStitcher::PeriodicVerify(const PixelView& curBand) {
    const int bh = curBand.height;
    if (!m_storeInitialized || bh <= 0) return false;
    if (m_viewportY < 0 || m_viewportY + bh > m_store->Height()) return false;

    const size_t rowBytes = static_cast<size_t>(m_frameW) * 4;
    m_verifyBuf.assign(static_cast<size_t>(bh) * rowBytes, 0);
    if (!m_store->ReadRows(m_viewportY, bh, m_verifyBuf.data(), static_cast<int>(rowBytes))) {
        m_driftRisk = true;  // 不应发生；标记漂移以收紧后续置信度
        return false;
    }
    const PixelView storeBand{m_verifyBuf.data(), m_frameW, bh, static_cast<int>(rowBytes)};
    std::vector<uint8_t> storeGray;
    ComputeGray(storeBand, storeGray);
    MatchFeatures stored, current;
    ComputeFeatures(storeGray, 0, bh, stored);
    ComputeFeatures(m_curGray, m_headerH, m_headerH + bh, current);

    // ±3px 小窗搜索：cur[y] ≈ store[y+d]，则正确 viewportY = 当前 + d（设计 11.1：
    // dy 逐帧累加会累积 ±1/±2 误差，周期校验抑制）
    int bestDy = 0;
    double bestCost = -1.0;
    for (int dy = -3; dy <= 3; ++dy) {
        const int yStart = dy > 0 ? 0 : -dy;
        const int yEnd = dy > 0 ? bh - dy : bh;
        uint64_t sum = 0;
        for (int y = yStart; y < yEnd; ++y) {
            sum += SignatureDistance(stored.signatures[y + dy], current.signatures[y]);
        }
        const double cost = static_cast<double>(sum) / (yEnd - yStart);
        if (bestCost < 0.0 || cost < bestCost) {
            bestCost = cost;
            bestDy = dy;
        }
    }
    const float minConf = m_driftRisk ? m_config.strictConfidence : m_config.minConfidence;
    const auto evidence = VerifyCandidate(stored, current, bestDy);
    if (evidence.matchRatio >= minConf && evidence.samples >= m_config.minEvidenceSamples &&
        evidence.rows >= m_config.minEvidenceRows) {
        if (bestDy != 0) m_viewportY += bestDy;
        return bestDy != 0;
    }
    m_driftRisk = true;  // 不一致：记录漂移风险
    return false;
}

bool ScrollCaptureStitcher::ExportFinalImage(std::vector<uint8_t>& out, int& outWidth,
                                             int& outHeight) {
    ClearLastError();
    if (!m_started) {
        m_lastErrorId = TextId::StitchNotStarted;
        m_lastError = "会话未开始";
        return false;
    }
    if (!m_storeInitialized) {
        // 全程无位移帧对：无固定区域检测信息（头/尾估计为 0），整帧作为内容提交
        const PixelView firstFull{m_firstFrame.data(), m_frameW, m_frameH, m_frameW * 4};
        if (m_store->Initialize(firstFull) != AppendResult::Ok) {
            m_lastErrorId = TextId::StitchStorageFailed;
            m_lastError = m_store->GetLastErrorMessage();
            return false;
        }
        m_storeInitialized = true;
        m_viewportY = 0;
    }
    const size_t rowBytes = static_cast<size_t>(m_frameW) * 4;
    // 固定头取首帧、固定尾取最新成功帧（设计 11.3），各出现一次
    const PixelView header{m_firstFrame.data(), m_frameW, m_headerH, static_cast<int>(rowBytes)};
    const PixelView footer{m_prevFrame.data() + static_cast<size_t>(m_frameH - m_footerH) * rowBytes,
                           m_frameW, m_footerH, static_cast<int>(rowBytes)};
    if (!ComposeFinalImage(*m_store, header, footer, out, outWidth, outHeight)) {
        m_lastErrorId = TextId::StitchStorageFailed;
        m_lastError = "长图合成失败";
        return false;
    }
    return true;
}

}  // namespace shotlet
