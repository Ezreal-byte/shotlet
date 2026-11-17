#pragma once
#include "core/CaptureDocument.h"
#include <memory>
#include <filesystem>

namespace shotlet {
struct HistoryConfig { bool enabled = true; std::string generation; };
struct HistoryDetails {
    static constexpr size_t MaxOcrTextBytes = 64u * 1024u;
    std::string title, captureType = "unknown", sourceApplication, sourceWindow;
    std::vector<std::string> tags;
    bool favorite = false, ocrIndexed = false;
    std::string ocrText, ocrError;
    int64_t deletedAtUnixMs = 0;
};
struct HistoryMetadata {
    static constexpr size_t MaxJsonBytes = 512u * 1024u;
    std::string id, action, sourceRecordId;
    int64_t createdAtUnixMs = 0;
    int width = 0, height = 0;
    uint64_t bytes = 0;
    int formatVersion = CaptureDocument::FormatVersion, renderVersion = CaptureDocument::RenderVersion;
    HistoryDetails details;
};
struct HistoryImage {
    std::shared_ptr<const unsigned char> pixels;
    int width = 0, height = 0;
    size_t ByteSize() const { return size_t(width) * height * 4; }
    explicit operator bool() const { return pixels && DocumentPixelBytes(width, height).has_value(); }
};
std::string NewHistoryId();
} // namespace shotlet
