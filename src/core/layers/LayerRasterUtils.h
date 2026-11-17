#pragma once

#include <imgui.h>

namespace shotlet {

void DrawLineToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                     int rowPitch, const ImVec2& canvasOffset,
                     ImVec2 p1, ImVec2 p2, ImU32 color, float thickness);

void FillTriangleToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                         int rowPitch, ImVec2 v0, ImVec2 v1, ImVec2 v2, ImU32 color);

} // namespace shotlet
