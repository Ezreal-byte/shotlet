#pragma once
#include "CursorManager.h"
#include "CaptureDocument.h"
#include "Layer.h"
#include "SmartRegionDetector.h"
#include "utils/Colors.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "utils/IStyle.h"
#include "ui/ImageEditorLayout.h"

namespace shotlet {
    // 绘图工具类型
    enum class ToolType {
        None,
        Rectangle, // 矩形
        Ellipse, // 椭圆
        Arrow, // 箭头
        Brush, // 画笔
        Mosaic, // 马赛克
        Highlight, // 高亮
        Text, // 文字
        Number, // 数字打点
        Watermark // 水印
    };

    // 选择区域的状态
    enum class SelectionState {
        None, // 没有选择
        Selecting, // 正在拖拽选择
        Selected, // 已选择（可以调整）
        Moving, // 正在移动选区
        Resizing, // 正在调整大小
        Drawing // 正在绘制图形
    };

    class Canvas {
    public:
        struct TextureUpdateRegion {
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;

            bool IsValid() const { return width > 0 && height > 0; }
        };

        using TextureUpdateCallback = std::function<void(const TextureUpdateRegion*)>;

        Canvas();

        ~Canvas() = default;

        // 设置 Tooltip 字体（由 ScreenOverlay 在渲染前传入）
        void SetTooltipFont(ImFont* font) { m_tooltipFont = font; }
        void SetUIScale(float scale);
        float GetDocumentRenderScale() const { return m_documentRenderScale; }

        // 调用方先 FlushPendingInput，再估计快照有效载荷，最后创建快照。
        // 后续历史服务还须额外预留临时对象、共享成图与编码工作区，不能将此值视为峰值预算。
        std::optional<size_t> EstimateDocumentSnapshotBytes(std::string& error) const;
        std::optional<size_t> DocumentSnapshotPixelBytes(std::string& error) const {
            int x, y, w, h;
            return GetDocumentCrop(x, y, w, h, error) ? DocumentPixelBytes(w, h) : std::nullopt;
        }
        std::optional<CaptureDocument> ExportDocument(std::string& error) const;
        std::optional<CaptureDocument> ExportCroppedDocument(ImRect crop, std::string& error);
        bool RestoreDocument(const CaptureDocument& document, ImTextureID texture, std::string& error);

        int GetMosaicBlockSize() const { return m_mosaicBlockSize; }
        void SetMosaicBlockSize(int pixels);
        bool SelectLayerById(uint64_t id);
        bool MoveMosaicLayer(uint64_t id, const ImVec2& delta);
        bool SetMosaicLayerBrushRadius(uint64_t id, float radius);
        bool DeleteLayerById(uint64_t id);
        uint64_t GetSelectedLayerId() const { return m_selectedLayer ? m_selectedLayer->GetId() : 0; }

        // 设置截图
        // pixelSize: 截图像素尺寸, displaySize: ImGui 逻辑点尺寸（pixelSize / scaleFactor）
        void SetScreenshot(ImTextureID texture, const ImVec2 &pixelSize, const ImVec2 &displaySize, const unsigned char *imageData = nullptr);
        void SetScreenshotOwned(ImTextureID texture, const ImVec2 &pixelSize, const ImVec2 &displaySize,
                                std::unique_ptr<unsigned char[]> imageData, int rowPitch);

        // 长图回填：清除旧标注、保持原始像素，按给定显示尺寸自动全选。
        void ReplaceScreenshotAndSelectAll(ImTextureID texture, const ImVec2& pixelSize,
            const ImVec2& displaySize, std::unique_ptr<unsigned char[]> data, int rowPitch);

        // 设置纹理更新回调（统一使用 SDL_GPU transfer buffer 上传）
        void SetTextureUpdateCallback(TextureUpdateCallback callback) { m_textureUpdateCallback = std::move(callback); }

        // 重置状态
        void Reset();

        // 主渲染
        void Render(ImDrawList *drawList);
        void RenderInEditor(ImDrawList* drawList, const ImageEditorTransform& transform, const ImRect& viewport);
        void SetImageEditing(bool enabled) { m_imageEditing = enabled; }
        bool IsImageEditing() const { return m_imageEditing; }
        void RotateImageCounterClockwise();
        int GetImageQuarterTurns() const { return m_imageQuarterTurns; }

        // 干净渲染（只渲染截图和图层，不渲染选框等 UI 元素，用于保存/导出）
        // renderOffset：渲染平移量（离屏只渲染选区时传 -selection.Min）
        void RenderClean(ImDrawList *drawList, const ImVec2& renderOffset = ImVec2(0, 0), const ImRect* region = nullptr);

        // 选区查询
        bool HasSelection() const { return m_state != SelectionState::None; }
        ImRect GetSelection() const { return GetSelectionRect(); }

        // 鼠标交互（由 EditorWindow 调用）
        void OnMouseDown(const ImVec2 &pos);

        void OnMouseMove(const ImVec2 &pos);

        void OnMouseUp(const ImVec2 &pos);

        // 双击交互（用于重新编辑 TextLayer）
        void OnMouseDoubleClick(const ImVec2 &pos);

        // 键盘交互（Delete/Backspace/方向键等，文字输入时使用）
        void OnKeyDown(int keyCode, int modifiers, bool repeat);

        // 文本输入状态
        bool IsTextInputActive() const { return m_textInput.active; }
        ImVec2 GetTextInputPosition() const { return m_textInput.position; }
        const std::string& GetTextInputText() const { return m_textInput.text; }
        // 获取 IME 光标像素位置（供 SDL_SetTextInputArea 使用）
        ImVec2 GetIMECursorPos() const { return m_imeCursorPos; }

        // 提交文本输入（由 ScreenOverlay 调用）
        void CommitTextInput();

        // 取消文本输入（ESC 取消，重新编辑时恢复原 TextLayer）
        void CancelTextInput();

        // 工具设置
        void SetTool(ToolType tool, bool clearSelection = true);
        ToolType GetTool() const { return m_currentTool; }
        // 属性工具栏跟随选中的标注；马赛克选中后仅允许删除。
        ToolType GetPropertyTool() const;
        void SetToolColor(ImU32 color) { m_toolColor = color; }
        ImU32 GetToolColor() const { return m_toolColor; }

        // 选中图层属性
        bool HasSelectedLayer() const { return m_selectedLayer != nullptr; }
        bool HasSelectedTextLayer() const { return m_selectedLayer && m_selectedLayer->SupportsFontSize(); }
        bool HasSelectedFillableLayer() const { return m_selectedLayer && m_selectedLayer->SupportsFill(); }
        float GetSelectedTextLayerFontSize() const;
        float GetTextFontSizeBase() const { return m_textFontSize / TEXT_FONT_SIZE_RATIOS[m_textFontSizeIndex]; }
        ImU32 GetSelectedLayerColor() const;
        void SetSelectedLayerColor(ImU32 color);
        float GetSelectedLayerThickness() const;
        void SetSelectedLayerThickness(float thickness);
        bool GetSelectedLayerFilled() const;
        void SetSelectedLayerFilled(bool filled);
        void ClearAllSelectedLayers();  // 清除选中状态

        // 重编辑状态访问
        bool IsReeditActive() const { return m_reedit.active; }
        ImU32 GetReeditColor() const { return m_reedit.color; }
        void SetToolThickness(float thickness) { m_toolThickness = thickness; }
        float GetToolThickness() const { return m_toolThickness; }

        void SetToolFill(bool fill) { m_toolFill = fill; }
        bool GetToolFill() const { return m_toolFill; }

        // 多行文本光标上下移动
        void MoveCursorVertically(int direction);  // direction: -1=上, 1=下

        // 粗细档位选项
        static constexpr float THICKNESS_OPTIONS[] = {2.0f, 4.0f, 6.0f};
        static constexpr int THICKNESS_COUNT = 3;

        // 马赛克笔刷半径档位（小/中/大，逻辑坐标）
        static constexpr float MOSAIC_BRUSH_OPTIONS[] = {8.0f, 10.0f, 16.0f};
        static constexpr int MOSAIC_BRUSH_COUNT = 3;
        void SetMosaicBrushIndex(int index);
        int GetMosaicBrushIndex() const { return m_mosaicBrushIndex; }
        float GetMosaicBrushRadius() const { return m_mosaicBrushRadius; }

        // 字号档位比例（小/中/大 = 0.7/1.0/1.4，中档 = GetTextLayerFontSize()）
        static constexpr float TEXT_FONT_SIZE_RATIOS[] = {0.7f, 1.0f, 1.4f};
        static constexpr int TEXT_FONT_SIZE_COUNT = 3;
        void SetTextFontSizeIndex(int index);
        int GetTextFontSizeIndex() const { return m_textFontSizeIndex; }
        float GetTextFontSize() const { return m_textFontSize; }
        float GetScaledTextFontSizeForIndex(int index) const;

        // 数字点尺寸在创建时换算为逻辑坐标，不随渲染重复缩放。
        static constexpr float NUMBER_DIAMETER_OPTIONS[] = {24.0f, 32.0f, 40.0f};
        static constexpr int NUMBER_SIZE_COUNT = 3;
        bool HasSelectedNumberLayer() const { return dynamic_cast<NumberLayer*>(m_selectedLayer) != nullptr; }
        void SetNumberSizeIndex(int index);
        int GetNumberSizeIndex() const;
        void SetHighlightEllipse(bool ellipse);
        bool GetHighlightEllipse() const;
        void SetHighlightTransparency(int percent);
        int GetHighlightTransparency() const { return m_highlightTransparency; }

        // 水印文字（子工具栏输入框编辑，按 UTF-8 字符数限制最多 10 个）
        static constexpr int WATERMARK_MAX_CHARS = WatermarkSettings::MaxCharacters;
        void SetWatermarkText(const std::string& text);
        const std::string& GetWatermarkText() const { return m_watermarkText; }
        void SetWatermarkOpacity(int percent);
        int GetWatermarkOpacity() const { return m_watermarkOpacity; }
        float GetNumberDiameter() const { return NUMBER_DIAMETER_OPTIONS[m_numberSizeIndex] * m_documentRenderScale; }

        // 预设颜色（引用 Colors 统一定义）
        static constexpr const ImU32* PRESET_COLORS = Colors::PRESET_COLORS;
        static constexpr int PRESET_COLOR_COUNT = Colors::PRESET_COLOR_COUNT;

        // 光标管理器（生命周期由 ScreenOverlay 管理）
        void SetCursorManager(CursorManager* cm) { m_cursorManager = cm; }
        CursorType GetLastCursorType() const { return m_lastCursorType; }

        // 撤回功能
        bool CanUndo() const { return !m_layerManager.IsEmpty(); }
        void Undo();

        // 刷新纹理（撤销后重新渲染所有图层）
        void RefreshTexture();

        // 条件性刷新纹理：非 Windows 平台执行 RefreshTexture()，Windows 跳过（由渲染管线处理）
        void ConditionalRefreshTexture();

        // 更新 GPU 纹理
        void UpdateTexture() const;
        void UpdateTextureRegion(const ImRect& regionPx) const;

        // 获取图像数据副本（用于同步到 EditorWindow）
        std::unique_ptr<unsigned char[]> GetImageDataCopy(int &rowPitch) const;

        // 智能区域识别
        void SetSmartRegion(const SmartRegion* region);
        const SmartRegion* GetSmartRegion() const { return m_smartRegion; }
        bool IsWindowCapture() const {
            const auto selection = GetSelectionRect();
            return !m_captureWindowLabel.empty() && selection.Min.x == m_captureWindowBounds.Min.x &&
                selection.Min.y == m_captureWindowBounds.Min.y && selection.Max.x == m_captureWindowBounds.Max.x &&
                selection.Max.y == m_captureWindowBounds.Max.y;
        }
        const std::string& GetCaptureWindowLabel() const { return m_captureWindowLabel; }
        void ClearSmartRegion();

        // 获取当前图像数据（包含马赛克等直接像素修改）
        const unsigned char* GetCurrentImageData() const { return m_imageData.get(); }
        int GetCurrentRowPitch() const { return m_imageRowPitch; }
        ImVec2 GetScreenshotSize() const { return m_screenshotSize; }
        ImVec2 GetDisplaySize() const { return m_displaySize; }
        ImVec2 GetPixelScale() const {
            if (m_displaySize.x <= 0 || m_displaySize.y <= 0) return ImVec2(1, 1);
            return m_documentScaleLocked ? ImVec2(m_documentPixelScale.x, m_documentPixelScale.y)
                : ImVec2(m_screenshotSize.x / m_displaySize.x, m_screenshotSize.y / m_displaySize.y);
        }
        void FreezeDocumentScale() {
            const auto scale = GetPixelScale();
            m_documentPixelScale = {scale.x, scale.y}; m_documentScaleLocked = true;
        }
        ImVec2 ToPixel(ImVec2 p) const {
            const auto s = GetPixelScale();
            return ImVec2(p.x * s.x - m_sampleOffset.x, p.y * s.y - m_sampleOffset.y);
        }
        bool HasFractionalSampling() const;
        bool HasFractionalSampling(ImRect region) const;
        LayerManager& GetLayerManager() { return m_layerManager; }
        const LayerManager& GetLayerManager() const { return m_layerManager; }

        // 提交未完成的输入（保存/确认前调用）
        void FlushPendingInput();

    private:
        // 渲染各个部分
        void RenderScreenshot(ImDrawList *drawList) const;
        void RenderImageRegion(ImDrawList* drawList, ImRect region, ImVec2 offset) const;

        void RenderDimmedRegion(ImDrawList *drawList) const;

        void RenderSelectionBorder(ImDrawList *drawList) const;

        void RenderResizeHandles(ImDrawList *drawList) const;

        void RenderSizeTooltip(ImDrawList *drawList) const;

        void RenderCurrentShape(ImDrawList *drawList) const; // 渲染正在绘制的图形

        // 渲染智能区域高亮框
        void RenderSmartRegionBorder(ImDrawList *drawList) const;

        // 渲染文本输入框
        void RenderTextInput();
        void RenderHighlightMask(ImDrawList* drawList, ImVec2 offset, ImRect region) const;
        // 水印文字 45° 斜向平铺覆盖选区（预览与导出共用）
        void RenderWatermark(ImDrawList* drawList, ImVec2 offset, ImRect region) const;
        float GetWatermarkFontSize() const;

        // 处理文本输入队列（渲染文本输入框前调用）
        void ProcessTextInput();

        // 工具函数（private）
        ImRect GetSelectionRect() const;

        ResizeHandle HitTestResizeHandle(const ImVec2 &pos) const;

        ImVec2 ClampToCanvas(const ImVec2 &pos) const;

        // 将坐标限制在选区范围内
        ImVec2 ClampToSelection(const ImVec2 &pos) const;

        // 更新绘制内容边界（在图层提交/撤销后调用）
        void UpdateDrawnContentBounds(const Layer* layer);

        // 重新计算所有图层的绘制内容边界（撤销后使用）
        void RecalculateDrawnContentBounds();

        // 获取绘制内容边界约束后的选区（移动/缩放时调用）
        ImRect ConstrainSelectionToBounds(const ImRect& sel) const;

        static ImU32 DarkenColor(ImU32 color, float factor);

        // 编辑缓冲（original/mosaic/mask 共用的像素区域，全局像素坐标，m_editW==0 表示未建立）：
        // 所有像素级编辑都限制在选区内，缓冲只覆盖"选区外扩最大画笔半径"，
        // 5K 屏全图编辑缓冲 ~126MB → 选区大小。区域只增不减；扩展时
        // original/mosaic 搬迁旧数据，新增部分从 m_imageData 取原始像素（新增区未被编辑过）
        void EnsureEditRegion(const ImRect& regionPx);
        // 在当前编辑区域上计算 mosaic 块平均并初始化 mask（首次涂抹前调用）
        void EnsureMosaicBuffers();

        // 在 mask 上画径向渐变圆（涂抹笔刷），brushRadius 逻辑坐标单位
        void PaintMosaicMask(const ImVec2& point, float brushRadius);

        // 在指定像素区域合成 original + mosaic → imageData
        void CompositeMosaicRegion(const ImRect& regionPx);

        // RefreshTexture 的受影响区域（选区外扩最大画笔半径，像素坐标）：
        // 所有绘制都限制在选区内，画笔从选区边界外溢最多一个半径
        ImRect ComputeRefreshRegionPx() const;
        // 区域内从原始图恢复 / 重建区域 mask
        void RestoreRegionFromOriginal(const ImRect& regionPx);
        void RebuildMaskRegion(const ImRect& regionPx);
        bool GetDocumentCrop(int& x, int& y, int& width, int& height, std::string& error) const;

        // 验证 m_selectedLayer 是否仍在图层列表中
        bool ValidateSelectedLayer();

        void FinalizeCurrentLayer();

        // 马赛克块大小
        int m_mosaicBlockSize = 18;
        int m_mosaicGridOffsetX = 0;
        int m_mosaicGridOffsetY = 0;
        int m_numberSizeIndex = 1; // 数字点默认中档

        // 马赛克软遮罩相关
        float m_mosaicBrushRadius = 10.0f;       // 笔刷半径（逻辑坐标），默认与 m_mosaicBrushIndex=1 对应
        int m_mosaicBrushIndex = 1;               // 笔刷档位索引（0=小, 1=中, 2=大）
        // 以下三个缓冲均为 m_editX/Y/W/H 子矩形的行主序存储（pitch = m_editW 或 m_editW*4）
        std::unique_ptr<unsigned char[]> m_mosaicImageData;  // 区域预计算马赛克（RGBA）
        std::unique_ptr<unsigned char[]> m_maskData;         // 单通道灰度遮罩（0=原图, 255=完全马赛克）
        int m_editX = 0;             // 编辑缓冲区域原点（全局像素坐标）
        int m_editY = 0;
        int m_editW = 0;             // 0 = 编辑缓冲未建立
        int m_editH = 0;
        ImVec2 m_lastMosaicPoint;      // 上一个涂抹点（插值用）
        bool m_hasLastMosaicPoint = false;

        // 选区状态
        bool m_imageEditing = false; // 编辑窗口中固定全图选区，不响应截图选框的移动/缩放
        int m_imageQuarterTurns = 0;
        SelectionState m_state = SelectionState::None;
        ImVec2 m_selectionStart;
        ImVec2 m_selectionEnd;
        ImVec2 m_dragStart; // 拖拽起始点
        ImRect m_selectionBeforeDrag; // 拖拽前的选区
        ResizeHandle m_activeHandle = ResizeHandle::None;

        // ─── 统一选中图层状态 ───
        Layer* m_selectedLayer = nullptr;  // 当前选中的图层（替代 5 个分类型指针）
        bool m_layerDragging = false;      // 是否正在拖拽图层移动

        // 文本输入状态
        struct TextInputState {
            bool active = false;
            ImVec2 position;
            std::string text;
            int cursorPos = 0;         // 光标位置（字节偏移）
            int selectionStart = -1;   // 选中起始（字节偏移），-1 = 无选中
            int selectionEnd = -1;     // 选中结束（字节偏移），-1 = 无选中
            bool focusRequested = false; // 首帧自动获取焦点
            float fontSize = 0.0f;     // 独立字号，不受全局切换影响
            bool selecting = false;    // 鼠标拖拽选中中
            float wrapWidth = 0.0f;    // 自动换行宽度，提交时固定
        };
        TextInputState m_textInput;

        // 文本选中辅助方法
        int GetCharIndexFromPixelPos(ImVec2 pixelPos) const;  // 屏幕像素→文本字节偏移
        bool HandleTextInputClick(const ImVec2& pos);         // 文本输入点击检测（由 OnMouseDown 调用）
        ImRect GetTextInputFrameBounds() const;
        NumberLayer* GetEditingNumberLayer() const;
        void BeginNumberLabelInput(NumberLayer& marker);
        void UpdateNumberLabelInput();
        bool HasTextSelection() const;
        std::string GetSelectedText() const;
        void DeleteSelectedText();
        void InsertText(const std::string& text);

        // 重新编辑状态（双击 TextLayer 时记录原始信息，ESC 取消时恢复）
        struct ReeditState {
            bool active = false;
            bool numberLabel = false;
            uint64_t layerId = 0;
            ImVec2 position;
            std::string text;
            ImU32 color;
            float fontSize;
            float wrapWidth;
        };
        ReeditState m_reedit;

        // 纹理更新回调（由 ScreenOverlay 设置，替代直接 OpenGL 调用）
        TextureUpdateCallback m_textureUpdateCallback;

        // 截图
        ImTextureID m_screenshot = 0;
        ImVec2 m_screenshotSize;       // 像素尺寸（用于图像数据操作）
        ImVec2 m_displaySize;          // 逻辑点尺寸（用于 ImGui 渲染）

        // 原始图像数据副本（m_editX/Y/W/H 子矩形行主序，pitch = m_editW*4），
        // 首次编辑时延迟创建
        mutable std::unique_ptr<unsigned char[]> m_originalImageData;

        // 当前图像数据（包含所有已应用的图层效果）
        std::unique_ptr<unsigned char[]> m_imageData;
        int m_imageRowPitch = 0;

        // 鼠标状态
        ImVec2 m_mousePos;
        bool m_mouseDown = false;

        // 工具状态
        ToolType m_currentTool = ToolType::None;
        bool m_highlightEllipse = false;
        int m_highlightTransparency = HighlightSettings::DefaultTransparency;
        std::string m_watermarkText; // 水印文字（跨截图保留，类似工具颜色/粗细设置）
        int m_watermarkOpacity = WatermarkSettings::DefaultOpacity;
        float m_watermarkFontSize = 0;
        ImVec2 m_watermarkOrigin{};
        ImU32 m_toolColor = Colors::ToolDefault; // 默认红色
        float m_toolThickness = 4.0f;
        bool m_toolFill = false;  // 填充开关（仅矩形/椭圆有效）
        float m_textFontSize = 0.0f;  // 文字字号（初始化时由 GetTextLayerFontSize() 设置）
        int m_textFontSizeIndex = 1;  // 字号档位索引（0=小, 1=中, 2=大）
        ImVec2 m_imeCursorPos = ImVec2(0, 0);  // IME 光标像素位置（每帧在 RenderTextInput 中更新）
        std::string m_lastInputStr;  // 上次输入的字符串（用于长按 repeat 复用）
        ImFont* m_tooltipFont = nullptr;  // Tooltip 字体（由 ScreenOverlay 传入）
        float m_uiScale = 1.0f;
        float m_documentRenderScale = 1.0f;
        bool m_documentScaleLocked = false; // 恢复文档后，窗口 DPI 只影响操作界面
        DocumentPoint m_documentCropOrigin;
        DocumentPoint m_documentPixelScale{1, 1};
        DocumentPoint m_sampleOffset;

        bool m_justDeselected = false;  // 本帧刚取消图层选中，不绘制新图案
        bool m_skipTextLayerSelect = false;  // 取消选中 TextLayer 后，下次点击跳过 TextLayer 选中检测
        LayerManager m_layerManager;

        CursorManager* m_cursorManager = nullptr;   // 光标管理器（生命周期由 ScreenOverlay 管理）
        CursorType m_lastCursorType = CursorType::Arrow;  // 上一帧的光标类型（供 ScreenOverlay 读取）

        ImRect m_layerPreDragBounds;  // 拖拽前图层边界，用于选区约束

        // 智能区域（外部指针，生命周期由 ScreenOverlay 的 SmartRegionDetector 管理）
        const SmartRegion* m_smartRegion = nullptr;
        std::string m_captureWindowLabel;
        ImRect m_captureWindowBounds;

        // 绘制内容边界（屏幕坐标），用于限制选区移动/缩放时不能裁掉绘制内容
        // 无绘制内容时 m_hasDrawnContent = false
        bool m_hasDrawnContent = false;
        ImRect m_drawnContentBounds;  // 屏幕坐标

        // 当前正在绘制的图形（用于实时预览）
        std::unique_ptr<Layer> m_currentLayer;

        // 当前绘图的起点（用于矩形、椭圆、箭头等拖拽工具）
        ImVec2 m_drawStart;

        // 配置
        static constexpr float HANDLE_SIZE = 8.0f;
        float GetSelectionBorderWidth() const { return IStyle::SelectionBorderWidth * m_uiScale; }
        float GetSelectionHandleSize() const { return IStyle::SelectionHandleSize * m_uiScale; }
        float GetSelectionHandleHitSize() const { return (HANDLE_SIZE + 2.0f) * m_uiScale; }
        float GetTextFrameScale() const {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            return m_uiScale;
#else
            return 1.0f;
#endif
        }

        // 内边距
        const float padding = 5.0f;
    };
} // namespace shotlet
