#pragma once
#include "i18n/Localization.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scrollcapture/LongImageStore.h"
#include "scrollcapture/ScrollCaptureTypes.h"

namespace shotlet {

// 每帧输入：稳定帧 + 滚动先验
struct FrameInput {
    PixelView pixels;       // 稳定帧（尺寸须与首帧一致）
    int expectedShift = 0;  // 先验：注入量×增益的预期 dy（向下为正）
    bool hasPrior = false;  // 是否携带先验（expectedShift==0 也可能是真实先验）
    bool stable = true;     // 稳定帧判定结果；false → 更严格置信度（设计 10.4）
    int allowedDirection = 0; // 会话方向：1 向下，-1 向上，0 不限制
    int maxShiftRows = 0; // 连续模式限制搜索位移，0 使用原有重叠下限
};

// 步骤状态（Controller 按 14 节区分致命/非致命与文案）
enum class StitchStatus {
    Ok,               // dy 可信，长图已按需更新
    NoChange,         // dy=0：边界/不可滚动（非致命，设计 11.6）
    Ambiguous,        // 候选差距不足且先验无法消歧（可恢复）
    InsufficientInfo, // 有效行不足（可恢复：原地重新取帧）
    MatchFailed,      // 重叠不足/验证不过（进恢复流程）
    LimitReached,     // 存储追加上限触发，保留结果（非致命）
};

struct StitchResult {
    StitchStatus status = StitchStatus::Ok;
    int dy = 0;                     // 本帧相对上一帧的位移（供 Planner 标定增益）
    float confidence = 0.0f;        // 重叠区有效证据的匹配行比例 [0,1]
    int appendedTopRows = 0;
    int appendedBottomRows = 0;
    bool usedFallbackSearch = false; // 先验窗口失败后使用全重叠范围，不能用于本步增益标定
    bool viewportCorrected = false; // 周期校验修正了累积误差
    bool usedAnchorSearch = false; // 多个独立锚点提供候选且通过严格验证
    bool usedColumnExclusion = false; // 排除局部失配竖条（悬浮滚动条/按钮）后才通过验证
};

// 阈值集中配置：设计文档中均为"建议初值，spike 校准"，必然要调；测试需要小上限/小间隔
struct StitcherConfig {
    int minOverlapRows = 200;        // 重叠下限，低于 → MatchFailed
    float minConfidence = 0.60f;     // 低于 → 不采用
    float strictConfidence = 0.75f;  // 不稳定帧/漂移风险后的更严格阈值
    float ambiguityMargin = 0.08f;   // 最优−次优 < 此值 → 歧义
    int minEffectiveRows = 60;       // 低于此值进入稀疏匹配，禁止仅用先验消歧
    int verifyInterval = 8;          // 周期校验间隔（帧）
    int priorWindowRows = 250;       // 有先验时搜索范围 ±
    int searchWindowRows = 700;      // 无先验时搜索范围 ±
    int tileHeight = 1024;
    int64_t maxImageHeight = ScrollCaptureLimits::kMaxImageHeight;
    int64_t maxRgbaBytes = ScrollCaptureLimits::kMaxRgbaBytes;
    int matchRightMargin = 0;        // 右侧常驻滚动条排除宽度
    int minEvidenceSamples = 16;     // 候选重叠区的最小有效采样点数
    int minEvidenceRows = 2;         // 候选重叠区的最小有效行数
    float maxExcludedColumnRatio = 0.15f; // 可排除的局部失配竖条总宽占匹配宽度上限
    float maxExcludedSampleRatio = 0.5f;  // 可排除竖条的有效样本占重叠区样本上限
    float maxStaticOverlayColumnRatio = 0.04f; // 静止画面仅此宽度内变化（滚动条淡出）→ 无变化
};

// 视口偏移跟踪拼接器（设计文档 11.1）
// 坐标约定：长图（内容带）纵轴向下，覆盖 [0, storeHeight)；m_viewportY 为当前帧
// 内容带顶边的长图坐标（≥0）。向下滚动 dy>0。
class ScrollCaptureStitcher {
public:
    explicit ScrollCaptureStitcher(const StitcherConfig& config = {});

    // 会话开始：首帧暂存（固定区域检测完成前不提交，设计 11.3）
    bool StartSession(const PixelView& firstFrame);

    // 处理一个稳定帧：估计 dy → 更新固定区域估计 → 偏移状态机追加
    StitchResult ProcessFrame(const FrameInput& frame);

    // 存储访问（预览/Controller/测试用；StartSession 前为 null）
    [[nodiscard]] const LongImageStore* GetStore() const { return m_store.get(); }

    [[nodiscard]] int ViewportY() const { return m_viewportY; }
    [[nodiscard]] int FixedHeaderHeight() const { return m_headerH; }
    [[nodiscard]] int FixedFooterHeight() const { return m_footerH; }

    // 最终导出：固定头（首帧）+ 内容带 + 固定尾（最新成功帧），各出现一次
    bool ExportFinalImage(std::vector<uint8_t>& out, int& outWidth, int& outHeight);

    // 最近一次公开操作的错误；无错误时 ID 为 Count，文字为空。
    [[nodiscard]] TextId GetLastErrorId() const { return m_lastErrorId; }
    [[nodiscard]] const std::string& GetLastErrorMessage() const { return m_lastError; }

private:
    void ClearLastError() {
        m_lastError.clear();
        m_lastErrorId = TextId::Count;
    }

    struct ShiftEstimate {
        bool ok = false;
        StitchStatus status = StitchStatus::Ok;
        int dy = 0;
        float confidence = 0.0f;
        bool usedAnchorSearch = false;
        bool usedColumnExclusion = false;
        int evidenceRows = 0;
    };
    struct MatchEvidence {
        float matchRatio = 0.0f;
        int rows = 0;
        int samples = 0;
    };
    struct Candidate {
        int dy = 0;
        MatchEvidence evidence;
        bool columnsExcluded = false;
    };

    // 分段保留字符的横向位置，避免等宽终端中不同文字行的亮度总和相同。
    static constexpr int kSignatureSegments = 32;
    using RowSignature = std::array<uint32_t, kSignatureSegments>;
    struct MatchFeatures {
        int beginY = -1, endY = 0, left = 0, right = 0, columns = 0;
        std::vector<uint8_t> gray, blank, mask;
        std::vector<RowSignature> signatures;
    };
    [[nodiscard]] static uint64_t SignatureDistance(const RowSignature& a, const RowSignature& b);

    // 位移估计三级管线（设计 11.2）：行签名 → 1D 搜索 → 像素验证
    ShiftEstimate EstimateShift(const PixelView& prevBand, const PixelView& curBand,
                                const FrameInput& input, bool fullSearch = false);
    void ComputeGray(const PixelView& frame, std::vector<uint8_t>& gray) const;
    void SelectMatchColumns();
    void ComputeFeatures(const std::vector<uint8_t>& gray, int beginY, int endY,
                         MatchFeatures& features) const;
    [[nodiscard]] ShiftEstimate SelectCandidate(const std::vector<int>& candidates,
                                                const FrameInput& input, int effective);
    [[nodiscard]] std::vector<int> AnchorCandidates(int lo, int hi) const;
    [[nodiscard]] bool HasCompetingCandidate(int bestDy, float confidence, int lo, int hi) const;
    [[nodiscard]] MatchEvidence VerifyCandidate(const MatchFeatures& prev, const MatchFeatures& cur,
                                               int dy, bool ignoreStationary = false,
                                               const std::vector<uint8_t>* excludedColumns = nullptr) const;
    // 候选位移下失配集中的窄竖条（特征列掩码）；不满足宽度/样本占比上限时返回空。
    [[nodiscard]] std::vector<uint8_t> LocalMismatchColumns(int dy) const;
    // 零位移下除窄竖条外两帧一致（悬浮滚动条淡出等），可安全判为无变化。
    [[nodiscard]] bool UnchangedOutsideNarrowColumns() const;

    // 用缓存的全帧行差异更新固定头/尾估计（min 收敛）并应用存储调整。
    // 仅在位移帧对上调用。
    void UpdateFixedRegions(int dy);
    [[nodiscard]] bool RowDiffers(int y) const;

    // 周期校验：当前内容带与存储对应区域 ±3px 搜索，修正累积误差（设计 11.1）
    bool PeriodicVerify(const PixelView& curBand);

    // 帧的内容带切片（用当前头/尾估计）；固定区域未知时使用全帧。
    [[nodiscard]] PixelView BandOf(const PixelView& frame) const;
    [[nodiscard]] int BandHeight() const { return m_frameH - m_headerH - m_footerH; }

    StitcherConfig m_config;
    int m_frameW = 0;
    int m_frameH = 0;
    bool m_started = false;
    bool m_storeInitialized = false;  // 首个位移帧对出现后内容带才提交
    bool m_limitReached = false;
    bool m_driftRisk = false;         // 周期校验不一致 → 提高后续匹配置信度
    int m_viewportY = 0;
    int m_headerH = 0;                // 已确认固定头（仅收缩，无判别力的留白保留为内容）
    int m_footerH = 0;                // 已确认固定尾（仅收缩，无判别力的留白保留为内容）
    int m_okFrames = 0;               // 周期校验计数
    int m_displacedPairs = 0;

    std::unique_ptr<LongImageStore> m_store;  // StartSession 时按帧宽创建
    std::vector<uint8_t> m_firstFrame;        // 首帧全帧紧凑拷贝
    std::vector<uint8_t> m_prevFrame;         // 上一成功帧拷贝（固定尾取自它）
    // 灰度与特征跟随最后成功帧；失败帧不替换基准。匹配区域改变时重新构建特征。
    std::vector<uint8_t> m_prevGray, m_curGray;
    MatchFeatures m_prevFeatures, m_curFeatures;
    int m_matchLeft = 0, m_matchRight = 0; // 仅影响匹配，输出仍保留选区全宽
    std::vector<uint8_t> m_stationaryRows;     // 同屏位置不变的有结构行，不参与滚动位移评分
    std::vector<uint8_t> m_verifyBuf;         // 周期校验行读取缓冲
    TextId m_lastErrorId = TextId::Count;
    std::string m_lastError;
};

}  // namespace shotlet
