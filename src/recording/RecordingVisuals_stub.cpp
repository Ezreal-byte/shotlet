#include "RecordingVisuals.h"
namespace shotlet {
std::unique_ptr<RecordingBorder> CreateRecordingBorder(const RecordingOptions&, std::string&) { return {}; }
std::unique_ptr<RecordingCamera> CreateRecordingCamera(const RecordingOptions&, std::string&) { return {}; }
}
