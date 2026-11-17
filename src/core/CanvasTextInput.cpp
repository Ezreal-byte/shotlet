// Canvas 文本输入与编辑逻辑：
// 负责文字输入框、文本选区、光标移动、键盘输入处理、
// 以及重新编辑 TextLayer 时的恢复与提交。
#include "Canvas.h"
#include "platform/FontProvider.h"
#include "utils/Logger.h"
#include "utils/DashedLine.h"
#include "utils/Colors.h"
#include "utils/IStyle.h"
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <cfloat>

namespace shotlet {
    NumberLayer* Canvas::GetEditingNumberLayer() const {
        if (!m_reedit.numberLabel) return nullptr;
        for (const auto& layer : m_layerManager.GetLayers())
            if (layer->GetId() == m_reedit.layerId) return dynamic_cast<NumberLayer*>(layer.get());
        return nullptr;
    }

    void Canvas::BeginNumberLabelInput(NumberLayer& marker) {
        auto* label = marker.GetLabel();
        if (!label) return;
        if (m_textInput.active) CommitTextInput();
        ClearAllSelectedLayers();
        m_reedit = {};
        m_reedit.active = true;
        m_reedit.numberLabel = true;
        m_reedit.layerId = marker.GetId();
        m_reedit.text = label->GetText();
        m_reedit.color = marker.GetColor();
        m_reedit.fontSize = label->GetFontSize();
        m_textInput = {};
        m_textInput.active = true;
        m_textInput.position = label->GetPosition();
        m_textInput.text = label->GetText();
        m_textInput.cursorPos = static_cast<int>(m_textInput.text.size());
        m_textInput.focusRequested = true;
        m_textInput.fontSize = label->GetFontSize();
        m_textInput.wrapWidth = label->GetWrapWidth();
        marker.SetLabelEditing(true);
        m_state = SelectionState::Selected;
    }

    void Canvas::UpdateNumberLabelInput() {
        if (auto* marker = GetEditingNumberLayer()) {
            marker->GetLabel()->SetText(m_textInput.text);
            marker->GetLabel()->SetFontSize(m_textInput.fontSize);
        }
    }

    ImRect Canvas::GetTextInputFrameBounds() const {
        if (const auto* marker = GetEditingNumberLayer()) return marker->GetLabelFrameBounds();
        const auto selection = GetSelectionRect();
        const auto position = m_textInput.position;
        auto* font = ImGui::GetFont();
        auto size = font->CalcTextSizeA(m_textInput.fontSize, FLT_MAX, m_textInput.wrapWidth, m_textInput.text.c_str());
        size.y += CountEmptyLines(m_textInput.text) *
            font->CalcTextSizeA(m_textInput.fontSize, FLT_MAX, 0, " ").y;
        const float pad = TextLayer::FramePadding * GetTextFrameScale();
        const float right = ImMin(position.x + size.x + pad, selection.Max.x);
        const float left = ImMax(right - (size.x + 2 * pad), selection.Min.x);
        return ImRect(ImVec2(left - pad, position.y - pad), ImVec2(right + pad, position.y + size.y + pad));
    }

    bool Canvas::HandleTextInputClick(const ImVec2& pos) {
        if (!m_textInput.active) return false;
        const auto* marker = GetEditingNumberLayer();
        const bool hitCircle = marker && marker->ContainsCircle(pos);
        if (!hitCircle && GetTextInputFrameBounds().Contains(pos)) {
            const int charIdx = ImGui::GetCurrentContext() ? GetCharIndexFromPixelPos(pos) : 0;
            m_textInput.cursorPos = charIdx;
            m_textInput.selectionStart = charIdx;
            m_textInput.selectionEnd = charIdx;
            m_textInput.selecting = true;
            return true;
        }
        CommitTextInput();
        // 失焦点击只提交当前文字，避免同一次点击又创建数字点或拖动已有标注。
        return true;
    }

    void Canvas::CommitTextInput() {
        if (!m_textInput.active) return;

        if (auto* marker = GetEditingNumberLayer()) {
            UpdateNumberLabelInput();
            marker->SetLabelEditing(false);
            RecalculateDrawnContentBounds();
        } else if (!m_reedit.numberLabel && !m_textInput.text.empty()) {
            // 重新编辑时使用原 TextLayer 的颜色和字号，新建时使用输入时的字号
            ImU32 color = m_reedit.active ? m_reedit.color : m_toolColor;
            float fontSize = m_reedit.active ? m_reedit.fontSize : m_textInput.fontSize;
            // wrapWidth 在进入输入时已固定（新建=选区计算，重新编辑=原 TextLayer 值）
            float wrapWidth = m_textInput.wrapWidth;
            auto textLayer = std::make_unique<TextLayer>(
                m_textInput.position, m_textInput.text, color, fontSize, wrapWidth);
            if (m_reedit.active) textLayer->m_id = m_reedit.layerId;
#if !defined(SHOTLET_PLATFORM_WINDOWS)
            UpdateDrawnContentBounds(textLayer.get());
#endif
            m_layerManager.AddLayer(std::move(textLayer));
        }

        m_textInput = {};
        m_reedit = {};
        m_lastInputStr.clear();
        m_state = SelectionState::Selected;
    }

    // ESC 取消文字输入，如果是重新编辑模式则恢复原 TextLayer

    void Canvas::CancelTextInput() {
        if (!m_textInput.active) return;

        if (auto* marker = GetEditingNumberLayer()) {
            marker->GetLabel()->SetText(m_reedit.text);
            marker->GetLabel()->SetFontSize(m_reedit.fontSize);
            marker->SetLabelEditing(false);
            RecalculateDrawnContentBounds();
        } else if (m_reedit.active && !m_reedit.numberLabel && !m_reedit.text.empty()) {
            auto textLayer = std::make_unique<TextLayer>(
                m_reedit.position, m_reedit.text, m_reedit.color, m_reedit.fontSize, m_reedit.wrapWidth);
            textLayer->m_id = m_reedit.layerId;
#if !defined(SHOTLET_PLATFORM_WINDOWS)
            UpdateDrawnContentBounds(textLayer.get());
#endif
            m_layerManager.AddLayer(std::move(textLayer));
        }

        m_textInput = {};
        m_reedit = {};
        m_lastInputStr.clear();
        m_state = SelectionState::Selected;
    }

    void Canvas::OnMouseDoubleClick(const ImVec2& pos) {
        for (auto it = m_layerManager.GetLayers().rbegin(); it != m_layerManager.GetLayers().rend(); ++it) {
            auto* marker = dynamic_cast<NumberLayer*>(it->get());
            if (!marker || !marker->GetLabel()) continue;
            // 双击圆点或可见的说明框均可编辑说明，空说明框隐藏时只能通过圆点进入。
            if (marker->ContainsCircle(pos) ||
                (marker->IsLabelVisible() && marker->GetLabelFrameBounds().Contains(pos))) {
                BeginNumberLabelInput(*marker);
                return;
            }
        }
        if (m_currentTool == ToolType::Number) {
            OnMouseDown(pos);
            return;
        }
        // 如果已经在文字输入中，不处理双击
        if (m_textInput.active) return;

        // 智能区域：双击确认选区
        // 允许已有小选区（来自第一次点击的拖拽）的情况，智能区域优先级更高
        if (!HasSelection() && m_smartRegion && m_smartRegion->bounds.Contains(pos)) {
            m_selectionStart = m_smartRegion->bounds.Min;
            m_selectionEnd = m_smartRegion->bounds.Max;
            m_captureWindowLabel = m_smartRegion->label;
            m_captureWindowBounds = m_smartRegion->bounds;
            m_state = SelectionState::Selected;
            Logger::Debug("[OnMouseDoubleClick] Smart region selected: {:.0f}x{:.0f}",
                          m_smartRegion->bounds.GetWidth(), m_smartRegion->bounds.GetHeight());
            return;
        }

        // 检查是否双击在已提交的 TextLayer 上
        // 注意：不能在遍历中移除图层（会迭代器失效），找到后先记录再移除
        auto& layers = m_layerManager.GetLayers();
        TextLayer* foundTextLayer = nullptr;
        for (auto& layer : layers) {
            if (auto* textLayer = dynamic_cast<TextLayer*>(layer.get())) {
                ImRect bounds = textLayer->GetFrameBounds(GetTextFrameScale());
                if (bounds.Contains(pos)) {
                    foundTextLayer = textLayer;
                    break;
                }
            }
        }
        if (foundTextLayer) {
            // 进入重新编辑模式：记录原始信息，移除原 TextLayer
            m_reedit.active = true;
            m_reedit.layerId = foundTextLayer->GetId();
            m_reedit.position = foundTextLayer->GetPosition();
            m_reedit.text = foundTextLayer->GetText();
            m_reedit.color = foundTextLayer->GetColor();
            m_reedit.fontSize = foundTextLayer->GetFontSize();
            m_reedit.wrapWidth = foundTextLayer->GetWrapWidth();

            m_textInput.active = true;
            m_textInput.position = foundTextLayer->GetPosition();
            m_textInput.text = foundTextLayer->GetText();
            m_textInput.cursorPos = static_cast<int>(m_textInput.text.size());
            m_textInput.focusRequested = true;
            m_textInput.fontSize = foundTextLayer->GetFontSize();
            m_textInput.wrapWidth = foundTextLayer->GetWrapWidth();
            m_state = SelectionState::Drawing;

            // 从图层管理器中移除原 TextLayer（遍历已结束，不会迭代器失效）
            m_selectedLayer = nullptr;
            m_layerDragging = false;
            m_layerManager.RemoveLayer(foundTextLayer);
            RefreshTexture();
            RecalculateDrawnContentBounds();
            return;
        }
    }

    void Canvas::OnKeyDown(const int keyCode, const int modifiers, const bool repeat) {
        if (keyCode == SDLK_ESCAPE && m_textInput.active && m_reedit.numberLabel) {
            CancelTextInput();
            return;
        }
        if (keyCode == SDLK_ESCAPE && dynamic_cast<NumberLayer*>(m_currentLayer.get())) {
            m_currentLayer.reset();
            m_mouseDown = false;
            m_state = SelectionState::Selected;
            return;
        }
        // 非文本输入状态下，Delete/Backspace 键删除选中图层
        if (!m_textInput.active && m_selectedLayer &&
            (keyCode == SDLK_DELETE || keyCode == SDLK_BACKSPACE)) {
            Layer* layer = m_selectedLayer;
            m_selectedLayer = nullptr;
            m_layerDragging = false;
            m_layerManager.RemoveLayer(layer);
            RefreshTexture();
            RecalculateDrawnContentBounds();
            return;
        }

        if (!m_textInput.active) return;

        // 长按重复：复用上次输入的字符串
        // SDL3 长按时只 repeat SDL_EVENT_KEY_DOWN，不 repeat SDL_EVENT_TEXT_INPUT
        // 首次按键时 SDL_EVENT_TEXT_INPUT 携带正确字符（含 Shift 变换），
        // 在 ProcessTextInput 中处理并记录到 m_lastInputStr，repeat 时直接复用
        // Backspace/Delete/方向键等无 lastInputStr，fall through 到功能键处理
        if (repeat && !(modifiers & (SDL_KMOD_GUI | SDL_KMOD_CTRL | SDL_KMOD_ALT)) && !m_lastInputStr.empty()) {
            if (HasTextSelection()) DeleteSelectedText();
            m_textInput.text.insert(m_textInput.cursorPos, m_lastInputStr);
            m_textInput.cursorPos += static_cast<int>(m_lastInputStr.size());
            return;
        }

        // 功能键按下时清空 lastInputStr，防止后续 repeat 错误复用之前的字符输入
        m_lastInputStr.clear();

        // SDL3 key codes
        // Backspace 键：删除光标前的字符
        if (keyCode == SDLK_BACKSPACE) {
            if (HasTextSelection()) {
                DeleteSelectedText();
            } else if (m_textInput.cursorPos > 0) {
                int prevCharStart = m_textInput.cursorPos - 1;
                while (prevCharStart > 0 && (m_textInput.text[prevCharStart] & 0xC0) == 0x80) {
                    prevCharStart--;
                }
                m_textInput.text.erase(prevCharStart, m_textInput.cursorPos - prevCharStart);
                m_textInput.cursorPos = prevCharStart;
            }
        }
        // Delete 键：删除光标后的字符
        else if (keyCode == SDLK_DELETE) {
            if (HasTextSelection()) {
                DeleteSelectedText();
            } else if (m_textInput.cursorPos < static_cast<int>(m_textInput.text.size())) {
                // 找到光标后一个 UTF-8 字符的结束位置
                int nextCharEnd = m_textInput.cursorPos + 1;
                while (nextCharEnd < static_cast<int>(m_textInput.text.size()) &&
                       (m_textInput.text[nextCharEnd] & 0xC0) == 0x80) {
                    nextCharEnd++;
                }
                m_textInput.text.erase(m_textInput.cursorPos, nextCharEnd - m_textInput.cursorPos);
            }
        }
        // 左箭头：光标向左移动一个 UTF-8 字符
        else if (keyCode == SDLK_LEFT) {
            if (m_textInput.cursorPos > 0) {
                int prevCharStart = m_textInput.cursorPos - 1;
                while (prevCharStart > 0 && (m_textInput.text[prevCharStart] & 0xC0) == 0x80) {
                    prevCharStart--;
                }
                m_textInput.cursorPos = prevCharStart;
            }
        }
        // 右箭头：光标向右移动一个 UTF-8 字符
        else if (keyCode == SDLK_RIGHT) {
            if (m_textInput.cursorPos < static_cast<int>(m_textInput.text.size())) {
                int nextCharEnd = m_textInput.cursorPos + 1;
                while (nextCharEnd < static_cast<int>(m_textInput.text.size()) &&
                       (m_textInput.text[nextCharEnd] & 0xC0) == 0x80) {
                    nextCharEnd++;
                }
                m_textInput.cursorPos = nextCharEnd;
            }
        }
        // 上箭头：光标移动到上一行同列位置
        else if (keyCode == SDLK_UP) {
            MoveCursorVertically(-1);
        }
        // 下箭头：光标移动到下一行同列位置
        else if (keyCode == SDLK_DOWN) {
            MoveCursorVertically(1);
        }
        // Home：光标移动到开头
        else if (keyCode == SDLK_HOME) {
            m_textInput.cursorPos = 0;
        }
        // End：光标移动到末尾
        else if (keyCode == SDLK_END) {
            m_textInput.cursorPos = static_cast<int>(m_textInput.text.size());
        }
        // Cmd/Ctrl + A：全选
        else if (keyCode == SDLK_A && (modifiers & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
            m_textInput.selectionStart = 0;
            m_textInput.selectionEnd = static_cast<int>(m_textInput.text.size());
        }
        // Cmd/Ctrl + C：复制选中文字
        else if (keyCode == SDLK_C && (modifiers & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
            if (HasTextSelection()) {
                SDL_SetClipboardText(GetSelectedText().c_str());
            }
        }
        // Cmd/Ctrl + X：剪切选中文字
        else if (keyCode == SDLK_X && (modifiers & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
            if (HasTextSelection()) {
                SDL_SetClipboardText(GetSelectedText().c_str());
                DeleteSelectedText();
                RefreshTexture();
            }
        }
        // Cmd/Ctrl + V：粘贴剪贴板文字
        else if (keyCode == SDLK_V && (modifiers & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
            if (SDL_HasClipboardText()) {
                char* clipboardText = SDL_GetClipboardText();
                InsertText(std::string(clipboardText));
                SDL_free(clipboardText);
            }
        }
        // Enter/Return：插入换行符
        else if (keyCode == SDLK_RETURN || keyCode == SDLK_KP_ENTER) {
            if (HasTextSelection()) DeleteSelectedText();
            m_textInput.text.insert(m_textInput.cursorPos, "\n");
            m_textInput.cursorPos += 1;
        }
    }

    void Canvas::MoveCursorVertically(const int direction) {
        if (!m_textInput.active) return;
        const std::string& text = m_textInput.text;
        const int cursorPos = m_textInput.cursorPos;

        // 将文本按 \n 分行，记录每行的字节偏移范围
        struct LineInfo { int start; int end; };  // end 不包含 \n 本身
        std::vector<LineInfo> lines;
        int pos = 0;
        while (pos <= static_cast<int>(text.size())) {
            int lineEnd = pos;
            while (lineEnd < static_cast<int>(text.size()) && text[lineEnd] != '\n') lineEnd++;
            lines.push_back({pos, lineEnd});
            pos = lineEnd + 1;  // 跳过 \n
        }

        // 找到当前光标所在行
        int currentLine = 0;
        int column = 0;  // 光标在本行的字节偏移
        for (int i = 0; i < static_cast<int>(lines.size()); i++) {
            if (cursorPos >= lines[i].start && cursorPos <= lines[i].end) {
                currentLine = i;
                column = cursorPos - lines[i].start;
                break;
            }
        }

        const int targetLine = currentLine + direction;
        if (targetLine < 0 || targetLine >= static_cast<int>(lines.size())) return;

        // 尝试保持同一列字节偏移，但不超过目标行长度
        const int targetColumn = ImMin(column, lines[targetLine].end - lines[targetLine].start);
        m_textInput.cursorPos = lines[targetLine].start + targetColumn;
    }

    bool Canvas::HasTextSelection() const {
        return m_textInput.selectionStart >= 0 && m_textInput.selectionEnd >= 0 &&
               m_textInput.selectionStart != m_textInput.selectionEnd;
    }

    std::string Canvas::GetSelectedText() const {
        if (!HasTextSelection()) return "";
        const int start = ImMin(m_textInput.selectionStart, m_textInput.selectionEnd);
        const int end = ImMax(m_textInput.selectionStart, m_textInput.selectionEnd);
        return m_textInput.text.substr(start, end - start);
    }

    void Canvas::DeleteSelectedText() {
        if (!HasTextSelection()) return;
        const int start = ImMin(m_textInput.selectionStart, m_textInput.selectionEnd);
        const int end = ImMax(m_textInput.selectionStart, m_textInput.selectionEnd);
        m_textInput.text.erase(start, end - start);
        m_textInput.cursorPos = start;
        m_textInput.selectionStart = -1;
        m_textInput.selectionEnd = -1;
    }

    void Canvas::InsertText(const std::string& text) {
        // 先删除选中文字
        if (HasTextSelection()) DeleteSelectedText();
        // 粘贴时过滤 \r，只保留 \n（跨平台剪贴板文本可能有 \r\n）
        std::string filtered;
        for (const char c : text) {
            if (c == '\r') continue;
            filtered += c;
        }
        m_textInput.text.insert(m_textInput.cursorPos, filtered);
        m_textInput.cursorPos += static_cast<int>(filtered.size());
    }

    int Canvas::GetCharIndexFromPixelPos(const ImVec2 pixelPos) const {
        if (!m_textInput.active) return 0;
        if (m_textInput.text.empty()) return 0;

        ImFont* font = ImGui::GetFont();
        if (!font) return 0;

        const ImVec2 pos = m_textInput.position;
        const float fontSize = m_textInput.fontSize;
        const float wrapWidth = m_textInput.wrapWidth;

        const char* textStart = m_textInput.text.c_str();
        const char* textEnd = textStart + m_textInput.text.size();
        const float singleLineHeight = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, " ", nullptr, nullptr).y;

        // 逐行遍历，找到 pixelPos.y 落在哪一行
        float lineY = pos.y;
        const char* lineStart = textStart;

        while (lineStart < textEnd) {
            // 查找本行结束位置
            const char* lineEnd = lineStart;
            while (lineEnd < textEnd && *lineEnd != '\n') lineEnd++;
            const char* wrapEnd = font->CalcWordWrapPosition(fontSize, lineStart, lineEnd, wrapWidth);
            const char* effectiveLineEnd = ImMin(lineEnd, wrapEnd);
            // 防止 effectiveLineEnd == lineStart 导致无限循环
            if (effectiveLineEnd == lineStart && lineStart < textEnd) {
                const char* nextChar = lineStart + 1;
                while (nextChar < textEnd && (*nextChar & 0xC0) == 0x80) nextChar++;
                effectiveLineEnd = nextChar;
            }

            const ImVec2 lineSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, effectiveLineEnd, nullptr);
            // 空行至少占 singleLineHeight
            const float lineH = ImMax(lineSize.y, singleLineHeight);

            // pixelPos.y 在本行范围内？
            if (pixelPos.y >= lineY && pixelPos.y < lineY + lineH) {
                const float targetX = pixelPos.x - pos.x;
                if (effectiveLineEnd == lineStart) {
                    // 空行，返回本行开头偏移
                    return static_cast<int>(lineStart - textStart);
                }
                // 用二分查找找最接近 targetX 的字符偏移
                // 将本行文本分成 UTF-8 字符偏移列表
                std::vector<int> charOffsets;  // 字符在行内的字节偏移（从 lineStart 起）
                const char* p = lineStart;
                while (p < effectiveLineEnd) {
                    charOffsets.push_back(static_cast<int>(p - lineStart));
                    const char* next = p + 1;
                    while (next < effectiveLineEnd && (*next & 0xC0) == 0x80) next++;
                    p = next;
                }
                charOffsets.push_back(static_cast<int>(effectiveLineEnd - lineStart));  // 末尾

                int lo = 0, hi = static_cast<int>(charOffsets.size()) - 1;
                while (lo < hi) {
                    const int mid = (lo + hi) / 2;
                    const char* midPtr = lineStart + charOffsets[mid];
                    const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, midPtr, nullptr);
                    if (sz.x < targetX) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                // 检查 lo 和 lo-1 哪个更接近
                const char* loPtr = lineStart + charOffsets[lo];
                const float loX = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, loPtr, nullptr).x;
                if (lo > 0) {
                    const char* prevPtr = lineStart + charOffsets[lo - 1];
                    const float prevX = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, prevPtr, nullptr).x;
                    if (fabsf(prevX - targetX) <= fabsf(loX - targetX)) {
                        lo--;
                    }
                }
                return static_cast<int>(lineStart + charOffsets[lo] - textStart);
            }

            lineY += lineH;

            // 前进到下一行
            if (effectiveLineEnd < textEnd && *effectiveLineEnd == '\n') {
                lineStart = effectiveLineEnd + 1;
            } else {
                lineStart = effectiveLineEnd;
            }
        }

        // 超出文本范围，返回末尾
        return static_cast<int>(m_textInput.text.size());
    }

    void Canvas::ProcessTextInput() {
        if (!m_textInput.active) return;

        // 处理键盘输入：从 ImGui InputQueue 接收字符（仅可打印字符）
        // Backspace/Delete/方向键在 OnKeyDown 中处理
        ImGuiIO& io = ImGui::GetIO();
        // 有选中文字时，输入第一个字符前先删除选中
        if (io.InputQueueCharacters.Size > 0 && HasTextSelection()) {
            DeleteSelectedText();
        }
        for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
            if (unsigned int c = io.InputQueueCharacters[i]; c >= 0x20 && c != 0x7F) {  // 可打印字符
                // Unicode code point → UTF-8 编码
                char utf8Buf[5] = {};
                int utf8Len = 0;
                if (c < 0x80) {
                    utf8Buf[0] = static_cast<char>(c);
                    utf8Len = 1;
                } else if (c < 0x800) {
                    utf8Buf[0] = static_cast<char>(0xC0 | (c >> 6));
                    utf8Buf[1] = static_cast<char>(0x80 | (c & 0x3F));
                    utf8Len = 2;
                } else if (c < 0x10000) {
                    utf8Buf[0] = static_cast<char>(0xE0 | (c >> 12));
                    utf8Buf[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    utf8Buf[2] = static_cast<char>(0x80 | (c & 0x3F));
                    utf8Len = 3;
                } else {
                    utf8Buf[0] = static_cast<char>(0xF0 | (c >> 18));
                    utf8Buf[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                    utf8Buf[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    utf8Buf[3] = static_cast<char>(0x80 | (c & 0x3F));
                    utf8Len = 4;
                }
                m_textInput.text.insert(m_textInput.cursorPos, utf8Buf, utf8Len);
                m_textInput.cursorPos += utf8Len;
                // 记录最后输入的字符，用于长按 repeat 复用
                m_lastInputStr = std::string(utf8Buf, utf8Len);
            }
        }
        io.InputQueueCharacters.resize(0);  // 清空已处理的字符队列

        UpdateNumberLabelInput();
    }

    void Canvas::RenderTextInput() {
        if (!m_textInput.active) return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (!drawList) return;

        ImRect sel = GetSelectionRect();
        ImVec2 pos = m_textInput.position;
        float fontSize = m_textInput.fontSize;
        ImFont* font = ImGui::GetFont();

        // 使用输入时固定的 wrapWidth
        float wrapWidth = m_textInput.wrapWidth;

        // 选区高亮和光标按文字工具相同的行高排版。
        float singleLineHeight = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, " ", nullptr, nullptr).y;

        const auto frame = GetTextInputFrameBounds();
        const ImVec2 borderMin = frame.Min, borderMax = frame.Max;
        const auto* numberMarker = GetEditingNumberLayer();

        // 裁剪区域 = 选区矩形，超出部分不可见
        drawList->PushClipRect(sel.Min, sel.Max, true);

        // 绘制透明背景矩形
        drawList->AddRectFilled(borderMin, borderMax, Colors::TextTransparentBg);

        if (numberMarker) {
            drawList->AddRect(borderMin, borderMax, numberMarker->GetColor(), 0, 0,
                numberMarker->GetLabel()->GetThickness() * numberMarker->GetLabelFrameScale());
        } else {
            DrawDashedRect(drawList, borderMin, borderMax, Colors::TextDashedBorder,
                IStyle::DashThickness, IStyle::DashLen, IStyle::GapLen, m_uiScale);
            // 数字说明只保留实线框；四角手柄属于普通文本工具的编辑样式。
            const ImVec2 corners[] = {
                borderMin,
                ImVec2(borderMin.x, borderMax.y),
                ImVec2(borderMax.x, borderMin.y),
                borderMax
            };
            for (const auto& corner : corners) {
                drawList->AddCircleFilled(corner, IStyle::HandleRadius * m_uiScale, Colors::HandleFill);
                drawList->AddCircle(corner, IStyle::HandleRadius * m_uiScale, Colors::HandleBorder);
            }
        }

        // 绘制选中高亮背景（在文字之前，文字覆盖在选中背景之上）
        if (HasTextSelection()) {
            int selStart = ImMin(m_textInput.selectionStart, m_textInput.selectionEnd);
            int selEnd = ImMax(m_textInput.selectionStart, m_textInput.selectionEnd);
            const char* textStart = m_textInput.text.c_str();
            const char* textEndPtr = textStart + m_textInput.text.size();
            const char* selStartPtr = textStart + selStart;
            const char* selEndPtr = textStart + selEnd;

            // 逐行遍历，绘制选中范围的每行高亮矩形
            float lineY = pos.y;
            const char* lineStart = textStart;
            while (lineStart < textEndPtr) {
                const char* lineEndPtr2 = lineStart;
                while (lineEndPtr2 < textEndPtr && *lineEndPtr2 != '\n') lineEndPtr2++;
                const char* wrapEnd2 = font->CalcWordWrapPosition(fontSize, lineStart, lineEndPtr2, wrapWidth);
                const char* effectiveLineEnd2 = ImMin(lineEndPtr2, wrapEnd2);
                // 防止 effectiveLineEnd2 == lineStart 导致无限循环
                if (effectiveLineEnd2 == lineStart && lineStart < textEndPtr) {
                    // 跳过至少一个 UTF-8 字符
                    const char* nextChar = lineStart + 1;
                    while (nextChar < textEndPtr && (*nextChar & 0xC0) == 0x80) nextChar++;
                    effectiveLineEnd2 = nextChar;
                }
                ImVec2 lineSize2 = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, effectiveLineEnd2, nullptr);
                float lineH2 = ImMax(lineSize2.y, singleLineHeight);

                // 选中范围与本行有交集？
                if (selStartPtr < effectiveLineEnd2 && selEndPtr > lineStart) {
                    const char* lineSelStart = ImMax(selStartPtr, lineStart);
                    const char* lineSelEnd = ImMin(selEndPtr, effectiveLineEnd2);
                    ImVec2 beforeSel = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, lineSelStart, nullptr);
                    ImVec2 selWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineSelStart, lineSelEnd, nullptr);
                    drawList->AddRectFilled(
                        ImVec2(pos.x + beforeSel.x, lineY),
                        ImVec2(pos.x + beforeSel.x + selWidth.x, lineY + lineH2),
                        Colors::TextSelectionBackground);  // 半透明蓝色选中背景
                }

                lineY += lineH2;
                if (effectiveLineEnd2 < textEndPtr && *effectiveLineEnd2 == '\n') {
                    lineStart = effectiveLineEnd2 + 1;
                } else {
                    lineStart = effectiveLineEnd2;
                }
            }
        }

        // 绘制文字内容
        const ImU32 textColor = numberMarker ? numberMarker->GetColor() :
            (m_reedit.active ? m_reedit.color : m_toolColor);
        drawList->AddText(font, fontSize, pos, textColor, m_textInput.text.c_str(), nullptr, wrapWidth);

        // 计算光标像素位置（每帧更新，用于 IME 候选词框定位和光标渲染）
        float cursorPixelX = pos.x;
        float cursorPixelY = pos.y;
        if (!m_textInput.text.empty()) {
            const char* textStart = m_textInput.text.c_str();
            const char* textEnd = textStart + m_textInput.text.size();
            const char* cursorPtr = textStart + m_textInput.cursorPos;

            // 将文本按 \n 分行，再按 wrapWidth 截断，逐行计算
            float lineY = pos.y;
            const char* lineStart = textStart;
            bool cursorFound = false;

            while (lineStart < textEnd) {
                // 查找本行结束位置：\n 或 wrap 换行
                const char* lineEnd = lineStart;
                while (lineEnd < textEnd && *lineEnd != '\n') lineEnd++;
                const char* wrapEnd = font->CalcWordWrapPosition(fontSize, lineStart, lineEnd, wrapWidth);
                const char* effectiveLineEnd = ImMin(lineEnd, wrapEnd);
                // 防止 effectiveLineEnd == lineStart 导致无限循环
                if (effectiveLineEnd == lineStart && lineStart < textEnd) {
                    const char* nextChar = lineStart + 1;
                    while (nextChar < textEnd && (*nextChar & 0xC0) == 0x80) nextChar++;
                    effectiveLineEnd = nextChar;
                }

                ImVec2 lineSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, effectiveLineEnd, nullptr);

                // 光标在本行内？
                if (!cursorFound && cursorPtr >= lineStart && cursorPtr <= effectiveLineEnd) {
                    ImVec2 beforeCursor = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, lineStart, cursorPtr, nullptr);
                    cursorPixelX = pos.x + beforeCursor.x;
                    cursorPixelY = lineY;
                    cursorFound = true;
                }

                lineY += lineSize.y;

                // 前进到下一行
                if (effectiveLineEnd < textEnd && *effectiveLineEnd == '\n') {
                    // \n 后光标在新行开头
                    if (!cursorFound && cursorPtr == effectiveLineEnd) {
                        cursorPixelX = pos.x;
                        cursorPixelY = lineY;
                        cursorFound = true;
                    }
                    lineStart = effectiveLineEnd + 1;
                } else {
                    lineStart = effectiveLineEnd;
                }
            }

            // 光标在文本末尾（while 循环结束时 lineStart == textEnd）
            if (!cursorFound) {
                const char* prevLineStart = textStart;
                float prevLineY = pos.y;
                // 重新遍历找最后一行位置
                const char* ls = textStart;
                float ly = pos.y;
                while (ls < textEnd) {
                    const char* le = ls;
                    while (le < textEnd && *le != '\n') le++;
                    const char* we = font->CalcWordWrapPosition(fontSize, ls, le, wrapWidth);
                    const char* ele = ImMin(le, we);
                    // 防止 ele == ls 导致无限循环
                    if (ele == ls && ls < textEnd) {
                        const char* nextChar = ls + 1;
                        while (nextChar < textEnd && (*nextChar & 0xC0) == 0x80) nextChar++;
                        ele = nextChar;
                    }
                    ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ls, ele, nullptr);
                    float szH = ImMax(sz.y, singleLineHeight);
                    prevLineStart = ls;
                    prevLineY = ly;
                    ly += szH;
                    if (ele < textEnd && *ele == '\n') {
                        ls = ele + 1;
                    } else {
                        ls = ele;
                    }
                }
                // 文本以 \n 结尾 → 光标在新空行开头
                if (m_textInput.text.back() == '\n') {
                    cursorPixelX = pos.x;
                    cursorPixelY = ly;
                } else {
                    ImVec2 lastSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, prevLineStart, textEnd, nullptr);
                    cursorPixelX = pos.x + lastSz.x;
                    cursorPixelY = prevLineY;
                }
                cursorFound = true;
            }
        }

        // 更新 IME 光标位置（供 SDL_SetTextInputArea 使用）
        m_imeCursorPos = ImVec2(cursorPixelX, cursorPixelY);

        // 绘制光标闪烁线
        float cursorTime = static_cast<float>(ImGui::GetTime());
        bool showCursor = (static_cast<int>(cursorTime * 2) % 2) == 0;

        if (showCursor) {
            drawList->AddLine(
                ImVec2(cursorPixelX, cursorPixelY),
                ImVec2(cursorPixelX, cursorPixelY + fontSize),
                textColor, 1.5f);
        }

        drawList->PopClipRect();
    }
} // namespace shotlet
