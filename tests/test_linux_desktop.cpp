#include <X11/Xlib.h>
// Xlib's legacy macros conflict with GTest and application enum names.
#undef Bool
#undef None
#undef Status

#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <limits>
#include <memory>
#include <optional>
#include "capture/backends/CaptureLinux.h"
#include "core/Canvas.h"
#include "output/FBORenderer.h"
#include "platform/FontProvider.h"
#include "platform/ImageOutput_linux.h"
#include "ui/ScreenOverlay.h"

namespace shotlet {
    namespace {
        TEST(LinuxSoftwareRenderer, OverlayEnablesFramePacing) {
            ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
            struct QuitVideo {
                ~QuitVideo() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
            } quit;
            ScreenOverlay overlay;
            ScreenOverlay::Config config;
            config.windowWidth = 128;
            config.windowHeight = 96;
            ASSERT_TRUE(overlay.PreCreate(config));
            auto *renderer = SDL_GetRenderer(overlay.GetSDLWindow());
            ASSERT_NE(renderer, nullptr);
            int vsync = 0;
            ASSERT_TRUE(SDL_GetRenderVSync(renderer, &vsync));
            EXPECT_EQ(vsync, 1);
        }

        TEST(LinuxSoftwareRenderer, AnnotatedExportPreservesDevicePixelScale) {
            ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
            struct QuitVideo {
                ~QuitVideo() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
            } quit;
            std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
                    SDL_CreateWindow("software scale export", 128, 96, SDL_WINDOW_HIDDEN), SDL_DestroyWindow);
            ASSERT_NE(window, nullptr);
            std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
                    SDL_CreateRenderer(window.get(), "software"), SDL_DestroyRenderer);
            ASSERT_NE(renderer, nullptr);
            auto *previous = ImGui::GetCurrentContext();
            auto *context = ImGui::CreateContext();
            struct RestoreContext {
                ImGuiContext *context;
                ImGuiContext *previous;
                ~RestoreContext() {
                    ImGui::SetCurrentContext(context);
                    if (ImGui::GetIO().BackendRendererUserData)
                        ImGui_ImplSDLRenderer3_Shutdown();
                    if (ImGui::GetIO().BackendPlatformUserData)
                        ImGui_ImplSDL3_Shutdown();
                    ImGui::DestroyContext(context);
                    ImGui::SetCurrentContext(previous);
                }
            } restore{context, previous};
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().Fonts->AddFontDefault();
            ASSERT_TRUE(ImGui_ImplSDL3_InitForSDLRenderer(window.get(), renderer.get()));
            ASSERT_TRUE(ImGui_ImplSDLRenderer3_Init(renderer.get()));
            std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
                    SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 256, 192),
                    SDL_DestroyTexture);
            ASSERT_NE(texture, nullptr);
            auto pixels = std::make_unique<unsigned char[]>(256 * 192 * 4);
            for (size_t i = 0; i < 256 * 192 * 4; i += 4) {
                pixels[i] = 40;
                pixels[i + 1] = 80;
                pixels[i + 2] = 112;
                pixels[i + 3] = 255;
            }
            ASSERT_TRUE(SDL_UpdateTexture(texture.get(), nullptr, pixels.get(), 256 * 4));
            Canvas canvas;
            canvas.ReplaceScreenshotAndSelectAll(reinterpret_cast<ImTextureID>(texture.get()), ImVec2(256, 192),
                                                 ImVec2(128, 96), std::move(pixels), 256 * 4);
            canvas.SetWatermarkText("scale");
            const auto result = FBORenderer::RenderToOffscreen(nullptr, window.get(), context, &canvas,
                                                               canvas.GetSelection(), 2.0f);
            ASSERT_NE(result.data, nullptr) << SDL_GetError();
            ASSERT_EQ(result.width, 256);
            ASSERT_EQ(result.height, 192);
            const auto *corner = result.data.get() + (190 * 256 + 254) * 4;
            EXPECT_EQ(corner[3], 255);
            int changed = 0, opaque = 0;
            for (size_t i = 0; i < 256 * 192 * 4; i += 4) {
                changed += result.data[i] != 40;
                opaque += result.data[i + 3] == 255;
            }
            EXPECT_EQ(opaque, 256 * 192) << "Background must fill the whole device resolution";
            EXPECT_GT(changed, 30) << "Watermark must be exported at the scaled device resolution";
        }

        TEST(LinuxSoftwareRenderer, SeparateVertexAndColorStridesPreserveTexturePixels) {
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
                    SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
            ASSERT_NE(surface, nullptr);
            std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
                    SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
            ASSERT_NE(renderer, nullptr) << SDL_GetError();
            std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
                    SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 12, 12),
                    SDL_DestroyTexture);
            ASSERT_NE(texture, nullptr);
            std::array<unsigned char, 12 * 12 * 4> pixels{};
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = 40;
                pixels[i + 1] = 80;
                pixels[i + 2] = 112;
                pixels[i + 3] = 255;
            }
            ASSERT_TRUE(SDL_UpdateTexture(texture.get(), nullptr, pixels.data(), 12 * 4));
            struct Vertex {
                float x, y, u, v;
                uint32_t packedColor;
            };
            const Vertex vertices[] = {{-2, -2, 0, 0, 0xffffffff},
                                       {10, -2, 1, 0, 0xffffffff},
                                       {10, 10, 1, 1, 0xffffffff},
                                       {-2, 10, 0, 1, 0xffffffff}};
            const SDL_FColor colors[] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
            const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
            ASSERT_TRUE(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 0));
            ASSERT_TRUE(SDL_RenderClear(renderer.get()));
            ASSERT_TRUE(SDL_RenderGeometryRaw(renderer.get(), texture.get(), &vertices[0].x, sizeof(Vertex), colors,
                                              sizeof(SDL_FColor), &vertices[0].u, sizeof(Vertex), 4, indices, 6,
                                              sizeof(uint16_t)))
                    << SDL_GetError();
            ASSERT_TRUE(SDL_FlushRenderer(renderer.get()));
            const auto *center = static_cast<const unsigned char *>(surface->pixels) + 4 * surface->pitch + 4 * 4;
            EXPECT_EQ(center[0], 40);
            EXPECT_EQ(center[1], 80);
            EXPECT_EQ(center[2], 112);
            EXPECT_EQ(center[3], 255);
        }

        class ScopedEnvironment {
        public:
            ScopedEnvironment(const char *name, const char *value) : name(name) {
                if (const char *old = std::getenv(name))
                    previous = old;
                setenv(name, value, 1);
            }
            ~ScopedEnvironment() {
                if (previous)
                    setenv(name.c_str(), previous->c_str(), 1);
                else
                    unsetenv(name.c_str());
            }

        private:
            std::string name;
            std::optional<std::string> previous;
        };

        TEST(LinuxCaptureSessionTest, WaylandIsRejectedWithoutInventingADisplay) {
            ScopedEnvironment wayland("WAYLAND_DISPLAY", "wayland-0");
            CaptureLinux capture;
            EXPECT_FALSE(capture.Initialize());
            EXPECT_NE(capture.GetLastErrorMessage().find("X11"), std::string::npos);
            EXPECT_TRUE(capture.GetDisplays().empty());
        }

        class LinuxDesktopTest : public ::testing::Test {
        protected:
            Display *display = nullptr;
            Window window = 0;
            CaptureLinux capture;
            std::filesystem::path directory;
            static constexpr int WindowX = 80, WindowY = 60, Width = 320, Height = 240;

            void SetUp() override {
                display = XOpenDisplay(nullptr);
                ASSERT_NE(display, nullptr) << "Run this test with xvfb-run -a or an X11 desktop";
                const Window root = DefaultRootWindow(display);
                XColor color{}, exact{};
                ASSERT_TRUE(XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)), "#2060a0",
                                             &color, &exact));
                window = XCreateSimpleWindow(display, root, WindowX, WindowY, Width, Height, 0, 0, color.pixel);
                XSetWindowAttributes attributes{};
                attributes.override_redirect = True;
                XChangeWindowAttributes(display, window, CWOverrideRedirect, &attributes);
                XMapRaised(display, window);
                auto gc = XCreateGC(display, window, 0, nullptr);
                ASSERT_TRUE(XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)), "#e02040",
                                             &color, &exact));
                XSetForeground(display, gc, color.pixel);
                XFillRectangle(display, window, gc, 50, 40, 80, 70);
                XFreeGC(display, gc);
                XSync(display, False);
                ASSERT_TRUE(capture.Initialize()) << capture.GetLastErrorMessage();
                directory = std::filesystem::temp_directory_path() /
                            ("shotlet-linux-" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                ASSERT_TRUE(std::filesystem::create_directory(directory));
            }
            void TearDown() override {
                if (window)
                    XDestroyWindow(display, window);
                if (display)
                    XCloseDisplay(display);
                std::error_code ignored;
                if (!directory.empty())
                    std::filesystem::remove_all(directory, ignored);
            }
            CaptureOptions WindowOptions() {
                const auto displays = capture.GetDisplays();
                EXPECT_FALSE(displays.empty());
                CaptureOptions options;
                options.displayId = displays.front().id;
                options.hasDisplayInfo = true;
                options.displayInfo = displays.front();
                // Exercise the caller-supplied monitor geometry used by OverlayManager.
                options.displayInfo.displayX = WindowX;
                options.displayInfo.displayY = WindowY;
                options.displayInfo.displayWidth = Width;
                options.displayInfo.displayHeight = Height;
                return options;
            }
        };

        TEST_F(LinuxDesktopTest, CapturesDisplayBoundsAndRgbaColors) {
            auto image = capture.Capture(WindowOptions());
            ASSERT_NE(image.data, nullptr) << capture.GetLastErrorMessage();
            EXPECT_EQ(image.width, Width);
            EXPECT_EQ(image.height, Height);
            EXPECT_EQ(image.captureX, WindowX);
            EXPECT_EQ(image.captureY, WindowY);
            EXPECT_EQ(image.rowPitch, Width * 4);
            const auto *blue = image.data;
            EXPECT_NEAR(blue[0], 0x20, 4);
            EXPECT_NEAR(blue[1], 0x60, 4);
            EXPECT_NEAR(blue[2], 0xa0, 4);
            EXPECT_EQ(blue[3], 255);
            const auto *red = image.data + (45 * Width + 55) * 4;
            EXPECT_NEAR(red[0], 0xe0, 4);
            EXPECT_NEAR(red[1], 0x20, 4);
            EXPECT_NEAR(red[2], 0x40, 4);
        }

        TEST_F(LinuxDesktopTest, ClipsRelativeRegionToSelectedDisplay) {
            auto options = WindowOptions();
            options.captureX = -10;
            options.captureY = -15;
            options.captureW = 40;
            options.captureH = 40;
            auto image = capture.Capture(options);
            ASSERT_NE(image.data, nullptr);
            EXPECT_EQ(image.captureX, WindowX);
            EXPECT_EQ(image.captureY, WindowY);
            EXPECT_EQ(image.width, 30);
            EXPECT_EQ(image.height, 25);
        }

        TEST_F(LinuxDesktopTest, RejectsInvalidBoundsAndClearsErrorAfterRecovery) {
            auto options = WindowOptions();
            options.captureX = std::numeric_limits<int>::max();
            options.captureW = 50;
            options.captureH = 50;
            EXPECT_EQ(capture.Capture(options).data, nullptr);
            EXPECT_FALSE(capture.GetLastErrorMessage().empty());
            options.captureX = 50;
            options.captureY = 40;
            options.captureW = 10;
            options.captureH = 10;
            auto image = capture.Capture(options);
            ASSERT_NE(image.data, nullptr);
            EXPECT_EQ(image.captureX, WindowX + 50);
            EXPECT_EQ(image.captureY, WindowY + 40);
            EXPECT_TRUE(capture.GetLastErrorMessage().empty());
            options.captureH = 0;
            EXPECT_EQ(capture.Capture(options).data, nullptr);
        }

        TEST_F(LinuxDesktopTest, UnknownDisplayIsNotSilentlyCapturedAsWholeDesktop) {
            CaptureOptions options;
            options.displayId = std::numeric_limits<int>::max();
            EXPECT_EQ(capture.Capture(options).data, nullptr);
            EXPECT_FALSE(capture.GetLastErrorMessage().empty());
        }

        TEST_F(LinuxDesktopTest, PngRoundTripPreservesColorsAndUnicodePath) {
            auto image = capture.Capture(WindowOptions());
            ASSERT_NE(image.data, nullptr);
            const auto path = directory / std::filesystem::path(u8"截图 test.png");
            std::string error;
            ASSERT_TRUE(SaveLinuxPng(image.data, image.width, image.height, path, error)) << error;
            auto *loaded = SDL_LoadPNG(path.c_str());
            ASSERT_NE(loaded, nullptr) << SDL_GetError();
            auto *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
            ASSERT_NE(rgba, nullptr);
            EXPECT_EQ(rgba->w, Width);
            EXPECT_EQ(rgba->h, Height);
            const auto *red = static_cast<unsigned char *>(rgba->pixels) + 45 * rgba->pitch + 55 * 4;
            EXPECT_NEAR(red[0], 0xe0, 4);
            EXPECT_NEAR(red[1], 0x20, 4);
            EXPECT_NEAR(red[2], 0x40, 4);
            SDL_DestroySurface(rgba);
            SDL_DestroySurface(loaded);
            EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                                    std::filesystem::directory_iterator()),
                      1);
        }

        TEST_F(LinuxDesktopTest, FailedSavePreservesExistingFileAndRemovesTemporaryFiles) {
            const auto path = directory / "existing.png";
            {
                std::ofstream file(path);
                file << "keep me";
            }
            unsigned char pixel[] = {10, 20, 30, 40};
            std::string error;
            EXPECT_FALSE(SaveLinuxPng(pixel, -1, 1, path, error));
            {
                std::ifstream file(path);
                std::string text;
                std::getline(file, text);
                EXPECT_EQ(text, "keep me");
            }
            const auto folder = directory / "folder.png";
            ASSERT_TRUE(std::filesystem::create_directory(folder));
            EXPECT_FALSE(SaveLinuxPng(pixel, 1, 1, folder, error));
            EXPECT_TRUE(std::filesystem::is_directory(folder));
            ASSERT_TRUE(SaveLinuxPng(pixel, 1, 1, path, error)) << error;
            auto *saved = SDL_LoadPNG(path.c_str());
            ASSERT_NE(saved, nullptr);
            SDL_DestroySurface(saved);
            EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                                    std::filesystem::directory_iterator()),
                      2);
        }

        TEST_F(LinuxDesktopTest, LoadsSystemCjkFontAndItsCollectionIndex) {
            const auto fonts = GetCachedFontData();
            const auto sizes = GetCachedFontDataSizes();
            ASSERT_EQ(fonts.size(), 1u) << "Install fonts-noto-cjk or fonts-wqy-zenhei";
            ASSERT_EQ(sizes.size(), fonts.size());
            EXPECT_GT(sizes.front(), 0);
            EXPECT_NE(fonts.front(), nullptr);
            EXPECT_GE(GetChineseFontNo(), 0);
            ASSERT_EQ(GetChineseFontPaths().size(), 1u);
            EXPECT_TRUE(std::filesystem::is_regular_file(GetChineseFontPaths().front()));
        }
    } // namespace
} // namespace shotlet
