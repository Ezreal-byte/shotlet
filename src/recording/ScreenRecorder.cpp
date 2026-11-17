#include "ScreenRecorder.h"
#include <cmath>

namespace shotlet {
bool PrepareRecordingOptions(int displayId, RectD region, double pixelDensity, RecordingOptions& result) {
    constexpr int ChromaAlignmentPixels = 2;
    constexpr int MaximumDimensionPixels = 16384;
    result = {};
    if (displayId < 0 || !std::isfinite(pixelDensity) || pixelDensity <= 0 ||
        !std::isfinite(region.x) || !std::isfinite(region.y) ||
        !std::isfinite(region.width) || !std::isfinite(region.height)) return false;
    const double widthPixels = std::floor(region.width * pixelDensity);
    const double heightPixels = std::floor(region.height * pixelDensity);
    if (widthPixels < ChromaAlignmentPixels || heightPixels < ChromaAlignmentPixels ||
        widthPixels > MaximumDimensionPixels || heightPixels > MaximumDimensionPixels) return false;
    result.displayId = displayId;
    result.widthPixels = int(widthPixels) / ChromaAlignmentPixels * ChromaAlignmentPixels;
    result.heightPixels = int(heightPixels) / ChromaAlignmentPixels * ChromaAlignmentPixels;
    region.width = result.widthPixels / pixelDensity;
    region.height = result.heightPixels / pixelDensity;
    result.region = region;
    return true;
}

RecordingSession::RecordingSession(std::unique_ptr<IScreenRecorder> recorder) : m_recorder(std::move(recorder)), m_nativeRecorder(!m_recorder) {}
RecordingSession::~RecordingSession() {
    if (m_started) { std::string ignored; Stop(ignored); }
}
bool RecordingSession::Start(const RecordingOptions& options, const std::filesystem::path& target, std::string& error) {
    if (m_started) { error = "录制已开始"; return false; }
    if (m_nativeRecorder) m_recorder = CreateScreenRecorder(options.format);
    if (!m_file.Prepare(target, error)) return false;
    if (!m_recorder->Start(options, m_file.Path(), error)) { m_file.Discard(); return false; }
    m_started = true;
    m_paused = false;
    return true;
}
bool RecordingSession::SetPaused(bool paused, std::string& error) {
    error.clear();
    if (!m_started) { error = "录制尚未开始"; return false; }
    if (paused == m_paused) return true;
    if (!m_recorder->SetPaused(paused, error)) return false;
    m_paused = paused;
    return true;
}
bool RecordingSession::Stop(std::string& error) {
    if (!m_started) { error = "录制尚未开始"; return false; }
    m_started = false;
    m_paused = false;
    if (!m_recorder->Stop(error)) { m_file.Discard(); return false; }
    return m_file.Commit(error);
}
}
