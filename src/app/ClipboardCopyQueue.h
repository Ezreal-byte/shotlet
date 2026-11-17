#pragma once

#include "platform/ClipboardService.h"

#if defined(SHOTLET_PLATFORM_MACOS)
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <deque>

namespace shotlet {

// 主线程提交和领取，后台只编码。最多保留正在编码和最新待编码的两张图。
// 每次提交都有独立结果承诺，剪贴板按需取数不依赖主线程轮询，避免死锁。
class ClipboardCopyQueue {
public:
    using Clock = std::chrono::steady_clock;
    using Encoder = std::function<ClipboardPng(const unsigned char*, int, int)>;
    struct Request {
        std::shared_ptr<const unsigned char> pixels;
        int width = 0, height = 0;
        uint64_t sequence = 0;
        Clock::time_point confirmed;
        std::shared_ptr<void> context;
    };
    struct Result {
        ClipboardPng png;
        int width = 0, height = 0;
        uint64_t sequence = 0;
        Clock::time_point confirmed;
        std::shared_ptr<void> context;
        double encodeMs = 0;
        bool superseded = false;
        std::string error;
    };

    explicit ClipboardCopyQueue(Encoder encoder = EncodeClipboardPng) : m_encoder(std::move(encoder)) {}
    ~ClipboardCopyQueue() {
        { std::lock_guard lock(m_mutex); m_stop = true; }
        m_wake.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    std::shared_future<ClipboardPng> Submit(Request request) {
        auto completion = std::make_shared<std::promise<ClipboardPng>>();
        auto future = completion->get_future().share();
        {
            std::lock_guard lock(m_mutex);
            if (m_pending) m_pending->completion->set_value({});
            m_latestSequence = request.sequence;
            m_pending = Work{std::move(request), std::move(completion)};
            if (!m_worker.joinable()) {
                try {
                    m_worker = std::thread([this] { Run(); });
                } catch (const std::exception& error) {
                    Result failed;
                    failed.context = std::move(m_pending->request.context);
                    failed.width = m_pending->request.width;
                    failed.height = m_pending->request.height;
                    failed.sequence = m_pending->request.sequence;
                    failed.confirmed = m_pending->request.confirmed;
                    failed.error = error.what();
                    m_pending->completion->set_value({});
                    m_pending.reset();
                    m_completed.push_back(std::move(failed));
                }
            }
        }
        m_wake.notify_all();
        return future;
    }

    bool Busy() const {
        std::lock_guard lock(m_mutex);
        return m_active || m_pending.has_value() || !m_completed.empty();
    }

    std::optional<Result> TakeCompleted(bool wait = false) {
        std::unique_lock lock(m_mutex);
        if (wait) m_wake.wait(lock, [this] { return !m_completed.empty() || (!m_active && !m_pending); });
        if (m_completed.empty()) return {};
        auto result = std::move(m_completed.front());
        m_completed.pop_front();
        result.superseded = result.sequence != m_latestSequence;
        return result;
    }

private:
    struct Work {
        Request request;
        std::shared_ptr<std::promise<ClipboardPng>> completion;
    };
    void Run() {
        for (;;) {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || m_pending; });
            if (!m_pending) return;
            auto work = std::move(*m_pending);
            m_pending.reset();
            m_active = true;
            lock.unlock();
            Result result;
            result.context = std::move(work.request.context);
            result.width = work.request.width;
            result.height = work.request.height;
            result.sequence = work.request.sequence;
            result.confirmed = work.request.confirmed;
            const auto start = Clock::now();
            try {
                result.png = m_encoder(work.request.pixels.get(), result.width, result.height);
            } catch (const std::exception& error) {
                result.error = error.what();
            } catch (...) {
                result.error = "unknown encoder failure";
            }
            result.encodeMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            work.request.pixels.reset();
            work.completion->set_value(result.png);
            lock.lock();
            m_active = false;
            m_completed.push_back(std::move(result));
            lock.unlock();
            m_wake.notify_all();
        }
    }

    Encoder m_encoder;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_worker;
    bool m_stop = false, m_active = false;
    uint64_t m_latestSequence = 0;
    std::optional<Work> m_pending;
    std::deque<Result> m_completed;
};

} // namespace shotlet
#endif
