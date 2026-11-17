#include <gtest/gtest.h>
#include "platform/MagnifierOverlay.h"

using namespace shotlet;

TEST(MagnifierScreenDataTest, UsesBorrowedDataWhenOwnerIsEmpty) {
    uint8_t pixels[4] = {1, 2, 3, 4};

    ScreenData data;
    data.borrowedData = pixels;

    EXPECT_EQ(data.GetData(), pixels);
    EXPECT_FALSE(data.OwnsData());
}

TEST(MagnifierScreenDataTest, OwnedDataTakesPrecedence) {
    uint8_t borrowed[4] = {1, 2, 3, 4};
    auto owned = std::make_unique<uint8_t[]>(4);
    owned[0] = 9;

    ScreenData data;
    data.borrowedData = borrowed;
    data.data = std::move(owned);

    EXPECT_NE(data.GetData(), borrowed);
    EXPECT_EQ(data.GetData()[0], 9);
    EXPECT_TRUE(data.OwnsData());
}

TEST(MagnifierOverlayTest, RefreshMousePositionUsesPlatformCoordinateSource) {
    MagnifierOverlay overlay;
    overlay.UpdateMousePosition(1, 2);

    overlay.RefreshMousePosition();

    EXPECT_EQ(overlay.m_mouseX, 123);
    EXPECT_EQ(overlay.m_mouseY, 456);
}
