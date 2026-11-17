#include "HistoryService.h"
#include "core/Canvas.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include "platform/OcrService.h"
#include "HistoryLibrary.h"

namespace shotlet {
HistoryBudget::Lease HistoryBudget::TryReserve(size_t bytes) {
    auto used = m_state->used.load();
    do {
        if (bytes == 0 || bytes > m_state->limit || used > m_state->limit - bytes) return {};
    } while (!m_state->used.compare_exchange_weak(used, used + bytes));
    return Lease(m_state, bytes);
}
HistoryService::HistoryService(std::filesystem::path root, size_t memoryLimit) : m_store(std::move(root)), m_budget(memoryLimit) {
    m_store.Recover(); m_worker = std::thread([this] { Run(); });
}
HistoryService::~HistoryService() { StopAccepting(); Drain(); }
void HistoryService::Notice(std::string error) { std::lock_guard lock(m_mutex); m_notices.push_back(std::move(error)); }
std::vector<std::string> HistoryService::TakeNotices() {
    std::lock_guard lock(m_mutex); auto notices = std::move(m_notices); m_notices.clear(); return notices;
}
std::unique_ptr<HistoryService::Candidate> HistoryService::Prepare(Canvas* canvas, HistoryImage result,
                                                                 const std::string& action, const std::string& sourceRecordId) {
    if (!m_accepting || !result) return {};
    try {
        std::string error;
        auto generation = m_store.RegisterCandidate(error); // 在任何快照分配前读取持久化开关和代次
        if (!generation) { if (!error.empty()) Notice(error); return {}; }
        auto estimate = canvas ? canvas->EstimateDocumentSnapshotBytes(error) : std::optional(result.ByteSize() + sizeof(CaptureDocument));
        if (!estimate) { Notice(error); return {}; }
        const size_t rgba = canvas ? *canvas->DocumentSnapshotPixelBytes(error) : result.ByteSize();
        // SDL 3.4.18 的 PNG 编码器为 miniz：单张顺序编码。工作区上界取 6 倍最大 RGBA，
        // 覆盖行滤波、压缩状态和压缩输出扩容；另留 16 MiB 固定空间及保守 JSON DOM 空间。
        // 候选与共享成图从登记到编码结束始终计费，不依赖队列长度。
        size_t pointsAndText = 0;
        if (canvas) for (const auto& layer : canvas->GetLayerManager().GetLayers()) {
            if (const auto* p = dynamic_cast<const BrushLayer*>(layer.get())) pointsAndText += p->GetPoints().size() * 512;
            else if (const auto* p = dynamic_cast<const MosaicLayer*>(layer.get())) pointsAndText += p->GetPoints().size() * 512;
            else if (const auto* p = dynamic_cast<const TextLayer*>(layer.get())) pointsAndText += p->GetText().size() * 8;
            pointsAndText += 4096;
        }
        const size_t required = *estimate + result.ByteSize() + 6 * std::max(rgba, result.ByteSize()) +
                                16u * 1024u * 1024u + pointsAndText;
        auto lease = m_budget.TryReserve(required);
        if (!lease) { Notice("本次截图历史无法保留：内存预算不足"); return {}; }
        auto candidate = std::make_unique<Candidate>(std::move(lease));
        candidate->generation = *generation; candidate->action = action; candidate->result = std::move(result);
        if (canvas) {
            auto document = canvas->ExportDocument(error);
            if (!document) { Notice(error); return {}; }
            candidate->document = std::move(*document);
        } else {
            auto& d = candidate->document;
            d.width = candidate->result.width; d.height = candidate->result.height;
            d.logicalSize = {float(d.width), float(d.height)}; d.source = "scroll";
            d.createdAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            d.originalRgba.assign(candidate->result.pixels.get(), candidate->result.pixels.get() + candidate->result.ByteSize());
        }
        candidate->document.sourceRecordId = sourceRecordId;
        candidate->document.source = !sourceRecordId.empty() ? "history" : canvas && !canvas->IsImageEditing() ? "capture" : "scroll";
        if (!sourceRecordId.empty()) {
            if (auto source = m_store.Get(sourceRecordId, error)) {
                candidate->details = source->details;
                candidate->details.title.clear(); candidate->details.favorite = false;
                candidate->details.deletedAtUnixMs = 0; candidate->details.ocrIndexed = false;
                candidate->details.ocrText.clear(); candidate->details.ocrError.clear();
            }
        } else if (canvas && !canvas->IsImageEditing()) {
            const auto selection = canvas->GetSelection(); const auto size = canvas->GetDisplaySize();
            const bool full = std::abs(selection.Min.x) < .01f && std::abs(selection.Min.y) < .01f &&
                std::abs(selection.Max.x - size.x) < .01f && std::abs(selection.Max.y - size.y) < .01f;
            candidate->details.captureType = full ? "fullscreen" : canvas->IsWindowCapture() ? "window" : "region";
            auto label = canvas->GetCaptureWindowLabel();
            auto separator = label.find(" - ");
            candidate->details.sourceApplication = separator == std::string::npos ? label : label.substr(0, separator);
            if (separator != std::string::npos) candidate->details.sourceWindow = label.substr(separator + 3);
        } else candidate->details.captureType = "scroll";
        return candidate;
    } catch (const std::exception& e) { Notice(e.what()); return {}; }
}
void HistoryService::Finish(std::unique_ptr<Candidate> candidate) {
    if (!candidate) return;
    try { if (m_store.GetConfig().generation != candidate->generation) return; }
    catch (const std::exception& e) { Notice(e.what()); return; }
    {
        std::lock_guard lock(m_mutex);
        if (m_stop) return;
        m_queue.push_back(std::move(candidate));
    }
    m_wake.notify_one();
}
void HistoryService::Run() {
    for (;;) {
        std::unique_ptr<Candidate> work;
        std::string indexId;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty() || !m_indexQueue.empty(); });
            if (!m_queue.empty()) { work = std::move(m_queue.front()); m_queue.pop_front(); }
            else if (!m_stop && !m_indexQueue.empty()) { indexId = std::move(m_indexQueue.front()); m_indexQueue.pop_front(); }
            else return;
        }
        if (!indexId.empty()) {
            const bool completed = Index(indexId);
            std::unique_lock lock(m_mutex);
            if (completed || m_stop) m_indexPending.erase(indexId);
            else {
                // 临时预算不足只重试当前 OCR 任务，不依赖界面扫描所有元数据。
                m_indexQueue.push_back(indexId);
                m_wake.wait_for(lock, std::chrono::seconds(1), [this] { return m_stop || !m_queue.empty(); });
            }
            continue;
        }
        std::string error;
        std::string id;
        const bool ok = m_store.Write(work->generation, work->document, work->result, work->action, error, work->details, &id);
        if (ok) RequestIndex(id);
        else if (error != "候选代次已失效") Notice("本次截图历史无法保留：" + error);
        // work 在本轮末尾析构，其预算包含此时仍在持有的所有缓冲。
    }
}
void HistoryService::RequestIndex(const std::string& id, bool force) {
    std::string error;
    auto metadata = m_store.Get(id, error);
    if (!metadata || metadata->details.deletedAtUnixMs || (!force && metadata->details.ocrIndexed)) return;
    std::lock_guard lock(m_mutex);
    if (m_stop) return;
    if (!m_indexPending.insert(id).second) {
        if (!force || std::find(m_indexQueue.begin(), m_indexQueue.end(), id) == m_indexQueue.end()) return;
        std::erase(m_indexQueue, id);
    }
    if (force) m_indexQueue.push_front(id); else m_indexQueue.push_back(id);
    m_wake.notify_one();
}
bool HistoryService::IsIndexing(const std::string& id) {
    std::lock_guard lock(m_mutex); return m_indexPending.contains(id);
}
bool HistoryService::Index(const std::string& id) {
    try {
        std::string error; auto metadata = m_store.Get(id, error);
        if (!metadata || metadata->details.deletedAtUnixMs) return true;
        const size_t required=size_t(metadata->width)*metadata->height*8+16u*1024u*1024u;
        auto budget = m_budget.TryReserve(required);
        if (!budget) {
            if(required<=m_budget.Limit()) return false;
            m_store.UpdateDetails(id,[](HistoryDetails& d){
                d.ocrIndexed=true;d.ocrError="图片过大，无法在当前内存预算内识别文字";
            },error);
            return true;
        }
        auto image = m_store.LoadResult(id, false, error);
        OcrResult result;
        if (image) result = OcrService::RecognizeDetailedAsync(image.pixels.get(), image.width, image.height).get();
        else result.error = error;
        if (result.text.size() > HistoryDetails::MaxOcrTextBytes) {
            size_t end = HistoryDetails::MaxOcrTextBytes;
            while (end && (static_cast<unsigned char>(result.text[end]) & 0xc0) == 0x80) --end;
            result.text.resize(end);
        }
        if (result.error.size() > 2048) result.error = "OCR failed";
        m_store.UpdateDetails(id, [&](HistoryDetails& d) {
            d.ocrIndexed = true; d.ocrText = result.text; d.ocrError = result.success ? "" : result.error;
        }, error);
    } catch (const std::exception& e) {
        try {
            std::string error;
            m_store.UpdateDetails(id,[&](HistoryDetails& d){
                d.ocrIndexed=true;d.ocrError=std::string(e.what()).substr(0,2048);
            },error);
        } catch (...) { /* 被删除的记录或资源不足时不重新创建资产。 */ }
    } catch (...) { /* 单张识别失败不终止后台队列。 */ }
    return true;
}
void HistoryService::Drain() {
    { std::lock_guard lock(m_mutex); m_stop = true; m_indexQueue.clear(); }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}
void HistoryService::ResumeAfterDrain() {
    if (m_worker.joinable()) return;
    { std::lock_guard lock(m_mutex); m_stop = false; m_indexPending.clear(); }
    m_accepting = true;
    m_worker = std::thread([this] { Run(); });
}
} // namespace shotlet
