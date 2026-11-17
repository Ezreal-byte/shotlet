#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace shotlet {

// 单线程调度策略，时间由调用方传入，便于验证积压、延迟和恢复行为。
class SmoothScrollInput {
public:
    SmoothScrollInput(int height, int minOverlap)
        : m_budget(std::max(1, std::min(height / 4, (height - minOverlap) / 2))),
          m_step(std::max(1, std::min(height / 64, m_budget / 8))) {}
    void SetContentHeight(int height, int minOverlap) {
        m_budget = std::max(1, std::min(height / 4, (height - minOverlap) / 2));
        m_step = std::max(1, std::min(height / 64, m_budget / 8));
        m_pending = std::min<int64_t>(m_pending, m_budget);
    }
    void Add(int64_t delta, int64_t inputTime, int64_t now) {
        if (!delta || m_failed || now - inputTime > 120) return;
        const int direction = delta > 0 ? 1 : -1;
        if (m_direction && m_direction != direction) return;
        m_direction = direction;
        m_pending = std::min<int64_t>(m_budget, m_pending + std::abs(std::clamp<int64_t>(delta, -m_budget, m_budget)));
        m_inputTime = inputTime;
    }
    void Feedback(int64_t movedRows, bool matched, int64_t now, bool freshAfterInjection) {
        m_failed = !matched;
        m_feedbackTime = now;
        m_debt = std::max<int64_t>(0, m_debt - std::abs(movedRows));
        if (!matched) { m_pending = 0; return; }
        // 未响应/滚到边界时等满观察窗口；旧样本不能解除累计位移预算。
        if (movedRows == 0 && freshAfterInjection && now - m_lastInject >= 400) {
            m_debt = 0;
            m_pending = 0;
        }
    }
    int64_t Next(int64_t now) {
        if (m_failed || now - m_inputTime > 120 ||
            (m_debt > 0 && now - std::max(m_feedbackTime, m_firstUnacked) > 120)) {
            m_pending = 0;
            return 0;
        }
        // 未确认注入不超过视口的四分之一；应用放大响应仍由重叠验证拦截。
        const int64_t step = std::min({m_pending, int64_t(m_step), m_budget - m_debt});
        if (step <= 0) return 0;
        if (m_debt == 0) m_firstUnacked = now;
        m_pending -= step;
        m_debt += step;
        m_lastInject = now;
        return m_direction * step;
    }
    int Budget() const { return m_budget; }
    int64_t Pending() const { return m_pending; }
    int64_t Debt() const { return m_debt; }
    void DiscardPending() { m_pending = 0; }
private:
    int m_budget, m_step, m_direction = 0;
    bool m_failed = false;
    int64_t m_pending = 0, m_debt = 0, m_inputTime = 0, m_feedbackTime = 0, m_lastInject = 0, m_firstUnacked = 0;
};
}
