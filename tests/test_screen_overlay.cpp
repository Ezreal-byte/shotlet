#include <gtest/gtest.h>
#include "ui/ScreenOverlay.h"

using namespace shotlet;

// Test Config default values
TEST(ScreenOverlayConfigTest, DefaultValues) {
    ScreenOverlay::Config config;
    EXPECT_EQ(config.displayId, -1);
    EXPECT_EQ(config.windowX, 0);
    EXPECT_EQ(config.windowY, 0);
    EXPECT_EQ(config.windowWidth, 0);
    EXPECT_EQ(config.windowHeight, 0);
    EXPECT_FLOAT_EQ(config.scaleFactor, 1.0f);
}

// Test Config can be initialized with custom values (Retina display)
TEST(ScreenOverlayConfigTest, RetinaDisplayConfig) {
    ScreenOverlay::Config config;
    config.displayId = 0;
    config.windowX = 0;
    config.windowY = 0;
    config.windowWidth = 1512;
    config.windowHeight = 982;
    config.scaleFactor = 2.0f;

    EXPECT_EQ(config.displayId, 0);
    EXPECT_EQ(config.windowX, 0);
    EXPECT_EQ(config.windowWidth, 1512);
    EXPECT_FLOAT_EQ(config.scaleFactor, 2.0f);
}

// Test Config for external display (1x scale)
TEST(ScreenOverlayConfigTest, ExternalDisplayConfig) {
    ScreenOverlay::Config config;
    config.displayId = 1;
    config.windowX = 1512;
    config.windowY = 0;
    config.windowWidth = 2560;
    config.windowHeight = 1440;
    config.scaleFactor = 1.0f;

    EXPECT_EQ(config.displayId, 1);
    EXPECT_EQ(config.windowX, 1512);
    EXPECT_FLOAT_EQ(config.scaleFactor, 1.0f);
}

// Test ScreenOverlay initial state (before Create)
// These test the inline accessors that don't need any implementation
TEST(ScreenOverlayTest, InitialState) {
    ScreenOverlay overlay;
    EXPECT_FALSE(overlay.IsCreated());
    EXPECT_FALSE(overlay.HasResult());
    EXPECT_FALSE(overlay.ShouldClose());
    EXPECT_FALSE(overlay.IsActive());
    EXPECT_EQ(overlay.GetDisplayId(), -1);
    EXPECT_EQ(overlay.GetCanvas(), nullptr);
}

// Test SetActive toggles active state
TEST(ScreenOverlayTest, SetActiveChangesState) {
    ScreenOverlay overlay;
    EXPECT_FALSE(overlay.IsActive());

    overlay.SetActive(true);
    EXPECT_TRUE(overlay.IsActive());

    overlay.SetActive(false);
    EXPECT_FALSE(overlay.IsActive());
}

// Test dim overlay: activation and dim removal are separate operations
TEST(ScreenOverlayTest, DimStateChangesOnlyWhenExplicitlyRemoved) {
    ScreenOverlay overlay;
    EXPECT_TRUE(overlay.IsDimmed());  // starts dimmed

    overlay.SetActive(true);
    EXPECT_TRUE(overlay.IsDimmed());  // activation alone does not remove dim

    overlay.RemoveDim();
    EXPECT_FALSE(overlay.IsDimmed());

    // Setting back to inactive should NOT restore dim
    overlay.SetActive(false);
    EXPECT_FALSE(overlay.IsDimmed());
}

// Test GetConfig returns the stored config
TEST(ScreenOverlayTest, GetConfigReturnsDefault) {
    ScreenOverlay overlay;
    const auto& config = overlay.GetConfig();
    EXPECT_EQ(config.displayId, -1);
    EXPECT_EQ(config.windowWidth, 0);
    EXPECT_FLOAT_EQ(config.scaleFactor, 1.0f);
}
