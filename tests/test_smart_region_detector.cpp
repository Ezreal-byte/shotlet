#include <gtest/gtest.h>
#include "core/SmartRegionDetector.h"

using namespace shotlet;

TEST(SmartRegionDetectorTest, ClearEmptiesCache) {
    SmartRegionDetector detector;
    detector.Clear();
    EXPECT_EQ(detector.FindBestRegion(100.0f, 100.0f), nullptr);
}

TEST(SmartRegionDetectorTest, FindBestRegionReturnsNullOnEmptyCache) {
    SmartRegionDetector detector;
    EXPECT_EQ(detector.FindBestRegion(0.0f, 0.0f), nullptr);
    EXPECT_EQ(detector.FindBestRegion(-100.0f, -100.0f), nullptr);
}

TEST(SmartRegionDetectorTest, FallsBackToWholeScreenForBlankArea) {
    SmartRegionDetector detector;
    std::vector<RawWindow> windows = {
        {ImRect(ImVec2(100.0f, 100.0f), ImVec2(500.0f, 500.0f)), "App"},
        {ImRect(ImVec2(0.0f, 860.0f), ImVec2(360.0f, 900.0f)), "Dock"}
    };

    detector.BuildCacheFromWindows(windows, 0.0f, 0.0f, 1440.0f, 900.0f);

    const SmartRegion* region = detector.FindBestRegion(1000.0f, 880.0f);
    ASSERT_NE(region, nullptr);
    EXPECT_FLOAT_EQ(region->bounds.Min.x, 0.0f);
    EXPECT_FLOAT_EQ(region->bounds.Min.y, 0.0f);
    EXPECT_FLOAT_EQ(region->bounds.Max.x, 1440.0f);
    EXPECT_FLOAT_EQ(region->bounds.Max.y, 900.0f);
}

TEST(SmartRegionDetectorTest, BuildCacheAndFindRegion) {
    SmartRegionDetector detector;
    detector.BuildCache(0.0f, 0.0f, 1920.0f, 1080.0f);
    const SmartRegion* region = detector.FindBestRegion(0.0f, 0.0f);
    if (region) {
        EXPECT_GT(region->bounds.GetWidth(), 0.0f);
        EXPECT_GT(region->bounds.GetHeight(), 0.0f);
    }
}
