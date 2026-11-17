#include <array>
#include <cstdio>
#include <gtest/gtest.h>
#include <memory>
#include "ui/HistoryWindow.h"
#include "ui/WindowChrome.h"
#include "utils/ImGuiUtils.h"
#include "utils/StringUtils.h"

namespace shotlet {
    class HistoryWindowTest : public testing::Test {
    protected:
        void SetUp() override {
            m_previousContext = ImGui::GetCurrentContext();
            m_root = std::filesystem::temp_directory_path() / ("shotlet-history-window-" + NewHistoryId());
            m_service = std::make_unique<HistoryService>(m_root);
            m_context = ImGui::CreateContext();
            ImGui::SetCurrentContext(m_context);
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1280, 820);
            io.ConfigInputTrickleEventQueue = false;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            ImFontConfig font;
            font.SizePixels = 18.2f;
            io.FontDefault = io.Fonts->AddFontDefault(&font);
            m_window = std::make_unique<HistoryWindow>(nullptr, *m_service, Language::English, Theme::Light);
            m_window->m_context = m_context;
            m_window->ApplyStyle();
        }
        void TearDown() override {
            // 测试只运行真实 ImGui 控件，不创建 SDL 窗口或 GPU 纹理。
            if (m_window) {
                m_window->m_context = nullptr;
                m_window.reset();
            }
            if (m_context)
                ImGui::DestroyContext(m_context);
            ImGui::SetCurrentContext(m_previousContext);
            m_service.reset();
            std::error_code error;
            if (!m_root.empty())
                std::filesystem::remove_all(m_root, error);
        }
        void SeedRecords() {
            constexpr int64_t baseTimeMs = 1700000000000ll;
            constexpr std::array<int64_t, 4> offsetsMs{300, 100, 100, 200};
            for (size_t i = 0; i < offsetsMs.size(); ++i) {
                HistoryMetadata record;
                record.id = "00000000-0000-0000-0000-00000000000" + std::to_string(i);
                record.createdAtUnixMs = baseTimeMs + offsetsMs[i];
                record.width = 1280;
                record.height = 800;
                record.details.title = "Screenshot " + std::to_string(i);
                record.details.tags = i % 2 ? std::vector<std::string>{"Other"} : std::vector<std::string>{"Shared"};
                record.details.ocrIndexed = true;
                m_window->m_records.push_back(std::move(record));
            }
            m_window->UpdateFilter();
        }
        const std::string &Id(size_t index) const { return m_window->m_records[index].id; }
        void Click(int index, bool extend = false, bool toggle = false) {
            m_window->ClickRecord(index, extend, toggle);
        }
        std::vector<std::string> Selected() const { return m_window->SelectedIds(); }
        bool MultiSelect() const { return m_window->m_multiSelect; }
        void FilterTag(const std::string &tag) {
            m_window->m_query.scope = HistoryScope::Tag;
            m_window->m_query.value = tag;
            m_window->UpdateFilter(true);
        }
        void BeginDetailsTag(size_t index) { m_window->StartTagEdit({Id(index)}, HistoryWindow::TagEditor::Details); }
        void BeginSelectionTag() { m_window->StartTagEdit(Selected(), HistoryWindow::TagEditor::SelectionHeader); }
        void SetTag(const char *text) { std::snprintf(m_window->m_tagInput, sizeof(m_window->m_tagInput), "%s", text); }
        bool TagInputEmpty() const { return m_window->m_tagInput[0] == '\0'; }
        bool EditingTag() const { return m_window->m_tagEditor != HistoryWindow::TagEditor::None; }
        void FinishTag(bool save = true) { m_window->FinishTagEdit(save); }
        void FinishMultiSelect() { m_window->EndMultiSelect(); }
        std::vector<HistoryWindow::Request> Requests() { return m_window->TakeRequests(); }
        void HideDetails() { m_window->m_selected = -1; }
        std::vector<std::string> StitchOrder() {
            m_window->SelectAllVisible(true);
            m_window->OpenStitch();
            return m_window->m_stitchOrder;
        }
        std::set<std::string> AvailableTagsWithDeletedRecord() {
            m_window->m_records.back().details.deletedAtUnixMs = 1;
            m_window->m_records.back().details.tags = {"Deleted only"};
            return m_window->AvailableTags();
        }
        void Configure(float scale, Theme theme) {
            m_window->m_uiScale = scale;
            m_window->m_theme = theme;
            m_window->ApplyStyle();
            ImGui::GetIO().DisplaySize = ImVec2(1280 * scale, 820 * scale);
        }
        void RenderContent() {
            ImGui::NewFrame();
            const auto styleVariables = ImGui::GetCurrentContext()->StyleVarStack.Size;
            const auto colors = ImGui::GetCurrentContext()->ColorStack.Size;
            m_window->RenderContent();
            EXPECT_EQ(ImGui::GetCurrentContext()->StyleVarStack.Size, styleVariables);
            EXPECT_EQ(ImGui::GetCurrentContext()->ColorStack.Size, colors);
            EXPECT_EQ(ImGui::GetCurrentContext()->CurrentWindowStack.Size, 1);
            ImGui::Render();
        }
        float ToolbarHeight(float width) {
            ImGui::NewFrame();
            ImGui::Begin("selection-header-test");
            const auto height = m_window->RenderSelectionHeader(width);
            ImGui::End();
            ImGui::Render();
            return height;
        }
        ImRect RenderHeader() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2());
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("header-test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            m_window->RenderHeader(ImGui::GetIO().DisplaySize.x);
            const ImRect listButton(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::End();
            ImGui::Render();
            return listButton;
        }
        void ClickHeader(ImVec2 point) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            RenderHeader();
            io.AddMouseButtonEvent(0, false);
            RenderHeader();
        }
        bool ListView() const { return m_window->m_listView; }
        std::string SearchText() const { return m_window->m_search; }
        std::string SearchQuery() const { return m_window->m_query.search; }
        std::vector<std::string> VisibleRecordIds() const {
            std::vector<std::string> ids;
            for (size_t index : m_window->m_visible) ids.push_back(Id(index));
            return ids;
        }
        static constexpr float HeaderHeight() { return HistoryWindow::HeaderLogicalHeight; }
        const std::vector<ImRect>& HeaderControls() const { return m_window->m_headerControlRects; }
        bool HeaderDragAt(ImVec2 point) const {
            return m_window->IsHeaderDragArea(point, ImGui::GetIO().DisplaySize.x);
        }
        void MarkFavorite(size_t index) { m_window->m_records[index].details.favorite = true; }
        ImRect RenderSingleCard(int index) {
            const ImVec2 size(220, 148);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2());
            ImGui::SetNextWindowSize(ImVec2(640, 400));
            ImGui::Begin("card-test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::SetCursorPos(ImVec2(20, 40));
            const auto position = ImGui::GetCursorScreenPos();
            m_window->RenderCard(index, size, false);
            ImGui::End();
            ImGui::Render();
            return ImRect(position, ImVec2(position.x + size.x, position.y + size.y));
        }
        std::filesystem::path m_root;
        std::unique_ptr<HistoryService> m_service;
        std::unique_ptr<HistoryWindow> m_window;
        ImGuiContext *m_context = nullptr;
        ImGuiContext *m_previousContext = nullptr;
    };

#if defined(SHOTLET_PLATFORM_WINDOWS)
    TEST_F(HistoryWindowTest, HeaderUsesSharedTitleBarHeight) {
        EXPECT_FLOAT_EQ(HeaderHeight(), WindowChrome::TitleBarHeight);
    }
#endif

    TEST_F(HistoryWindowTest, HeaderSearchTrimsQueryButKeepsSpacesForNextKeyword) {
        SeedRecords();
        RenderHeader(); RenderHeader();
        ClickHeader(HeaderControls().front().GetCenter());
        auto& io = ImGui::GetIO();
        io.AddInputCharactersUTF8("  sCrEeNsHoT  ");
        RenderHeader();
        EXPECT_EQ(SearchQuery(), "sCrEeNsHoT");
        EXPECT_EQ(SearchText(), "  sCrEeNsHoT  ");
        EXPECT_EQ(VisibleRecordIds().size(), 4u);

        io.AddInputCharactersUTF8("1  ");
        RenderHeader();
        EXPECT_EQ(SearchQuery(), "sCrEeNsHoT  1");
        EXPECT_EQ(SearchText(), "  sCrEeNsHoT  1  ");
        EXPECT_EQ(VisibleRecordIds(), (std::vector<std::string>{Id(1)}));
    }

    TEST_F(HistoryWindowTest, HeaderDragUsesEventCoordinatesInsteadOfPreviousHover) {
        constexpr size_t HeaderControlCount = 4; // 搜索框、排序框、网格按钮、列表按钮。
        constexpr std::array DpiScales{1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f};
        for (float scale : DpiScales) {
            SCOPED_TRACE(scale);
            Configure(scale, Theme::Light);
            RenderHeader(); RenderHeader();
            ASSERT_EQ(HeaderControls().size(), HeaderControlCount);
            const float height = HeaderHeight() * scale;
            const ImVec2 blankPoint(HeaderControls().front().Min.x / 2, height / 2);
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(blankPoint.x, blankPoint.y);
            RenderHeader(); RenderHeader();
            ASSERT_FALSE(ImGui::IsAnyItemHovered());
            // 原生命中先于新一帧执行，不能把快速移入搜索、排序或视图按钮的点击当成拖动。
            for (const auto &rect : HeaderControls())
                EXPECT_FALSE(HeaderDragAt(rect.GetCenter()));
            const ImVec2 chromePoint(io.DisplaySize.x - WindowChrome::WindowControlsWidth(scale, true) / 2, height / 2);
            EXPECT_FALSE(HeaderDragAt(chromePoint));
            EXPECT_FALSE(HeaderDragAt(ImVec2(blankPoint.x, height)));
            EXPECT_TRUE(HeaderDragAt(blankPoint));

            const auto searchPoint = HeaderControls().front().GetCenter();
            io.AddMousePosEvent(searchPoint.x, searchPoint.y);
            RenderHeader();
            ASSERT_TRUE(ImGui::IsAnyItemHovered());
            EXPECT_TRUE(HeaderDragAt(blankPoint));
        }
    }

    TEST_F(HistoryWindowTest, RangeSelectionUsesVisibleOrderAndResolvesAnchorAfterFiltering) {
        SeedRecords();
        Click(0);
        Click(1, true);
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(0), Id(3), Id(1)}));
        EXPECT_TRUE(MultiSelect());
        FilterTag("Shared");
        EXPECT_TRUE(Selected().empty());
        Click(0, true);
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(0)}));
        Click(2, true);
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(0), Id(2)}));
    }
    TEST_F(HistoryWindowTest, ToggleSelectionDoesNotLoseOtherSelectedRecords) {
        SeedRecords();
        Click(0);
        Click(1, false, true);
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(0), Id(1)}));
        Click(0, false, true);
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(1)}));
        EXPECT_TRUE(MultiSelect());
    }
    TEST_F(HistoryWindowTest, EndingMultiSelectCommitsTagToOriginalTargets) {
        SeedRecords();
        Click(0);
        Click(1, false, true);
        BeginSelectionTag();
        SetTag(" \t Project \r\n");
        FinishMultiSelect();
        const auto requests = Requests();
        ASSERT_EQ(requests.size(), 1u);
        EXPECT_EQ(requests[0].action, HistoryWindow::Action::AddTag);
        EXPECT_EQ(requests[0].ids, (std::vector<std::string>{Id(0), Id(1)}));
        EXPECT_EQ(requests[0].value, "Project");
        EXPECT_FALSE(EditingTag());
        EXPECT_FALSE(MultiSelect());
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(1)}));
    }
    TEST_F(HistoryWindowTest, SwitchingTagEditorCommitsBeforeReplacingTargets) {
        SeedRecords();
        BeginDetailsTag(0);
        SetTag("First");
        BeginDetailsTag(2);
        EXPECT_TRUE(TagInputEmpty());
        SetTag("Second");
        FinishTag();
        const auto requests = Requests();
        ASSERT_EQ(requests.size(), 2u);
        EXPECT_EQ(requests[0].ids, (std::vector<std::string>{Id(0)}));
        EXPECT_EQ(requests[0].value, "First");
        EXPECT_EQ(requests[1].ids, (std::vector<std::string>{Id(2)}));
        EXPECT_EQ(requests[1].value, "Second");
    }
    TEST_F(HistoryWindowTest, CancelledOrWhitespaceOnlyTagsDoNotQueueRequests) {
        SeedRecords();
        BeginDetailsTag(0);
        SetTag("discard");
        FinishTag(false);
        BeginDetailsTag(1);
        SetTag(" \t\r\n ");
        FinishTag();
        EXPECT_TRUE(Requests().empty());
        EXPECT_FALSE(EditingTag());
    }
    TEST_F(HistoryWindowTest, TagEditorDisappearingFromViewStillCommitsToOriginalRecord) {
        SeedRecords();
        BeginDetailsTag(0);
        SetTag("Hidden");
        HideDetails();
        RenderContent();
        const auto requests = Requests();
        ASSERT_EQ(requests.size(), 1u);
        EXPECT_EQ(requests[0].ids, (std::vector<std::string>{Id(0)}));
        EXPECT_EQ(requests[0].value, "Hidden");
        EXPECT_FALSE(EditingTag());
    }
    TEST_F(HistoryWindowTest, StitchOrderIsChronologicalAndStableForEqualTimestamps) {
        SeedRecords();
        EXPECT_EQ(StitchOrder(), (std::vector<std::string>{Id(1), Id(2), Id(3), Id(0)}));
    }
    TEST_F(HistoryWindowTest, AvailableTagsExcludeDeletedRecordsAndRemoveDuplicates) {
        SeedRecords();
        EXPECT_EQ(AvailableTagsWithDeletedRecord(), (std::set<std::string>{"Other", "Shared"}));
    }
    TEST_F(HistoryWindowTest, SelectionToolbarWrapsActionsAndPreservesScaledHeight) {
        SeedRecords();
        Click(0);
        Click(1, false, true);
        constexpr float singleRowHeight = 38;
        constexpr float wrappedRowHeight = 72;
        for (const float scale: {1.f, 1.25f, 1.5f, 2.f}) {
            Configure(scale, Theme::Light);
            EXPECT_FLOAT_EQ(ToolbarHeight(1200 * scale), singleRowHeight * scale);
            EXPECT_FLOAT_EQ(ToolbarHeight(300 * scale), wrappedRowHeight * scale);
        }
    }
    TEST_F(HistoryWindowTest, ViewButtonGapDoesNotTriggerModeSwitch) {
        RenderHeader();
        const auto listButton = RenderHeader();
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        ClickHeader(ImVec2(listButton.Min.x - gap * .5f, listButton.GetCenter().y));
        EXPECT_FALSE(ListView());
        ClickHeader(listButton.GetCenter());
        EXPECT_TRUE(ListView());
    }
    TEST_F(HistoryWindowTest, RenderingRestoresImGuiStyleInBothThemesAndAcrossScales) {
        SeedRecords();
        for (const auto theme: {Theme::Light, Theme::Dark}) {
            for (const float scale: {1.f, 1.25f, 1.5f, 2.f}) {
                Configure(scale, theme);
                RenderContent();
                RenderContent();
            }
        }
    }
    TEST_F(HistoryWindowTest, FavoriteButtonDoesNotAlsoSelectTheCard) {
        SeedRecords();
        Click(0);
        MarkFavorite(1);
        RenderSingleCard(1);
        const auto card = RenderSingleCard(1);
        constexpr float favoriteInset = 18;
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(card.Max.x - favoriteInset, card.Min.y + favoriteInset);
        io.AddMouseButtonEvent(0, true);
        RenderSingleCard(1);
        io.AddMouseButtonEvent(0, false);
        RenderSingleCard(1);
        const auto requests = Requests();
        ASSERT_EQ(requests.size(), 1u);
        EXPECT_EQ(requests[0].action, HistoryWindow::Action::Favorite);
        EXPECT_EQ(requests[0].id, Id(1));
        EXPECT_FALSE(requests[0].enabled);
        EXPECT_EQ(Selected(), (std::vector<std::string>{Id(0)}));
    }
    TEST(HistoryStringUtilsTest, TrimPreservesInnerWhitespaceAndUnicode) {
        EXPECT_EQ(StringUtils::Trim(" \t 项目 A \r\n"), "项目 A");
        EXPECT_EQ(StringUtils::Trim("A\t B"), "A\t B");
        EXPECT_TRUE(StringUtils::Trim(" \r\n\t ").empty());
        EXPECT_TRUE(StringUtils::Trim("").empty());
    }
} // namespace shotlet
