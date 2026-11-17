#pragma once
#include "i18n/Localization.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <mutex>
#include "scrollcapture/ScrollTarget.h"

#include "scrollcapture/ScrollCaptureTypes.h"  // GlobalRect

namespace shotlet {

// 滚轮接管与合成注入（设计 10.2 MacScrollDriver）：CGEventTap 拦截 + pixel 合成事件。
// 线程模型：tap 回调在专用 run loop 线程，回调必须 O(1)（设计 10.2）；
// Start/ValidateTarget/Stop 由控制线程调用；Inject 可由独立调度线程调用，Stop 前必须 join。
class MacScrollDriver {
public:
    // 用户滚轮意图回调：deltaPixels 向下为正（对齐 ScrollPlanner）。
    // 在 tap 专用线程调用，实现必须 O(1)、无锁无 I/O，典型只做 atomic 累加
    using IntentCallback = std::function<void(int64_t deltaPixels)>;

    struct Config {
        double displayScaleFactor = 1.0;  // 选区所在显示器 像素/点（point delta → pixel 意图）
        bool protectSelection = false; // 正式采集时拦截点击、拖拽和 Esc
        GlobalRect controlRect;        // 独立控制面板允许交互，即使覆盖选区
    };

    // 辅助功能权限（AXIsProcessTrusted；prompt=true 弹系统引导，HotkeyService 同款）
    static bool CheckPermission(bool prompt);

    // 注入坐标（全局点）：默认取首个被拦截滚轮事件的鼠标位置（设计 10.2 注入坐标锁定）；
    // 调用此方法可在首次滚动前显式指定（demo --inject）
    void SetInjectPoint(int x, int y);

    // 必须在 Start 前调用：选区所在显示器缩放系数（point delta → pixel 意图换算）
    void SetConfig(const Config& config) { m_config = config; }

    // 安装事件 tap 并开始拦截。selectionRect 为全局坐标点矩形（左上原点）；
    // tap 只吞选区内滚轮，其余事件原样放行。
    bool Start(const GlobalRect& selectionRect, const IntentCallback& onIntent);
    // 卸载 tap（所有退出路径必须调用，设计 15"遗留 tap"防御）
    void Stop();
    void RequestStop();

    // 注入一步合成滚轮（pixelDelta 向下为正）。合成事件源自本进程 pid，
    // tap 检测到 pid 标记直接放行（防自吞，设计 10.2）
    enum class Phase { None, Began, Changed, Ended };
    void Inject(int64_t pixelDelta, Phase phase = Phase::None);
    bool ValidateTarget(std::string& error);
    TextId GetTargetErrorId() const { return m_targetErrorId; }
    bool CancelRequested() const { return m_cancelRequested.load(); }

    [[nodiscard]] bool IsTapActive() const { return m_tapActive.load(); }
    [[nodiscard]] const std::string& GetLastErrorMessage() const { return m_lastError; }

    // 内部接口（.mm 的 tap trampoline 调用，refcon = this；不对外）。
    // 返回 true = 吞掉事件。内部严格 O(1)（设计 10.2）
    bool HandleTapEvent(uint32_t cgEventType, void* cgEvent);

    MacScrollDriver() = default;
    ~MacScrollDriver();
    MacScrollDriver(const MacScrollDriver&) = delete;
    MacScrollDriver& operator=(const MacScrollDriver&) = delete;

private:
    // tap 专用线程入口：创建 tap → 加入 run loop → CFRunLoopRun
    void RunLoopThreadMain();

    // ObjC/CoreFoundation 对象桥接（头文件保持纯 C++）
    void* m_tap = nullptr;            // CFMachPortRef
    void* m_runLoopSource = nullptr;  // CFRunLoopSourceRef
    void* m_eventSource = nullptr;    // CGEventSourceRef（注入用）
    // 控制线程唤醒 tap 线程；退出前在同一把锁内摘除，避免访问已退出线程的 run loop。
    mutable std::mutex m_runLoopMutex;
    void* m_runLoop = nullptr;

    std::thread m_loopThread;
    std::atomic<bool> m_tapActive{false};
    std::atomic<bool> m_stopRequested{false};

    // tap 回调线程读取 / 控制线程写入 的共享状态（简单原子，保持回调无锁）
    std::atomic<int> m_rectX{0};
    std::atomic<int> m_rectY{0};
    std::atomic<int> m_rectW{0};
    std::atomic<int> m_rectH{0};
    std::atomic<int> m_injectX{0};  // 全局点
    std::atomic<int> m_injectY{0};
    std::atomic<bool> m_injectPointSet{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<unsigned> m_disabledCount{0};
    unsigned m_swallowedButtons = 0; // 仅 tap 线程使用，拖出选区后仍吞掉配对的 up
    ScrollTargetWindow m_target;    // 仅后台控制线程使用
    Config m_config;             // Start 前写入，tap 线程只读
    double m_scaleFactor = 1.0;  // m_config.displayScaleFactor 快照（tap 回调只读）

    IntentCallback m_onIntent;  // 仅 tap 线程调用（Start 后不变更）
    TextId m_targetErrorId = TextId::ScrollHookLost;
    std::string m_lastError;
};

}  // namespace shotlet
