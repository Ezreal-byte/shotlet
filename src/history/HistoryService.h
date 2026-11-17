#pragma once
#include "HistoryStore.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <set>

namespace shotlet {
class Canvas;
class HistoryBudget {
    struct State { std::atomic<size_t> used{0}; size_t limit; explicit State(size_t value) : limit(value) {} };
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease() { if (m_state) m_state->used.fetch_sub(m_bytes); }
        Lease(Lease&& other) noexcept : m_state(std::move(other.m_state)), m_bytes(other.m_bytes) {}
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        explicit operator bool() const { return bool(m_state); }
    private:
        friend class HistoryBudget;
        Lease(std::shared_ptr<State> state, size_t bytes) : m_state(std::move(state)), m_bytes(bytes) {}
        std::shared_ptr<State> m_state;
        size_t m_bytes = 0;
    };
    explicit HistoryBudget(size_t limit) : m_state(std::make_shared<State>(limit)) {}
    Lease TryReserve(size_t bytes);
    size_t Used() const { return m_state->used.load(); }
    size_t Limit() const { return m_state->limit; }
private:
    std::shared_ptr<State> m_state;
};

class HistoryService {
public:
    struct Candidate {
        HistoryBudget::Lease budget; // 最后释放：其余缓冲必须先析构
        std::string generation, action;
        CaptureDocument document;
        HistoryImage result;
        HistoryDetails details;
        Candidate(HistoryBudget::Lease lease) : budget(std::move(lease)) {}
    };
    explicit HistoryService(std::filesystem::path root, size_t memoryLimit = 512u * 1024u * 1024u);
    ~HistoryService();
    std::unique_ptr<Candidate> Prepare(Canvas* canvas, HistoryImage result, const std::string& action,
                                       const std::string& sourceRecordId = {});
    // 成功动作消费候选并入队；取消/失败动作直接销毁候选，预算自动归还。
    void Finish(std::unique_ptr<Candidate> candidate);
    void StopAccepting() { m_accepting = false; }
    void Drain();
    // 目录迁移失败后恢复原服务，不改变窗口持有的服务引用。
    void ResumeAfterDrain();
    std::vector<std::string> TakeNotices();
    HistoryStore& Store() { return m_store; }
    size_t ReservedBytes() const { return m_budget.Used(); }
    uint64_t Revision() { return m_store.Revision(); }
    void RequestIndex(const std::string& id, bool force = false);
    bool IsIndexing(const std::string& id);
    HistoryBudget::Lease ReserveTask(size_t bytes) { return m_budget.TryReserve(bytes); }
private:
    void Run();
    bool Index(const std::string& id);
    void Notice(std::string error);
    HistoryStore m_store;
    HistoryBudget m_budget;
    std::atomic<bool> m_accepting{true};
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::unique_ptr<Candidate>> m_queue;
    std::deque<std::string> m_indexQueue;
    std::set<std::string> m_indexPending;
    std::vector<std::string> m_notices;
    bool m_stop = false;
    std::thread m_worker;
};
} // namespace shotlet
