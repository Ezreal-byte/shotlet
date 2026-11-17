#pragma once
#include "ScreenRecorder.h"
#include <memory>

namespace shotlet {
class RecordingBorder {
public:
    virtual ~RecordingBorder() = default;
    virtual bool IsVisible() const = 0;
    virtual std::vector<uint32_t> CaptureWindowIds() const { return {}; }
};
std::unique_ptr<RecordingBorder> CreateRecordingBorder(const RecordingOptions& options, std::string& error);

class RecordingCamera {
public:
    virtual ~RecordingCamera() = default;
    virtual uint32_t CaptureWindowId() const = 0;
    virtual std::string GetError() const = 0;
};
std::unique_ptr<RecordingCamera> CreateRecordingCamera(const RecordingOptions& options, std::string& error);
// 坐标单位与选区一致；根据系统 UI 缩放放置，保持在选区左下角。
RectD RecordingCameraRect(const RecordingOptions& options, double contentScale);
}
