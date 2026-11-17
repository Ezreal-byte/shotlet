// MacScrollDriver 非 macOS 占位实现（匹配 XxxService_stub 模式）
#include "scrollcapture/MacScrollDriver.h"
#include "utils/Logger.h"

namespace shotlet {

bool MacScrollDriver::CheckPermission(bool) {
    return false;
}

MacScrollDriver::~MacScrollDriver() {
    Stop();
}

bool MacScrollDriver::Start(const GlobalRect& selectionRect, const IntentCallback& onIntent) {
    Logger::Warning("[MacScrollDriver] scroll capture not supported on this platform");
    m_lastError = "scroll capture requires macOS";
    return false;
}

void MacScrollDriver::Stop() {}
void MacScrollDriver::RequestStop() { m_stopRequested.store(true); }

void MacScrollDriver::SetInjectPoint(int, int) {}

bool MacScrollDriver::HandleTapEvent(uint32_t, void*) { return false; }

void MacScrollDriver::RunLoopThreadMain() {}

void MacScrollDriver::Inject(int64_t, Phase) {}
bool MacScrollDriver::ValidateTarget(std::string&) { return false; }

}  // namespace shotlet
