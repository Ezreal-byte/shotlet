#pragma once

#include <cstdint>

namespace shotlet {

// 滚动规划配置（阈值均为设计文档 10.3 的"建议初值"，spike 校准）
struct PlannerConfig {
    double stepOverlapRatio = 0.6;   // 单步预期位移 / 视口高（保证相邻帧 ≥30% 重叠）
    double probeStepRatio = 0.3;     // 增益未标定前的探测步长比例（更保守，设计 10.3 探测步）
    int probeStepCount = 3;          // 探测步数
    double initialGain = 1.0;        // 增益初值（dy 像素 / 注入像素）
    double minGain = 0.2;
    double maxGain = 5.0;
    double gainAlpha = 0.3;          // 增益 EMA 平滑系数
    double failStepScale = 0.7;      // 匹配失败后步长收缩系数
    double recoverStepScale = 1.1;   // 成功后步长恢复系数
    double minStepScale = 0.2;       // 步长收缩下限
    int pendingCapHeights = 3;       // pending 上限 = 视口高 × N（防惯性无限积压）
};

// 滚动规划器（设计 10.3）：滚轮 delta 与页面真实位移非线性，
// 规划器只做意图累积/拆步/增益标定/串行化；真实位移一律由图像匹配得出，
// 增益仅用于反推注入量与提供匹配先验。
class ScrollPlanner {
public:
    // 一步执行计划
    struct Step {
        int64_t injectPixels = 0;  // 注入量（pixel，向下为正）
        int64_t expectedShift = 0; // 先验 dy（injectPixels × 增益）
    };

    explicit ScrollPlanner(const PlannerConfig& config = {});

    // 视口高（选区高，像素）；<=0 视为未配置，HasStep 恒为 false
    void SetViewportHeight(int height);

    // 累积滚动意图（pixel，向下为正）。同向累积、反向净量合并；
    // 超出 pending 上限截断（触控板 momentum 防无限积压，设计 10.2）
    void AddIntent(int64_t delta);

    // 是否有可执行步骤：无 in-flight 且 pending 非零
    [[nodiscard]] bool HasStep() const;

    // 取一步开始执行（in-flight 置位，pending 相应扣减）。
    // 无可执行步骤时返回 {0,0} 且不置位（fail loud 由调用方断言 HasStep）
    Step TakeStep();

    // 步骤完成：dy 为本步实际位移（图像匹配结果），injectPixels 为该步注入量。
    // matched=false（重叠不足/歧义）→ 下调步长上限，下一步更保守
    // （设计 10.3"位移明显大于预期进恢复流程"的保守化实现）
    // 不稳定、重试或全范围兜底的结果可以拼接，但无法可靠归因于本次注入。
    void CompleteStep(int64_t dy, int64_t injectPixels, bool matched, bool calibrate = true);

    // 会话终止/失败重置（in-flight 与 pending 清空）
    void Reset();

    [[nodiscard]] int64_t Pending() const { return m_pending; }
    [[nodiscard]] double Gain() const { return m_gain; }
    [[nodiscard]] bool InFlight() const { return m_inFlight; }

private:
    // 有效增益 = EMA 收敛值，受 [minGain, maxGain] 夹取
    [[nodiscard]] double EffectiveGain() const;
    // 当前步长目标（像素）：视口高 × 步长比例 × 失败收缩系数
    [[nodiscard]] int64_t StepTargetRows() const;

    PlannerConfig m_config;
    int m_viewportH = 0;
    int64_t m_pending = 0;
    bool m_inFlight = false;
    double m_gain;
    double m_stepScale = 1.0;  // 失败收缩 / 成功恢复的步长系数
    int m_completedSteps = 0;
};

}  // namespace shotlet
