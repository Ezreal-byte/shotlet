#include "platform/CaptureSafetyState.h"
#include <gtest/gtest.h>

namespace shotlet {
namespace {
using State = CaptureSafetyState;
using Reason = State::Reason;
constexpr auto StartTime = State::Clock::time_point{};
constexpr auto BeforeDeadline = std::chrono::milliseconds(1);

State::Sample VisibleSample() {
    State::Sample sample;
    sample.anyWindowVisible = true;
    sample.foregroundOwned = true;
    return sample;
}
} // namespace

TEST(CaptureSafetyStateTest, StalledUiTriggersAtHeartbeatDeadline) {
    State state;
    const auto sample = VisibleSample();
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    EXPECT_EQ(state.Evaluate(StartTime + State::HeartbeatTimeout - BeforeDeadline, StartTime, sample), Reason::None);
    EXPECT_EQ(state.Evaluate(StartTime + State::HeartbeatTimeout, StartTime, sample), Reason::Unresponsive);
}

TEST(CaptureSafetyStateTest, HealthyFramesAllowLongSelectionSessions) {
    State state;
    const auto sample = VisibleSample();
    constexpr int FrameCount = 20;
    for (int frame = 0; frame < FrameCount; ++frame) {
        const auto now = StartTime + State::HeartbeatTimeout * frame;
        EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    }
}

TEST(CaptureSafetyStateTest, HiddenPreparationDoesNotConsumeVisibleTimeout) {
    State state;
    auto sample = VisibleSample();
    sample.anyWindowVisible = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    const auto firstVisible = StartTime + State::HeartbeatTimeout * 2;
    sample.anyWindowVisible = true;
    EXPECT_EQ(state.Evaluate(firstVisible, StartTime, sample), Reason::None);
    EXPECT_EQ(state.Evaluate(firstVisible + State::HeartbeatTimeout, StartTime, sample), Reason::Unresponsive);
}

TEST(CaptureSafetyStateTest, LongEscapeCancelsEvenWithHealthyFramesAndTextInput) {
    State state;
    auto sample = VisibleSample();
    sample.escapeDown = true;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    const auto now = StartTime + State::EscapeHoldDuration;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::EscapeHeld);
}

TEST(CaptureSafetyStateTest, SeparateEscapeTapsDoNotAccumulateHoldDuration) {
    State state;
    auto sample = VisibleSample();
    sample.escapeDown = true;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    auto now = StartTime + State::EscapeHoldDuration - BeforeDeadline;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    sample.escapeDown = false;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    sample.escapeDown = true;
    now += BeforeDeadline;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
}

TEST(CaptureSafetyStateTest, SecurityDesktopImmediatelyCancelsVisibleCapture) {
    State state;
    auto sample = VisibleSample();
    sample.inputDesktopActive = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::DesktopChanged);
}

TEST(CaptureSafetyStateTest, GameRetainingForegroundCannotLeavePermanentOverlay) {
    State state;
    auto sample = VisibleSample();
    sample.foregroundOwned = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    const auto now = StartTime + State::ActivationGracePeriod;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::ForegroundLost);
}

TEST(CaptureSafetyStateTest, GameTakingForegroundBackCancelsAfterShortGrace) {
    State state;
    auto sample = VisibleSample();
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    sample.foregroundOwned = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    const auto now = StartTime + State::ForegroundLossGracePeriod;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::ForegroundLost);
}

TEST(CaptureSafetyStateTest, TransientForegroundLossAndSwitchingOwnWindowsAreAllowed) {
    State state;
    auto sample = VisibleSample();
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    sample.foregroundOwned = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    auto now = StartTime + State::ForegroundLossGracePeriod - BeforeDeadline;
    sample.foregroundOwned = true;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    now += State::ActivationGracePeriod;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
}

TEST(CaptureSafetyStateTest, RecoveryCannotRearmCancelledSession) {
    State state;
    auto sample = VisibleSample();
    sample.inputDesktopActive = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::DesktopChanged);
    sample.inputDesktopActive = true;
    sample.anyWindowVisible = false;
    const auto now = StartTime + State::HeartbeatTimeout;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::DesktopChanged);
}

TEST(CaptureSafetyStateTest, ScrollingMayYieldForegroundButStillHasHeartbeatProtection) {
    State state;
    auto sample = VisibleSample();
    sample.foregroundOwned = false;
    sample.foregroundRequired = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    auto now = StartTime + State::ActivationGracePeriod;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    EXPECT_EQ(state.Evaluate(now + State::HeartbeatTimeout, now, sample), Reason::Unresponsive);
}

TEST(CaptureSafetyStateTest, ReturningFromScrollingGetsTimeToActivateOverlayAgain) {
    State state;
    auto sample = VisibleSample();
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    sample.foregroundOwned = false;
    sample.foregroundRequired = false;
    EXPECT_EQ(state.Evaluate(StartTime, StartTime, sample), Reason::None);
    sample.foregroundRequired = true;
    auto now = StartTime + State::ActivationGracePeriod;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    now += State::ForegroundLossGracePeriod;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
    sample.foregroundOwned = true;
    EXPECT_EQ(state.Evaluate(now, now, sample), Reason::None);
}
} // namespace shotlet
