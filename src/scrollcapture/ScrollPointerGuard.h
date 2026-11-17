#pragma once
#include "scrollcapture/ScrollTarget.h"
#include <algorithm>
namespace shotlet {
enum class ScrollPointerAction { Down, Drag, Up };
// 不吞掉独立面板事件；选区内开始的拖拽越界后仍吞掉配对事件，避免底层收到孤立的 up。
inline bool SuppressScrollPointer(const GlobalRect& selection, const GlobalRect& controls,
    int x, int y, unsigned button, ScrollPointerAction action, unsigned& swallowedButtons) {
    const unsigned bit = 1u << std::min(button, 31u);
    if (action == ScrollPointerAction::Up && (swallowedButtons & bit)) {
        swallowedButtons &= ~bit;
        return true;
    }
    if (action == ScrollPointerAction::Drag && (swallowedButtons & bit)) return true;
    if (Contains(controls, x, y)) return false;
    const bool inside = Contains(selection, x, y);
    if (inside && action == ScrollPointerAction::Down) swallowedButtons |= bit;
    return inside;
}
} // namespace shotlet
