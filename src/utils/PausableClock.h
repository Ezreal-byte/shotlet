#pragma once
#include <chrono>

namespace shotlet {
// 调用方负责同步；显式时间参数便于使用同一个边界处理采集、暂停和停止。
class PausableClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    void Reset(TimePoint now = Clock::now()) { m_epoch = now; m_pausedAt = now; m_pauseDuration = {}; m_paused = false; }
    void SetPaused(bool paused, TimePoint now = Clock::now()) {
        if (paused == m_paused) return;
        if (paused) m_pausedAt = now;
        else m_pauseDuration += now - m_pausedAt;
        m_paused = paused;
    }
    Clock::duration Elapsed(TimePoint now = Clock::now()) const {
        return (m_paused ? m_pausedAt : now) - m_epoch - m_pauseDuration;
    }
    bool IsPaused() const { return m_paused; }
private:
    TimePoint m_epoch = Clock::now(), m_pausedAt = m_epoch;
    Clock::duration m_pauseDuration{};
    bool m_paused = false;
};
}
