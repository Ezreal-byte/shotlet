#include "Layer.h"
#include <algorithm>
#include <cstring>

namespace shotlet {

// ========== LayerManager ==========
void LayerManager::AddLayer(std::unique_ptr<Layer> layer) {
    if (layer->m_id == 0) layer->m_id = m_nextId;
    m_nextId = std::max(m_nextId, layer->m_id + 1);
    m_layers.push_back(std::move(layer));
    RenumberNumberLayers();
}

bool LayerManager::Undo() {
    if (m_layers.empty()) return false;
    m_layers.pop_back();
    RenumberNumberLayers();
    return true;
}

void LayerManager::RemoveLayer(Layer* layer) {
    auto it = std::find_if(m_layers.begin(), m_layers.end(),
        [layer](const std::unique_ptr<Layer>& l) { return l.get() == layer; });
    if (it != m_layers.end()) {
        m_layers.erase(it);
        RenumberNumberLayers();
    }
}

void LayerManager::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    for (const auto& layer : m_layers) {
        layer->Render(drawList, canvasOffset, uiScale);
    }
}

void LayerManager::RenderFromOriginal(unsigned char* originalData, int originalRowPitch,
                                      unsigned char* targetData, int imgWidth, int imgHeight,
                                      int rowPitch, const ImVec2& canvasOffset,
                                      float scaleFactor) {
    // 从原始数据复制到目标缓冲区
    int dataSize = rowPitch * imgHeight;
    memcpy(targetData, originalData, dataSize);

    // 让每个图层应用效果到目标缓冲区
    for (const auto& layer : m_layers) {
        layer->RenderToImage(targetData, imgWidth, imgHeight, rowPitch, canvasOffset, scaleFactor);
    }
}

void LayerManager::Clear() {
    m_layers.clear();
    m_nextId = 1;
}

void LayerManager::RenumberNumberLayers() {
    int number = 1;
    for (const auto& layer : m_layers) {
        if (auto* marker = dynamic_cast<NumberLayer*>(layer.get())) marker->SetNumber(number++);
    }
}


} // namespace shotlet
