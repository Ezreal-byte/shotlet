#pragma once

#include <chrono>
#include <optional>

namespace shotlet {

// 截图窗口短暂复用后回收；用单调时钟，避免系统时间调整影响截止时间。
class OverlayReclaimTimer {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto ReuseDelay = std::chrono::seconds(30);

    void Schedule(Clock::time_point now) { m_deadline = now + ReuseDelay; }
    void Cancel() { m_deadline.reset(); }
    bool IsPending() const { return m_deadline.has_value(); }
    bool IsDue(Clock::time_point now) const { return m_deadline && now >= *m_deadline; }

private:
    std::optional<Clock::time_point> m_deadline;
};

} // namespace shotlet
