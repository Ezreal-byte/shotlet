#include "ui/Theme.h"
#include "utils/Colors.h"
#include "utils/ImGuiUtils.h"
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

using namespace shotlet;

namespace {
bool Parse(std::vector<std::string> args, ThemePreference& preference, std::string& error) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return ParseThemeArguments(static_cast<int>(argv.size()), argv.data(), preference, error);
}
double Luminance(ImU32 color) {
    const auto c = ImGui::ColorConvertU32ToFloat4(color);
    auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.x) + 0.7152 * linear(c.y) + 0.0722 * linear(c.z);
}
}

TEST(ThemeTest, ArgumentsAcceptBothFormsAndLastPreferenceWins) {
    ThemePreference preference = ThemePreference::Auto;
    std::string error;
    ASSERT_TRUE(Parse({"shotlet", "--theme", "light", "--language=en", "--theme=dark"}, preference, error));
    EXPECT_EQ(preference, ThemePreference::Dark);
    ASSERT_TRUE(Parse({"shotlet", "--theme=auto"}, preference, error));
    EXPECT_EQ(preference, ThemePreference::Auto);
    ASSERT_TRUE(Parse({"shotlet", "--theme", "light"}, preference, error));
    EXPECT_EQ(preference, ThemePreference::Light);
    EXPECT_TRUE(error.empty());
}

TEST(ThemeTest, InvalidOrMissingArgumentDoesNotPartiallyApplyPreference) {
    for (const auto& args : std::vector<std::vector<std::string>>{
        {"shotlet", "--theme"}, {"shotlet", "--theme="}, {"shotlet", "--theme", "--daemon"},
        {"shotlet", "--theme=LIGHT"}, {"shotlet", "--theme=light", "--theme=invalid"}}) {
        ThemePreference preference = ThemePreference::Dark;
        std::string error;
        EXPECT_FALSE(Parse(args, preference, error));
        EXPECT_EQ(preference, ThemePreference::Dark);
        EXPECT_FALSE(error.empty());
    }
}

TEST(ThemeTest, ExplicitPreferenceIgnoresSystemAndUnknownFallsBackToDark) {
    for (auto system : {std::optional<Theme>{}, std::optional{Theme::Light}, std::optional{Theme::Dark}}) {
        EXPECT_EQ(ResolveTheme(ThemePreference::Light, system), Theme::Light);
        EXPECT_EQ(ResolveTheme(ThemePreference::Dark, system), Theme::Dark);
    }
    EXPECT_EQ(ResolveTheme(ThemePreference::Auto, Theme::Light), Theme::Light);
    EXPECT_EQ(ResolveTheme(ThemePreference::Auto, Theme::Dark), Theme::Dark);
    EXPECT_EQ(ResolveTheme(ThemePreference::Auto, {}), Theme::Dark);
}

TEST(ThemeTest, RepeatedSwitchesPreserveGeometryAndIndependentContextStyles) {
    auto* first = ImGui::CreateContext();
    ImGui::GetStyle().ScaleAllSizes(2.0f);
    const auto scaled = ImGui::GetStyle();
    auto* second = ImGui::CreateContext();
    ApplyImGuiTheme(ImGui::GetStyle(), Theme::Dark);
    const auto secondText = ImGui::GetStyle().Colors[ImGuiCol_Text];
    ImGui::SetCurrentContext(first);
    for (int i = 0; i < 20; ++i) {
        const auto theme = i % 2 ? Theme::Dark : Theme::Light;
        auto& style = ImGui::GetStyle();
        ApplyImGuiTheme(style, theme);
        EXPECT_EQ(style.WindowPadding.x, scaled.WindowPadding.x);
        EXPECT_EQ(style.FramePadding.y, scaled.FramePadding.y);
        EXPECT_EQ(style.ScrollbarSize, scaled.ScrollbarSize);
        EXPECT_EQ(style.WindowRounding, scaled.WindowRounding);
        EXPECT_EQ(ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Text]), GetThemePalette(theme).text);
    }
    ImGui::SetCurrentContext(second);
    EXPECT_EQ(ImGui::GetStyle().Colors[ImGuiCol_Text].x, secondText.x);
    ImGui::DestroyContext(first);
    ImGui::DestroyContext(second);
}

TEST(ThemeTest, BothPalettesKeepTextReadableAndAnnotationPresetsUnchanged) {
    const ImU32 annotation = Colors::ToolDefault;
    for (const auto theme : {Theme::Light, Theme::Dark}) {
        const auto& p = GetThemePalette(theme);
        const double foreground = Luminance(p.text), background = Luminance(p.panelBackground);
        EXPECT_GE((std::max(foreground, background) + 0.05) / (std::min(foreground, background) + 0.05), 4.5);
        ImGuiStyle style;
        ApplyImGuiTheme(style, theme);
        EXPECT_EQ(Colors::ToolDefault, annotation);
        EXPECT_NE(p.icon, p.panelBackground);
    }
}

namespace {
    // 真实鼠标输入验证按钮、菜单和禁用态，避免仅检查颜色常量而遗漏 ImGui 的状态切换。
    class DestructiveControlsTest : public testing::Test {
    protected:
        enum class Control { Button, SmallButton, MenuItem };
        struct Result {
            ImRect bounds;
            bool clicked = false;
            bool hoveredRed = false;
            bool activeRed = false;
        };
        void SetUp() override {
            m_previous = ImGui::GetCurrentContext();
            m_context = ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(400, 240);
            io.ConfigInputTrickleEventQueue = false;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.Fonts->AddFontDefault();
        }
        void TearDown() override {
            ImGui::DestroyContext(m_context);
            ImGui::SetCurrentContext(m_previous);
        }
        Result Draw(Control control, Theme theme, bool disabled = false) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2());
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("danger-test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetCursorPos(ImVec2(20, 20));
            auto *draw = ImGui::GetWindowDrawList();
            const int firstVertex = draw->VtxBuffer.Size;
            const auto stackSize = m_context->ColorStack.Size;
            const auto style = ImGui::GetStyle();
            const auto &palette = GetThemePalette(theme);
            Result result;
            ImGui::BeginDisabled(disabled);
            if (control == Control::Button)
                result.clicked = ImGuiUtils::DestructiveButton("Delete", palette);
            else if (control == Control::MenuItem)
                result.clicked = ImGuiUtils::DestructiveMenuItem("Delete", palette);
            else {
                const ImGuiUtils::DestructiveStyle danger(palette);
                result.clicked = ImGui::SmallButton("Delete");
            }
            result.bounds = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::EndDisabled();
            for (int i = firstVertex; i < draw->VtxBuffer.Size; ++i) {
                result.hoveredRed |= draw->VtxBuffer[i].col == palette.dangerHovered;
                result.activeRed |= draw->VtxBuffer[i].col == palette.dangerActive;
            }
            EXPECT_EQ(m_context->ColorStack.Size, stackSize);
            for (int i = 0; i < ImGuiCol_COUNT; ++i)
                EXPECT_EQ(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[i]),
                          ImGui::ColorConvertFloat4ToU32(style.Colors[i]));
            ImGui::Button("Normal");
            ImGui::End();
            ImGui::Render();
            return result;
        }
        ImGuiContext *m_context = nullptr;
        ImGuiContext *m_previous = nullptr;
    };
} // namespace

TEST_F(DestructiveControlsTest, HoverPressClickAndDisabledStatesRestoreSharedStyle) {
    for (const auto theme: {Theme::Light, Theme::Dark}) {
        ApplyImGuiTheme(ImGui::GetStyle(), theme);
        for (const auto control: {Control::Button, Control::SmallButton, Control::MenuItem}) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(-1, -1);
            Draw(control, theme);
            const auto bounds = Draw(control, theme).bounds;
            io.AddMousePosEvent(bounds.GetCenter().x, bounds.GetCenter().y);
            EXPECT_TRUE(Draw(control, theme).hoveredRed);
            io.AddMouseButtonEvent(0, true);
            const auto pressed = Draw(control, theme);
            EXPECT_FALSE(pressed.clicked);
            // 菜单允许按下后移到另一项，ImGui 在按住期间仍沿用悬停背景。
            if (control == Control::MenuItem)
                EXPECT_TRUE(pressed.hoveredRed);
            else
                EXPECT_TRUE(pressed.activeRed);
            io.AddMouseButtonEvent(0, false);
            EXPECT_TRUE(Draw(control, theme).clicked);
            io.AddMouseButtonEvent(0, true);
            const auto disabled = Draw(control, theme, true);
            EXPECT_FALSE(disabled.clicked);
            EXPECT_FALSE(disabled.hoveredRed);
            EXPECT_FALSE(disabled.activeRed);
            io.AddMouseButtonEvent(0, false);
            EXPECT_FALSE(Draw(control, theme, true).clicked);
        }
    }
}

TEST(ColorTest, NativePackingPreservesChannelsAndPremultipliedAlpha) {
    EXPECT_EQ(Colors::ToNativeRgb(Colors::PresetBlue), 0x00fead0fu);
    EXPECT_EQ(Colors::ToBGRA(Colors::PresetBlue), 0xff0fadfeu);
    EXPECT_EQ(Colors::ToBGRA(Colors::MagnifierPremultipliedBorder), 0x331a1a1au);
    EXPECT_EQ(Colors::ToBGRA(Colors::Transparent), 0u);
}
