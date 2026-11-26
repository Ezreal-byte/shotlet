#include "scrollcapture/ScrollPlanner.h"

#include <algorithm>
#include <cmath>

namespace shotlet {

ScrollPlanner::ScrollPlanner(const PlannerConfig& config)
    : m_config(config), m_gain(config.initialGain) {}

void ScrollPlanner::SetViewportHeight(int height) {
    m_viewportH = height;
}

void ScrollPlanner::AddIntent(int64_t delta) {
    if (m_viewportH <= 0) {
        return;  // 未配置视口：tap 不应喂入（防御，意图丢弃）
    }
    m_pending += delta;
    // 反向净量合并 + 上限截断（取上限时保留方向）
    const int64_t cap =
        static_cast<int64_t>(m_viewportH) * m_config.pendingCapHeights;
    m_pending = std::clamp(m_pending, -cap, cap);
}

bool ScrollPlanner::HasStep() const {
    return m_viewportH > 0 && !m_inFlight && m_pending != 0;
}

double ScrollPlanner::EffectiveGain() const {
    return std::clamp(m_gain, m_config.minGain, m_config.maxGain);
}

int64_t ScrollPlanner::StepTargetRows() const {
    const double ratio = (m_completedSteps < m_config.probeStepCount)
                             ? m_config.probeStepRatio  // 探测步：小步标定增益
                             : m_config.stepOverlapRatio;
    const double rows = m_viewportH * ratio * m_stepScale;
    return std::max<int64_t>(1, static_cast<int64_t>(std::llround(rows)));
}

ScrollPlanner::Step ScrollPlanner::TakeStep() {
    if (!HasStep()) {
        return {};
    }
    const int64_t dir = (m_pending > 0) ? 1 : -1;
    // 反推注入量：预期位移落在步长目标内（增益是 dy/注入，注入 = 目标/增益）
    const int64_t target = StepTargetRows();
    const double gain = EffectiveGain();
    const int64_t injectByTarget =
        std::max<int64_t>(1, static_cast<int64_t>(std::llround(static_cast<double>(target) / gain)));
    const int64_t inject = dir * std::min<int64_t>(std::llabs(m_pending), injectByTarget);
    Step step;
    step.injectPixels = inject;
    step.expectedShift =
        static_cast<int64_t>(std::llround(static_cast<double>(inject) * gain));
    m_pending -= inject;
    m_inFlight = true;
    return step;
}

void ScrollPlanner::CompleteStep(int64_t dy, int64_t injectPixels, bool matched, bool calibrate) {
    if (!m_inFlight) {
        return;  // 无进行中的步骤（防御）
    }
    m_inFlight = false;
    if (matched && calibrate && dy != 0 && injectPixels != 0 && (dy > 0) == (injectPixels > 0)) {
        // 增益 EMA 滑动更新（k = dy/注入），适应不同应用/容器的滚动缩放差异
        const double k = static_cast<double>(dy) / static_cast<double>(injectPixels);
        // 极小注入对应巨大位移说明存在迟到响应等异常，不能把它带入后续步长。
        if (k >= m_config.minGain && k <= m_config.maxGain) {
            ++m_completedSteps;
            m_gain = std::clamp(m_gain + m_config.gainAlpha * (k - m_gain),
                                m_config.minGain, m_config.maxGain);
        }
    }
    if (matched) {
        m_stepScale = std::min(1.0, m_stepScale * m_config.recoverStepScale);
    } else {
        // 失败（位移明显大于预期/重叠不足）→ 步长收缩，下一步更保守
        m_stepScale = std::max(m_config.minStepScale, m_stepScale * m_config.failStepScale);
    }
}

void ScrollPlanner::Reset() {
    m_pending = 0;
    m_inFlight = false;
}

}  // namespace shotlet
