#include "RecordingPlaybackWindow.h"
namespace shotlet {
std::unique_ptr<RecordingPlaybackWindow> CreateRecordingPlaybackWindow(const std::filesystem::path&,
    const RecordingOptions&, Language language, Theme, std::string& error) {
    error = Tr(language, TextId::RecordingPlaybackUnsupported); return {};
}
}
