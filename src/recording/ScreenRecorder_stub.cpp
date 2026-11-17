#include "ScreenRecorder.h"
namespace shotlet {
namespace {
class UnsupportedRecorder final : public IScreenRecorder {
public:
    bool Start(const RecordingOptions&, const std::filesystem::path&, std::string& error) override {
        error = "当前平台暂不支持屏幕录制"; return false;
    }
    bool Stop(std::string&) override { return true; }
    bool SetPaused(bool, std::string& error) override { error = "当前平台暂不支持屏幕录制"; return false; }
    std::string GetError() const override { return {}; }
};
}
std::unique_ptr<IScreenRecorder> CreateScreenRecorder(RecordingFormat) { return std::make_unique<UnsupportedRecorder>(); }
bool IsScreenRecordingSupported() { return false; }
void ExcludeRecordingControlWindow(SDL_Window*) {}
uint32_t RecordingControlWindowId(SDL_Window*) { return 0; }
bool PrepareRecordingPermissions(std::string& error, bool) { error = "当前平台暂不支持屏幕录制"; return false; }
}
