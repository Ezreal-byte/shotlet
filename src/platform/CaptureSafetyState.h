#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace shotlet {

// 与原生采样分离，确保超时和焦点切换的边界可以在没有 Windows 桌面的环境回归。
class CaptureSafetyState {
public:
    using Clock = std::chrono::steady_clock;
    enum class Reason { None, Unresponsive, EscapeHeld, DesktopChanged, ForegroundLost };
    struct Sample {
        bool anyWindowVisible = false;
        bool inputDesktopActive = true;
        bool escapeDown = false;
        bool foregroundOwned = false;
        bool foregroundRequired = true;
    };

    static constexpr auto HeartbeatTimeout = std::chrono::seconds(5);
    static constexpr auto EscapeHoldDuration = std::chrono::seconds(1);
    static constexpr auto ActivationGracePeriod = std::chrono::milliseconds(1500);
    static constexpr auto ForegroundLossGracePeriod = std::chrono::milliseconds(250);

    static const char* ReasonName(Reason reason) {
        switch (reason) {
            case Reason::None: return "none";
            case Reason::Unresponsive: return "ui_unresponsive";
            case Reason::EscapeHeld: return "escape_held";
            case Reason::DesktopChanged: return "input_desktop_changed";
            case Reason::ForegroundLost: return "foreground_lost";
        }
        return "unknown";
    }

    Reason Evaluate(Clock::time_point now, Clock::time_point heartbeat, const Sample& sample) {
        if (m_reason != Reason::None) return m_reason;
        if (!sample.anyWindowVisible) {
            m_visibleSince.reset();
            m_escapeSince.reset();
            m_foregroundLostSince.reset();
            m_hadForeground = false;
            return Reason::None;
        }
        if (!m_visibleSince) m_visibleSince = now;
        if (!sample.inputDesktopActive) return m_reason = Reason::DesktopChanged;
        // 创建首屏之前可能耗时很长；只计算遮罩真正可见后的无响应时间。
        if (now - std::max(heartbeat, *m_visibleSince) >= HeartbeatTimeout)
            return m_reason = Reason::Unresponsive;

        if (!sample.escapeDown) m_escapeSince.reset();
        else {
            if (!m_escapeSince) m_escapeSince = now;
            if (now - *m_escapeSince >= EscapeHoldDuration) return m_reason = Reason::EscapeHeld;
        }

        // 滚动采集需要把前台交还目标应用，但安全桌面、长按 Esc 和心跳保护仍生效。
        if (!sample.foregroundRequired) {
            m_foregroundLostSince.reset();
            m_hadForeground = false;
            return Reason::None;
        }
        if (sample.foregroundOwned) {
            m_hadForeground = true;
            m_foregroundLostSince.reset();
        } else {
            if (!m_foregroundLostSince) m_foregroundLostSince = now;
            const auto grace = m_hadForeground ? ForegroundLossGracePeriod : ActivationGracePeriod;
            if (now - *m_foregroundLostSince >= grace) return m_reason = Reason::ForegroundLost;
        }
        return Reason::None;
    }

private:
    Reason m_reason = Reason::None;
    std::optional<Clock::time_point> m_visibleSince, m_escapeSince, m_foregroundLostSince;
    bool m_hadForeground = false;
};

} // namespace shotlet
