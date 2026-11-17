// MacFrameSource 非 macOS 占位实现（匹配 XxxService_stub 模式）
#include "scrollcapture/MacFrameSource.h"
#include "utils/Logger.h"

namespace shotlet {

MacFrameSource::~MacFrameSource() {
    Stop();
}

bool MacFrameSource::Start(uint32_t cgDisplayId, const GlobalRect& rect, bool excludeOwnWindows, const std::function<bool()>&) {
    Logger::Warning("[MacFrameSource] scroll capture not supported on this platform");
    m_lastError = "scroll capture requires macOS";
    return false;
}

void MacFrameSource::Stop() {}
void MacFrameSource::RequestStop() {}

bool MacFrameSource::IsRunning() const { return false; }

bool MacFrameSource::WaitForFrameReference(uint64_t, CapturedFrame&, double) { return false; }

uint64_t MacFrameSource::CurrentFrameId() const { return 0; }

CaptureBaseline MacFrameSource::TakeBaseline() const { return {}; }

bool MacFrameSource::WaitForNextFrame(uint64_t, CapturedFrame&, double) { return false; }

bool MacFrameSource::WaitForStableFrame(const CaptureBaseline&, CapturedFrame&) { return false; }

void MacFrameSource::HandleFrame(const uint8_t*, int, int, int, bool, uint64_t) {}

void MacFrameSource::HandleIdleFrame(uint64_t) {}

void MacFrameSource::HandleStreamError(const char*) {}

}  // namespace shotlet
