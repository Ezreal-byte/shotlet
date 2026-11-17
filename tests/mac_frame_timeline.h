#pragma once

#include <algorithm>
#include <chrono>
#include <vector>

#include "scrollcapture/MacFrameSource.h"

namespace shotlet::TestUtils {

// 使用真实帧缓存与稳定判定，只把时间推进和送帧变成确定的事件序列。
class MacFrameTimeline {
public:
    using Milliseconds = std::chrono::duration<double, std::milli>;

    MacFrameTimeline(MacFrameSource& source, const CaptureBaseline& baseline)
        : m_source(source), m_baseline(baseline) {}

    // 借用像素至 WaitForStableFrame 返回；所有回调同步执行，不创建后台线程。
    void ScheduleFrame(Milliseconds offset, PixelView pixels) {
        m_events.push_back({std::chrono::ceil<Clock::duration>(offset), pixels});
    }

    bool WaitForStableFrame(CapturedFrame& out) {
        std::stable_sort(m_events.begin(), m_events.end(), [](const Event& a, const Event& b) {
            return a.offset < b.offset;
        });
        return m_source.WaitForStableFrame(m_baseline, out, {
            [this] { return m_now; },
            [this](uint64_t afterFrameId, CapturedFrame& frame, double timeoutMs) {
                return NextFrame(afterFrameId, frame, Milliseconds(timeoutMs));
            }});
    }

    Milliseconds Elapsed() const { return m_now.time_since_epoch(); }

private:
    using Clock = std::chrono::steady_clock;
    struct Event {
        Clock::duration offset;
        PixelView pixels;
    };

    bool NextFrame(uint64_t afterFrameId, CapturedFrame& out, Milliseconds timeout) {
        if (m_source.WaitForFrameReference(afterFrameId, out, 0)) return true;

        // 向上取整，防止浮点毫秒截断后时间停在截止点之前，造成零步长循环。
        const auto deadline = m_now + std::chrono::ceil<Clock::duration>(timeout);
        if (m_nextEvent == m_events.size() || m_events[m_nextEvent].offset > deadline.time_since_epoch()) {
            m_now = deadline;
            return false;
        }
        const auto& event = m_events[m_nextEvent++];
        m_now = Clock::time_point(event.offset);
        // 帧生成时间必须晚于注入基线和已发布的 Idle，避免被真实缓存当成旧帧丢弃。
        const auto displayTime = std::max(m_baseline.notBefore,
                                         m_source.TakeBaseline().frame.displayTime) + 1;
        m_source.HandleFrame(event.pixels.data, event.pixels.width, event.pixels.height,
                             event.pixels.pitch, false, displayTime);
        return m_source.WaitForFrameReference(afterFrameId, out, 0);
    }

    MacFrameSource& m_source;
    CaptureBaseline m_baseline;
    Clock::time_point m_now{};
    std::vector<Event> m_events;
    size_t m_nextEvent = 0;
};

}  // namespace shotlet::TestUtils
