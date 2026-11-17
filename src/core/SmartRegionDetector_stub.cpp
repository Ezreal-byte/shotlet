#include "SmartRegionDetector.h"

namespace shotlet {

void SmartRegionDetector::BuildCache(float screenOffsetX, float screenOffsetY,
                                     float screenW, float screenH) {
    // 其他平台的占位实现
    BuildCacheFromWindows({}, screenOffsetX, screenOffsetY, screenW, screenH);
}

} // namespace shotlet
