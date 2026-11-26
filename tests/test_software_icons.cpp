#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>
#include <memory>
#include <queue>
#include "ui/SoftwareIconCache.h"

namespace shotlet {
    namespace {
        class SoftwareIconsTest : public testing::Test {
        protected:
            void SetUp() override {
                surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
                ASSERT_NE(surface, nullptr);
                renderer = SDL_CreateSoftwareRenderer(surface);
                ASSERT_NE(renderer, nullptr);
                previous = ImGui::GetCurrentContext();
                context = ImGui::CreateContext();
                ImGui::GetIO().DisplaySize = {64, 64};
                ImGui::GetIO().DeltaTime = 1.0f / 60;
                ImGui::GetIO().IniFilename = nullptr;
                ImGui::GetIO().Fonts->AddFontDefault();
                ASSERT_TRUE(ImGui_ImplSDLRenderer3_Init(renderer));
                cache = std::make_unique<SoftwareIconCache>(renderer);
            }
            void TearDown() override {
                cache.reset();
                if (context) {
                    ImGui::SetCurrentContext(context);
                    if (ImGui::GetIO().BackendRendererUserData)
                        ImGui_ImplSDLRenderer3_Shutdown();
                    ImGui::DestroyContext(context);
                    ImGui::SetCurrentContext(previous);
                }
                SDL_DestroyRenderer(renderer);
                SDL_DestroySurface(surface);
            }
            ImTextureID Draw(ToolbarIcons::IconType icon, ImU32 color = IM_COL32_WHITE) {
                ImGui_ImplSDLRenderer3_NewFrame();
                ImGui::NewFrame();
                if (!cache->Draw(ImGui::GetBackgroundDrawList(), {8, 8}, 26, icon, color, 1)) {
                    ADD_FAILURE() << SDL_GetError();
                    ImGui::EndFrame();
                    return ImTextureID_Invalid;
                }
                ImGui::Render();
                ImTextureID texture = ImTextureID_Invalid;
                for (auto *list: ImGui::GetDrawData()->CmdLists)
                    for (const auto &command: list->CmdBuffer)
                        if (command.ElemCount)
                            texture = command.GetTexID();
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
                SDL_RenderClear(renderer);
                ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
                SDL_FlushRenderer(renderer);
                return texture;
            }
            const unsigned char *Pixel(int x, int y) {
                return static_cast<unsigned char *>(surface->pixels) + y * surface->pitch + x * 4;
            }
            SDL_Surface *surface = nullptr;
            SDL_Renderer *renderer = nullptr;
            ImGuiContext *context = nullptr;
            ImGuiContext *previous = nullptr;
            std::unique_ptr<SoftwareIconCache> cache;
        };

        TEST_F(SoftwareIconsTest, CircleIsConnectedWithSmoothCoverageAndEmptyCenter) {
            ASSERT_NE(Draw(ToolbarIcons::IconType::Circle), ImTextureID_Invalid);
            EXPECT_EQ(Pixel(21, 21)[3], 0);
            bool visited[64][64]{};
            int components = 0, softEdges = 0;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
            const auto brightness = Pixel(x, y)[0];
            softEdges += brightness > 0 && brightness < 255;
            if (brightness < 32 || visited[y][x])
                        continue;
                    ++components;
                    std::queue<std::pair<int, int>> points;
                    points.emplace(x, y);
                    visited[y][x] = true;
                    while (!points.empty()) {
                        auto [px, py] = points.front();
                        points.pop();
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = px + dx, ny = py + dy;
                                if (nx < 0 || nx >= 64 || ny < 0 || ny >= 64 || visited[ny][nx] ||
                            Pixel(nx, ny)[0] < 32)
                                    continue;
                                visited[ny][nx] = true;
                                points.emplace(nx, ny);
                            }
                    }
                }
            }
            EXPECT_EQ(components, 1) << "Circle outline must not break into dotted segments";
            EXPECT_GT(softEdges, 30);
        }

        TEST_F(SoftwareIconsTest, SelectionColorReusesTextureAndMosaicKeepsItsBlackWhiteCells) {
            const auto texture = Draw(ToolbarIcons::IconType::Circle);
            EXPECT_EQ(Draw(ToolbarIcons::IconType::Circle, IM_COL32(0, 200, 0, 255)), texture);
            EXPECT_EQ(Pixel(11, 21)[0], 0);
            EXPECT_GT(Pixel(11, 21)[1], 100);
            ASSERT_NE(Draw(ToolbarIcons::IconType::Mosaic, IM_COL32(0, 200, 0, 255)), ImTextureID_Invalid);
            EXPECT_GT(Pixel(17, 17)[0], 180);
            EXPECT_LT(Pixel(19, 17)[0], 60);
            EXPECT_EQ(Pixel(19, 17)[3], 255);
        }

        TEST_F(SoftwareIconsTest, EveryVectorIconCanBeCached) {
            for (int icon = 0; icon <= int(ToolbarIcons::IconType::Crop); ++icon)
                EXPECT_NE(Draw(ToolbarIcons::IconType(icon)), ImTextureID_Invalid) << icon;
        }
    } // namespace
} // namespace shotlet
