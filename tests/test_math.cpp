#include <gtest/gtest.h>
#include <cmath>
#include "utils/CoordinateUtils.h"

TEST(MathTest, BasicArithmetic) {
    EXPECT_EQ(2 + 2, 4);
    EXPECT_NEAR(std::sin(0.0), 0.0, 1e-10);
}

TEST(CoordinateUtilsTest, ConvertsCGTopYToAppKitWindowOrigin) {
    const shotlet::RectD screens[] = {
        {0.0, 0.0, 1512.0, 982.0},
        {0.0, -900.0, 1440.0, 900.0},
    };

    const double globalMaxY = shotlet::GlobalMaxY(screens, 2);

    EXPECT_DOUBLE_EQ(globalMaxY, 982.0);
    EXPECT_DOUBLE_EQ(shotlet::CGYToAppKitBottomY(100.0, 206.0, globalMaxY), 676.0);
    EXPECT_DOUBLE_EQ(shotlet::CGYToAppKitBottomY(-900.0, 206.0, globalMaxY), 1676.0);
}

TEST(CoordinateUtilsTest, ConvertsAppKitYToCGY) {
    EXPECT_DOUBLE_EQ(shotlet::AppKitYToCGY(982.0, 982.0), 0.0);
    EXPECT_DOUBLE_EQ(shotlet::AppKitYToCGY(0.0, 982.0), 982.0);
}

TEST(CoordinateUtilsTest, ConvertsCGSelectionToAppKitWindowOriginUsingGlobalDesktopTop) {
    const shotlet::RectD screens[] = {
        {0.0, 0.0, 2560.0, 1440.0},      // external primary screen
        {320.0, 1440.0, 1512.0, 982.0},  // built-in screen above it
    };

    const double globalMaxY = shotlet::GlobalMaxY(screens, 2);

    EXPECT_DOUBLE_EQ(globalMaxY, 2422.0);
    EXPECT_DOUBLE_EQ(
        shotlet::CGYToAppKitBottomY(1500.0, 300.0, globalMaxY),
        622.0);
}

TEST(CoordinateUtilsTest, ConvertsAppKitAndCGYWithinMatchedScreen) {
    // 内置屏作为主屏时，外接屏在 AppKit 坐标中可能位于负 Y；
    // CG 坐标仍使用左上原点的全局布局。转换必须按所在屏幕映射，
    // 不能只依赖全局高度翻转。
    const double appKitScreenY = -1080.0;
    const double appKitScreenHeight = 1080.0;
    const double cgScreenY = 982.0;

    EXPECT_DOUBLE_EQ(
        shotlet::AppKitYToCGYInScreen(-100.0, appKitScreenY, appKitScreenHeight, cgScreenY),
        1082.0);
    EXPECT_DOUBLE_EQ(
        shotlet::CGYToAppKitBottomYInScreen(1082.0, 206.0, appKitScreenY, appKitScreenHeight, cgScreenY),
        -306.0);
}
