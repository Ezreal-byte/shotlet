#pragma once
#include <chrono>
#include <imgui.h>

namespace shotlet::RecentCapturesLayout {
// 界面尺寸均为逻辑单位；SDL 的像素密度由渲染后端处理。
constexpr int Width = 368;
constexpr int HeaderHeight = 80;
constexpr int RowHeight = 58;
constexpr int FooterHeight = 54;
constexpr int FeedbackHeight = 122;
constexpr int EmptyBodyRows = 2;
constexpr int EdgeGap = 8;
constexpr int PreviewWidth = 400;
constexpr int PreviewHeight = 312;
constexpr int PreviewPixelWidth = PreviewWidth * 2;
constexpr int PreviewPixelHeight = PreviewHeight * 2;
constexpr float ThumbnailWidth = 68;
constexpr float ThumbnailHeight = 44;
constexpr float TextInset = 84;
constexpr float TextTop = 6;
constexpr float DetailTop = 30;
constexpr float RowRounding = 6;
constexpr float RowPadding = 6;
constexpr float FooterButtonHeight = 32;
constexpr float SettingsButtonWidth = 72;
constexpr auto PreviewDelay = std::chrono::milliseconds(200);
constexpr auto CopyFeedbackDuration = std::chrono::milliseconds(650);
constexpr auto FeedbackDuration = std::chrono::seconds(2);
constexpr size_t DisplayBudgetBytes = 16u * 1024u * 1024u;
constexpr size_t DecodeBufferCopies = 4;
} // namespace shotlet::RecentCapturesLayout
