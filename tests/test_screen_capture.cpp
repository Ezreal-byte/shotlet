#include <gtest/gtest.h>
#include "capture/ScreenCapture.h"
#include <type_traits>

using namespace shotlet;

namespace {
class FakeScreenCapture : public IScreenCapture {
public:
    bool Initialize() override { return true; }
    std::vector<DisplayInfo> GetDisplays() override { return {}; }
    CaptureImage Capture(const CaptureOptions&) override { return {}; }
    std::string GetLastErrorMessage() const override { return {}; }
};
} // namespace

TEST(ScreenCaptureContractTest, ConcurrentCaptureIsOptIn) {
    FakeScreenCapture capture;
    EXPECT_FALSE(capture.SupportsConcurrentCapture());
}

TEST(CaptureImageTest, MoveTransfersOwnedDataAndClearsSource) {
    static_assert(!std::is_copy_constructible_v<CaptureImage>);
    static_assert(!std::is_copy_assignable_v<CaptureImage>);
    static_assert(std::is_move_constructible_v<CaptureImage>);
    static_assert(std::is_move_assignable_v<CaptureImage>);

    CaptureImage image;
    image.data = new uint8_t[4]{1, 2, 3, 4};
    image.width = 1;
    image.height = 1;
    image.rowPitch = 4;
    image.displayId = 7;

    CaptureImage moved = std::move(image);

    EXPECT_EQ(image.data, nullptr);
    EXPECT_EQ(image.width, 0);
    EXPECT_EQ(image.height, 0);
    EXPECT_EQ(image.rowPitch, 0);
    EXPECT_EQ(moved.data[0], 1);
    EXPECT_EQ(moved.displayId, 7);
}
