#include "ui/Toolbar.h"

#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <iostream>
#include <memory>

int main() {
    // 位图绘制读取 ImGui 样式，独立测试需像应用一样先建立上下文。
    const std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context(
        ImGui::CreateContext(), ImGui::DestroyContext);
    const auto bitmap = shotlet::Toolbar::MakeScrollCaptureBitmap();
    if (bitmap.width != 216 || bitmap.height != 45 ||
        bitmap.rgba.size() != size_t(bitmap.width * bitmap.height * 4) ||
        bitmap.rgba[3] != 0) {
        std::cerr << "滚动截图工具栏位图尺寸或透明圆角错误\n";
        return 1;
    }
    for (int button = 0; button < 4; ++button) {
        int brightPixels = 0;
        for (int y = 9; y < 38; ++y) {
            for (int x = 8 + button * 52; x < 52 + button * 52; ++x) {
                const size_t index = (size_t(y) * bitmap.width + x) * 4;
                const int red = bitmap.rgba[index];
                const int green = bitmap.rgba[index + 1];
                const int blue = bitmap.rgba[index + 2];
                if (std::max({red, green, blue}) > 100) ++brightPixels;
            }
        }
        if (brightPixels <= 20) {
            std::cerr << "滚动截图工具栏图标缺失：" << button << '\n';
            return 1;
        }
    }
    // 覆盖整数和非整数 DPI，且圆角必须包含部分覆盖的透明像素。
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const auto scaled = shotlet::Toolbar::MakeScrollCaptureBitmap(scale);
        if (scaled.width != int(216 * scale) || scaled.height != int(std::lround(45 * scale)) ||
            scaled.rgba.size() != size_t(scaled.width) * scaled.height * 4) {
            std::cerr << "高 DPI 位图尺寸错误\n";
            return 1;
        }
        int partialPixels = 0;
        for (int y = 0; y < int(8 * scale); ++y) {
            for (int x = 0; x < int(8 * scale); ++x) {
                const auto alpha = scaled.rgba[(size_t(y) * scaled.width + x) * 4 + 3];
                if (alpha > 0 && alpha < 255) ++partialPixels;
            }
        }
        if (partialPixels < 5) {
            std::cerr << "圆角缺少抗锯齿覆盖率\n";
            return 1;
        }
        for (int button = 0; button < 4; ++button) {
            int smoothPixels = 0;
            for (int y = int(9 * scale); y < int(38 * scale); ++y) {
                for (int x = int((8 + button * 52) * scale); x < int((52 + button * 52) * scale); ++x) {
                    const size_t index = (size_t(y) * scaled.width + x) * 4;
                    const int red = scaled.rgba[index];
                    const int iconRed = button == 2 ? (shotlet::GetThemePalette(shotlet::Theme::Dark).danger & 255) :
                        button == 3 ? (shotlet::GetThemePalette(shotlet::Theme::Dark).confirm & 255) : 255;
                    if (red > 30 && red < iconRed) ++smoothPixels;
                }
            }
            if (smoothPixels < 10) {
                std::cerr << "图标边缘缺少抗锯齿过渡：" << button << '\n';
                return 1;
            }
        }
        // 背景和上下边缘应与主工具栏一致，不应残留白色描边或接缝。
        for (int y = 0; y < scaled.height; ++y) {
            const auto* pixel = scaled.rgba.data() + (size_t(y) * scaled.width + int(110 * scale)) * 4;
            if (pixel[0] != 30 || pixel[1] != 30 || pixel[2] != 31 || pixel[3] != 255) {
                std::cerr << "背景出现接缝或重复混合\n";
                return 1;
            }
        }
    }
    // 亮色背景不能用“亮像素计数”判断图标存在，按背景色差检查。
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const auto light = shotlet::Toolbar::MakeScrollCaptureBitmap(scale, shotlet::Theme::Light);
        const auto dark = shotlet::Toolbar::MakeScrollCaptureBitmap(scale, shotlet::Theme::Dark);
        const auto background = ImGui::ColorConvertU32ToFloat4(shotlet::GetThemePalette(shotlet::Theme::Light).panelBackground);
        if (light.width != dark.width || light.height != dark.height || light.rgba[3] != 0) return 1;
        const int expected[] = {int(std::lround(background.x * 255)), int(std::lround(background.y * 255)), int(std::lround(background.z * 255))};
        for (int button = 0; button < 4; ++button) {
            int iconPixels = 0;
            for (int y = int(9 * scale); y < int(38 * scale); ++y) {
                for (int x = int((8 + button * 52) * scale); x < int((52 + button * 52) * scale); ++x) {
                    const auto* pixel = light.rgba.data() + (size_t(y) * light.width + x) * 4;
                    if (pixel[3] == 255 && (std::abs(int(pixel[0]) - expected[0]) > 50 ||
                        std::abs(int(pixel[1]) - expected[1]) > 50 || std::abs(int(pixel[2]) - expected[2]) > 50)) ++iconPixels;
                }
            }
            if (iconPixels <= 20) { std::cerr << "亮色工具栏图标缺失\n"; return 1; }
        }
        for (int y = 0; y < light.height; ++y) {
            const auto* pixel = light.rgba.data() + (size_t(y) * light.width + int(110 * scale)) * 4;
            if (pixel[0] != expected[0] || pixel[1] != expected[1] || pixel[2] != expected[2] || pixel[3] != 255) return 1;
        }
        for (size_t i = 3; i < light.rgba.size(); i += 4) {
            if (light.rgba[i] != dark.rgba[i]) { std::cerr << "主题切换改变透明轮廓\n"; return 1; }
        }
    }
    return 0;
}
