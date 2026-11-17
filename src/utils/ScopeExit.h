#pragma once
#include <utility>

namespace shotlet {
// 局部资源和临时状态在提前返回或异常时也按作用域释放。
template<class Callback>
class ScopeExit {
public:
    explicit ScopeExit(Callback callback) : m_callback(std::move(callback)) {}
    ~ScopeExit() { m_callback(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    Callback m_callback;
};
}
