#include "RecordingVisuals.h"
#include <algorithm>

namespace shotlet {
RectD RecordingCameraRect(const RecordingOptions& options, double contentScale) {
    constexpr double CameraSizeLogical = 160;
    constexpr double CameraInsetLogical = 8;
    const auto& region = options.region;
    const double inset = std::min(CameraInsetLogical * contentScale, std::min(region.width, region.height) / 8);
    const double size = std::min({CameraSizeLogical * contentScale, region.width - 2 * inset, region.height - 2 * inset});
    return {region.x + inset, region.y + region.height - inset - size, size, size};
}
}
