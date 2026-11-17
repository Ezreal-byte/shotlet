#include <gtest/gtest.h>
#include "platform/OcrWindowLayout.h"

TEST(OcrWindowLayoutTest, KeepsInitialImageAndTextLayout) {
    constexpr int imageW = 722;
    constexpr int imageH = 93;
    constexpr int dividerW = 1;
    constexpr float textRatio = 0.5f;
    constexpr int textW = 361;

    const auto layout = shotlet::CalculateOcrWindowLayout(
        imageW + dividerW + textW, imageH, dividerW, textRatio);

    EXPECT_EQ(layout.image.Width(), imageW);
    EXPECT_EQ(layout.image.Height(), imageH);
    EXPECT_EQ(layout.divider.left, imageW);
    EXPECT_EQ(layout.divider.Width(), dividerW);
    EXPECT_EQ(layout.text.left, imageW + dividerW);
    EXPECT_EQ(layout.text.Width(), textW);
    EXPECT_EQ(layout.text.Height(), imageH);
}

TEST(OcrWindowLayoutTest, ExpandsImageWhenWindowIsResizedWider) {
    constexpr int imageW = 722;
    constexpr int imageH = 93;
    constexpr int dividerW = 1;
    constexpr float textRatio = 0.5f;
    constexpr int widenedClientW = imageW + dividerW + 361 + 300;

    const auto layout = shotlet::CalculateOcrWindowLayout(
        widenedClientW, imageH, dividerW, textRatio);

    EXPECT_GT(layout.image.Width(), imageW);
    EXPECT_EQ(layout.image.Height(), imageH);
    EXPECT_EQ(layout.divider.left, layout.image.right);
    EXPECT_EQ(layout.text.left, layout.divider.right);
    EXPECT_EQ(layout.text.right, widenedClientW);
}
