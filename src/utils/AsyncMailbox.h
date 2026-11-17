#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace shotlet {
// 将一次异步回调交给后台消费者；取消必须能唤醒没有收到回调的等待者。
// 回调方和消费者共同持有此对象的所有权，不能借用已经销毁的界面对象。
template<class Value>
class AsyncMailbox {
public:
    bool Publish(Value value) {
        {
            std::lock_guard lock(m_mutex);
            if (m_cancelled || m_value) return false;
            m_value.emplace(std::move(value));
        }
        m_ready.notify_one();
        return true;
    }
    bool Wait(Value& value) {
        std::unique_lock lock(m_mutex);
        m_ready.wait(lock, [this] { return m_cancelled || m_value.has_value(); });
        if (m_cancelled) return false;
        value = std::move(*m_value);
        m_value.reset();
        return true;
    }
    void Cancel() {
        {
            std::lock_guard lock(m_mutex);
            m_cancelled = true;
            // 保留待取值直到消费者收尾，避免在发起取消的 UI 线程释放原生样本。
        }
        m_ready.notify_all();
    }
    bool IsCancelled() const {
        std::lock_guard lock(m_mutex);
        return m_cancelled;
    }
    // 由资源所属线程在关闭设备前归还待取值；析构放在锁外，避免原生释放回调重入。
    void DiscardPending() {
        std::optional<Value> pending;
        {
            std::lock_guard lock(m_mutex);
            pending.swap(m_value);
        }
    }
private:
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::optional<Value> m_value;
    bool m_cancelled = false;
};
} // namespace shotlet
