#pragma once
#include <string>
#include "utils/Colors.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <cstdint>
#include <vector>
#include <utility>

namespace shotlet {

// 计算文本中 ImGui 不计高度的空行数量（用于补偿 textSize.y）
int CountEmptyLines(const std::string& text);

// 调整手柄位置（需在 Layer.h 中定义，供 Layer 虚函数使用）
enum class ResizeHandle {
    None,
    TopLeft, Top, TopRight,
    Left, Right,
    BottomLeft, Bottom, BottomRight
};

// 图层基类
class Layer {
public:
    virtual ~Layer() = default;

    // 获取类型
    virtual const char* GetTypeName() const = 0;

    // 渲染到 ImDrawList（预览用）
    virtual void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) = 0;

    // 渲染到图像数据（导出用）
    virtual void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                               int rowPitch, const ImVec2& canvasOffset,
                               float scaleFactor = 1.0f) = 0;

    // 获取边界框
    virtual ImRect GetBounds() const = 0;
    // 复合标注可只移动其中一部分，拖动约束必须使用实际移动部分的边界。
    virtual ImRect GetDragBounds() const { return GetBounds(); }

    // 克隆（用于复制）
    virtual std::unique_ptr<Layer> Clone() const = 0;

    uint64_t GetId() const { return m_id; }

    // ─── 选中状态 ───
    bool IsSelected() const { return m_selected; }
    void SetSelected(bool selected) { m_selected = selected; }

    // ─── 通用属性 ───
    ImU32 GetColor() const { return m_color; }
    void SetColor(ImU32 color) { m_color = color; }

    float GetThickness() const { return m_thickness; }
    void SetThickness(float thickness) { m_thickness = thickness; }

    // ─── 子类特有属性的通用虚接口 ───
    // 填充（矩形/椭圆支持，其他不支持）
    virtual bool SupportsFill() const { return false; }
    virtual bool IsFilled() const { return false; }
    virtual void SetFilled(bool filled) {}

    // 字号（文字支持，其他不支持）
    virtual bool SupportsFontSize() const { return false; }
    virtual float GetFontSize() const { return 0.0f; }
    virtual void SetFontSize(float size) {}

    // ─── 拖拽移动虚接口 ───
    virtual void StartDrag(const ImVec2& mousePos) {}
    virtual void UpdateDrag(const ImVec2& delta) {}
    virtual void EndDrag() {}
    virtual bool IsDragging() const { return false; }

    // ─── Resize 手柄虚接口 ───
    virtual ResizeHandle HitTestResizeHandle(const ImVec2& pos) const { return ResizeHandle::None; }
    virtual void StartResize(ResizeHandle handle) {}
    virtual void UpdateResize(const ImVec2& delta, const ImRect& selection) {}
    virtual void EndResize() {}
    virtual ResizeHandle GetResizeHandle() const { return ResizeHandle::None; }

    // ─── 渲染选中 UI（边框 + 手柄）───
    virtual void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const {}

    // ─── 点击检测 ───
    // 检测点是否在图层边框/边线附近（用于选中判断）
    virtual bool IsPointNearBorder(const ImVec2& pos, float threshold) const { return false; }

protected:
    Layer() = default;
    Layer(ImU32 color, float thickness) : m_color(color), m_thickness(thickness) {}

    ImU32 m_color = Colors::ToolDefault;
    float m_thickness = 2.0f;
    bool m_selected = false;

private:
    friend class Canvas;
    friend class LayerManager;
    uint64_t m_id = 0;
};

// 矩形图层
class RectangleLayer : public Layer {
public:
    RectangleLayer(ImVec2 start, ImVec2 end, ImU32 color, float thickness);

    const char* GetTypeName() const override { return "Rectangle"; }

    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                       int rowPitch, const ImVec2& canvasOffset, float scaleFactor = 1.0f) override;

    ImRect GetBounds() const override;
    std::unique_ptr<Layer> Clone() const override;

    // 填充属性
    bool SupportsFill() const override { return true; }
    void SetFilled(bool filled) override { m_filled = filled; }
    bool IsFilled() const override { return m_filled; }

    // 位置
    void SetShowBorder(bool show) { m_showBorder = show; }
    void SetStart(const ImVec2& start) { m_start = start; }
    void SetEnd(const ImVec2& end) { m_end = end; }
    const ImVec2& GetStart() const { return m_start; }
    const ImVec2& GetEnd() const { return m_end; }

    // 拖拽
    void StartDrag(const ImVec2& mousePos) override;
    void UpdateDrag(const ImVec2& delta) override;
    void EndDrag() override;
    bool IsDragging() const override { return m_dragging; }

    // Resize 手柄
    ResizeHandle HitTestResizeHandle(const ImVec2& pos) const override;
    void StartResize(ResizeHandle handle) override;
    void UpdateResize(const ImVec2& delta, const ImRect& selection) override;
    void EndResize() override;
    ResizeHandle GetResizeHandle() const override { return m_resizeHandle; }

    // 选中 UI
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;

    // 点击检测
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;

private:
    ImVec2 m_start;
    ImVec2 m_end;
    bool m_showBorder = true;
    bool m_filled = false;

    // 拖拽状态
    bool m_dragging = false;
    ImVec2 m_dragStartMouse;      // 拖拽起始鼠标位置
    ImVec2 m_posStartBeforeDrag;  // 拖拽前 start
    ImVec2 m_posEndBeforeDrag;    // 拖拽前 end

    // Resize 状态
    ResizeHandle m_resizeHandle = ResizeHandle::None;
    ImRect m_boundsBeforeResize;
};

// 高亮复用矩形的边界、拖动和缩放；遮罩由画布合并所有高亮区域后统一绘制。
class HighlightLayer : public RectangleLayer {
public:
    HighlightLayer(ImVec2 start, ImVec2 end, bool ellipse);
    const char* GetTypeName() const override { return "Highlight"; }
    void Render(ImDrawList*, const ImVec2&, float = 1) override {}
    void RenderToImage(unsigned char*, int, int, int, const ImVec2&, float = 1) override {}
    bool SupportsFill() const override { return false; }
    bool IsEllipse() const { return m_ellipse; }
    void SetEllipse(bool ellipse) { m_ellipse = ellipse; }
    bool IsPointNearBorder(const ImVec2& point, float threshold) const override;
    std::unique_ptr<Layer> Clone() const override;
private:
    bool m_ellipse;
};

// 椭圆图层
class EllipseLayer : public Layer {
public:
    EllipseLayer(ImVec2 start, ImVec2 end, ImU32 color, float thickness);

    const char* GetTypeName() const override { return "Ellipse"; }

    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                       int rowPitch, const ImVec2& canvasOffset, float scaleFactor = 1.0f) override;

    ImRect GetBounds() const override;
    std::unique_ptr<Layer> Clone() const override;

    // 填充属性
    bool SupportsFill() const override { return true; }
    void SetFilled(bool filled) override { m_filled = filled; }
    bool IsFilled() const override { return m_filled; }

    // 位置
    void SetShowBorder(bool show) { m_showBorder = show; }
    void SetStart(const ImVec2& start) { m_start = start; }
    void SetEnd(const ImVec2& end) { m_end = end; }
    const ImVec2& GetStart() const { return m_start; }
    const ImVec2& GetEnd() const { return m_end; }

    // 拖拽
    void StartDrag(const ImVec2& mousePos) override;
    void UpdateDrag(const ImVec2& delta) override;
    void EndDrag() override;
    bool IsDragging() const override { return m_dragging; }

    // Resize 手柄
    ResizeHandle HitTestResizeHandle(const ImVec2& pos) const override;
    void StartResize(ResizeHandle handle) override;
    void UpdateResize(const ImVec2& delta, const ImRect& selection) override;
    void EndResize() override;
    ResizeHandle GetResizeHandle() const override { return m_resizeHandle; }

    // 选中 UI
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;

    // 点击检测
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;

private:
    ImVec2 m_start;
    ImVec2 m_end;
    bool m_showBorder = true;
    bool m_filled = false;

    // 拖拽状态
    bool m_dragging = false;
    ImVec2 m_dragStartMouse;
    ImVec2 m_posStartBeforeDrag;
    ImVec2 m_posEndBeforeDrag;

    // Resize 状态
    ResizeHandle m_resizeHandle = ResizeHandle::None;
    ImRect m_boundsBeforeResize;
};

// 箭头图层
class ArrowLayer : public Layer {
public:
    ArrowLayer(ImVec2 start, ImVec2 end, ImU32 color, float thickness);

    const char* GetTypeName() const override { return "Arrow"; }

    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                       int rowPitch, const ImVec2& canvasOffset, float scaleFactor = 1.0f) override;

    ImRect GetBounds() const override;
    std::unique_ptr<Layer> Clone() const override;

    // 位置
    void SetShowHandles(bool show) { m_showHandles = show; }
    void SetStart(const ImVec2& start) { m_start = start; }
    void SetEnd(const ImVec2& end) { m_end = end; }
    const ImVec2& GetStart() const { return m_start; }
    const ImVec2& GetEnd() const { return m_end; }

    // 拖拽
    void StartDrag(const ImVec2& mousePos) override;
    void UpdateDrag(const ImVec2& delta) override;
    void EndDrag() override;
    bool IsDragging() const override { return m_dragging; }

    // 手柄（起点/终点 2 个）
    ResizeHandle HitTestResizeHandle(const ImVec2& pos) const override;
    void StartResize(ResizeHandle handle) override;
    void UpdateResize(const ImVec2& delta, const ImRect& selection) override;
    void EndResize() override;
    ResizeHandle GetResizeHandle() const override { return m_resizeHandle; }

    // 选中 UI
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;

    // 点击检测
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;

    // 箭头手柄检测：0=无，1=起点，2=终点（用于 Canvas 内部）
    int HitTestArrowHandle(const ImVec2& pos) const;

private:
    ImVec2 m_start;
    ImVec2 m_end;
    bool m_showHandles = true;  // 绘制过程中显示手柄圆点

    // 拖拽状态
    bool m_dragging = false;
    ImVec2 m_dragStartMouse;
    ImVec2 m_posStartBeforeDrag;
    ImVec2 m_posEndBeforeDrag;

    // Resize 状态（起点/终点手柄）
    ResizeHandle m_resizeHandle = ResizeHandle::None;
    int m_arrowHandlePoint = 0;  // 0=无，1=起点，2=终点
    ImVec2 m_posStartBeforeResize;
    ImVec2 m_posEndBeforeResize;
};

// 画笔图层
class BrushLayer : public Layer {
public:
    BrushLayer(std::vector<ImVec2> points, ImU32 color, float thickness);

    const char* GetTypeName() const override { return "Brush"; }

    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                       int rowPitch, const ImVec2& canvasOffset, float scaleFactor = 1.0f) override;

    ImRect GetBounds() const override;
    std::unique_ptr<Layer> Clone() const override;

    void AddPoint(ImVec2 point) { m_points.push_back(point); }
    const std::vector<ImVec2>& GetPoints() const { return m_points; }
    void OffsetAllPoints(const ImVec2& offset);

    // 拖拽
    void StartDrag(const ImVec2& mousePos) override;
    void UpdateDrag(const ImVec2& delta) override;
    void EndDrag() override;
    bool IsDragging() const override { return m_dragging; }

    // 选中 UI
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;

    // 点击检测
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;

private:
    std::vector<ImVec2> m_points;

    // 拖拽状态
    bool m_dragging = false;
    ImRect m_boundsBeforeDrag;
    ImVec2 m_totalDelta;
};

// 马赛克图层
class MosaicLayer : public Layer {
public:
    MosaicLayer(std::vector<ImVec2> points, int blockSize = 15, float brushRadius = 15.0f);

    const char* GetTypeName() const override { return "Mosaic"; }

    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                       int rowPitch, const ImVec2& canvasOffset, float scaleFactor = 1.0f) override;

    ImRect GetBounds() const override;
    std::unique_ptr<Layer> Clone() const override;
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;

    void AddPoint(ImVec2 point) { m_points.push_back(point); }
    const std::vector<ImVec2>& GetPoints() const { return m_points; }
    float GetBrushRadius() const { return m_brushRadius; }
    void SetBrushRadius(float radius) { m_brushRadius = radius; }
    void OffsetAllPoints(const ImVec2& offset);

private:
    std::vector<ImVec2> m_points;
    int m_blockSize = 15;
    float m_brushRadius = 15.0f;  // 笔刷半径（逻辑坐标）
};

// 文本图层
class TextLayer : public Layer {
public:
    static constexpr float FramePadding = 5.0f;

    TextLayer(ImVec2 position, const std::string& text, ImU32 color, float fontSize, float wrapWidth = 0.0f);

    const char* GetTypeName() const override { return "Text"; }

    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                       int rowPitch, const ImVec2& canvasOffset, float scaleFactor = 1.0f) override;

    ImRect GetBounds() const override;
    ImRect GetFrameBounds(float uiScale = 1.0f) const;
    std::unique_ptr<Layer> Clone() const override;

    // 字号属性
    bool SupportsFontSize() const override { return true; }
    float GetFontSize() const override { return m_fontSize; }
    void SetFontSize(float size) override { m_fontSize = size; }

    void SetText(const std::string& text);
    const std::string& GetText() const { return m_text; }
    void SetPosition(ImVec2 pos) { m_position = pos; }
    ImVec2 GetPosition() const { return m_position; }
    void SetWrapWidth(float w) { m_wrapWidth = w; }
    float GetWrapWidth() const { return m_wrapWidth; }

    // 标记为已提交（失焦后调用）
    void Commit() { m_committed = true; }
    bool IsCommitted() const { return m_committed; }

    // 拖拽
    void StartDrag(const ImVec2& mousePos) override;
    void UpdateDrag(const ImVec2& delta) override;
    void EndDrag() override;
    bool IsDragging() const override { return m_dragging; }

    // 选中 UI
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;

    // 点击检测
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;

private:
    ImVec2 m_position;
    std::string m_text;
    float m_fontSize;
    float m_wrapWidth = 0.0f;  // 自动换行宽度，0 = 不换行
    bool m_committed = true;  // 创建时即已提交（失焦后）

    // 拖拽状态
    bool m_dragging = false;
    ImVec2 m_dragStartMouse;
    ImVec2 m_posBeforeDrag;
};

// 数字打点图层：圆点与说明框分别定位，连接点随相对位置重新计算。
class NumberLayer : public Layer {
public:
    NumberLayer(ImVec2 center, int number, ImU32 color, float diameter);
    const char* GetTypeName() const override { return "Number"; }
    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f) override;
    void RenderToImage(unsigned char*, int, int, int, const ImVec2&, float = 1.0f) override;
    ImRect GetBounds() const override;
    ImRect GetCircleBounds() const;
    bool ContainsCircle(ImVec2 point) const;
    ImRect GetDragBounds() const override;
    void InitializeLabel(const ImRect& selection, float fontSize, float frameScale);
    void SetLabel(ImVec2 position, const std::string& text, float fontSize, float wrapWidth, float frameScale);
    TextLayer* GetLabel() { return m_label.get(); }
    const TextLayer* GetLabel() const { return m_label.get(); }
    // 空说明框仅在编辑或选中时显示，有文字时常驻显示。
    bool IsLabelVisible() const;
    ImRect GetLabelFrameBounds() const;
    float GetLabelFrameScale() const { return m_labelFrameScale; }
    void SetLabelEditing(bool editing) { m_labelEditing = editing; }
    std::pair<ImVec2, ImVec2> GetConnector() const;
    std::unique_ptr<Layer> Clone() const override;
    int GetNumber() const { return m_number; }
    void SetNumber(int number) { m_number = number; }
    ImVec2 GetCenter() const { return m_center; }
    void SetCenter(ImVec2 center) { m_center = center; }
    float GetDiameter() const { return m_diameter; }
    void SetDiameter(float diameter) { m_diameter = diameter; }
    void StartDrag(const ImVec2& mousePos) override;
    void UpdateDrag(const ImVec2& delta) override;
    void EndDrag() override { m_dragging = false; }
    bool IsDragging() const override { return m_dragging; }
    bool IsPointNearBorder(const ImVec2& pos, float threshold) const override;
    void RenderSelectionUI(ImDrawList* drawList, float uiScale = 1.0f) const override;
private:
    void RenderLabel(ImDrawList* drawList, const ImVec2& canvasOffset);
    bool HasLabelText() const { return m_label && !m_label->GetText().empty(); }
    void RenderLabelFrame(ImDrawList* drawList, const ImVec2& canvasOffset, bool drawFrame = true) const;
    ImVec2 m_center;
    int m_number;
    float m_diameter;
    bool m_dragging = false;
    ImVec2 m_centerBeforeDrag;
    std::unique_ptr<TextLayer> m_label;
    // 保存创建时的边框间距比例，历史恢复不受当前窗口 DPI 和平台坐标差异影响。
    float m_labelFrameScale = 1;
    bool m_labelEditing = false;
    bool m_dragLabel = false;
    ImVec2 m_labelBeforeDrag;
};

// 图层管理器
class LayerManager {
public:
    LayerManager() = default;

    // 添加图层
    void AddLayer(std::unique_ptr<Layer> layer);

    // 撤回（删除最后一个图层）
    bool Undo();

    // 按指针移除图层（用于重新编辑文字时移除原 TextLayer）
    void RemoveLayer(Layer* layer);

    // 获取所有图层
    const std::vector<std::unique_ptr<Layer>>& GetLayers() const { return m_layers; }

    // 渲染所有图层到 ImDrawList
    void Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale = 1.0f);

    // 从原始图像数据开始，渲染所有图层到目标缓冲区
    // scaleFactor: 点坐标到像素坐标的缩放因子（用于 Retina 屏幕）
    void RenderFromOriginal(unsigned char* originalData, int originalRowPitch,
                           unsigned char* targetData, int imgWidth, int imgHeight,
                           int rowPitch, const ImVec2& canvasOffset,
                           float scaleFactor = 1.0f);

    // 清空所有图层
    void Clear();

    // 是否为空
    bool IsEmpty() const { return m_layers.empty(); }

private:
    void RenumberNumberLayers();
    std::vector<std::unique_ptr<Layer>> m_layers;
    uint64_t m_nextId = 1;
};

} // namespace shotlet
