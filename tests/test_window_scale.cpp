#include "utils/WindowUtils.h"
#include <gtest/gtest.h>
#include <array>
#include <limits>

namespace {
constexpr float LogicalWidth = 640;
constexpr float LogicalHeight = 560;
constexpr float SidebarLogicalWidth = 160;
}

TEST(WindowScaleTest, LogicalSizeMatchesWindowsScalingAndMacRetinaWithoutDoubleScaling) {
    struct Case {
        const char* name;
        float pixelDensity, displayScale;
        float windowWidth, windowHeight, sidebarWidth, pixelWidth;
    };
    constexpr std::array cases{
        Case{"Windows 100%", 1, 1, 640, 560, 160, 640},
        Case{"Windows 125%", 1, 1.25f, 800, 700, 200, 800},
        Case{"Windows 150%", 1, 1.5f, 960, 840, 240, 960},
        Case{"Windows 175%", 1, 1.75f, 1120, 980, 280, 1120},
        Case{"Windows 200%", 1, 2, 1280, 1120, 320, 1280},
        Case{"macOS 1x", 1, 1, 640, 560, 160, 640},
        Case{"macOS Retina 2x", 2, 2, 640, 560, 160, 1280},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        const auto scale = shotlet::WindowUtils::CalculateDisplayScale(item.pixelDensity, item.displayScale);
        EXPECT_FLOAT_EQ(LogicalWidth * scale.contentScale, item.windowWidth);
        EXPECT_FLOAT_EQ(LogicalHeight * scale.contentScale, item.windowHeight);
        EXPECT_FLOAT_EQ(SidebarLogicalWidth * scale.contentScale, item.sidebarWidth);
        EXPECT_FLOAT_EQ(LogicalWidth * scale.contentScale * scale.pixelDensity, item.pixelWidth);
    }
}

TEST(WindowScaleTest, InvalidMeasurementsPreserveAnUnscaledLayout) {
    constexpr std::array invalid{0.f, -1.f, std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN()};
    for (const float value : invalid) {
        const auto invalidDensity = shotlet::WindowUtils::CalculateDisplayScale(value, 1);
        EXPECT_FLOAT_EQ(invalidDensity.pixelDensity, 1);
        EXPECT_FLOAT_EQ(invalidDensity.contentScale, 1);
        const auto invalidDisplayScale = shotlet::WindowUtils::CalculateDisplayScale(2, value);
        EXPECT_FLOAT_EQ(invalidDisplayScale.pixelDensity, 2);
        EXPECT_FLOAT_EQ(invalidDisplayScale.contentScale, 1);
    }
}

TEST(WindowScaleTest, AnchoredAndAdjacentPopupsStayOnTheSelectedDisplay) {
    using namespace shotlet::WindowUtils;
    const SDL_Rect area{-1440, -900, 1440, 875};
    const SDL_Point listSize{368, 590}, previewSize{400, 312};
    for (const SDL_Rect anchor : {SDL_Rect{-1400, -900, 24, 24}, SDL_Rect{-24, -900, 24, 24},
                                  SDL_Rect{-24, -49, 24, 24}, SDL_Rect{-1440, -49, 24, 24}}) {
        const auto list = PlaceAnchoredPopup(anchor, listSize, area, 8);
        const auto preview = PlaceAdjacentPopup(list, previewSize, area, 8);
        for (const auto bounds : {list, preview}) {
            EXPECT_GE(bounds.x, area.x); EXPECT_GE(bounds.y, area.y);
            EXPECT_LE(bounds.x + bounds.w, area.x + area.w);
            EXPECT_LE(bounds.y + bounds.h, area.y + area.h);
        }
    }
    const auto list = PlaceAnchoredPopup({-24, -900, 24, 24}, listSize, area, 8);
    const auto preview = PlaceAdjacentPopup(list, previewSize, area, 8);
    EXPECT_LT(preview.x, list.x);
    const auto small = PlaceAnchoredPopup({0, 0, 24, 24}, listSize, {0, 0, 240, 320}, 8);
    EXPECT_EQ(small.w, 240); EXPECT_EQ(small.h, 320);
}
