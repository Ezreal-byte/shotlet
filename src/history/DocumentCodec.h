#pragma once
#include "HistoryTypes.h"
#include <string_view>

namespace shotlet {
// JSON 只包含结构与图层参数，像素通过独立 PNG 资产读写。
std::string EncodeDocument(const CaptureDocument& document);
CaptureDocument DecodeDocument(std::string_view json);
std::string EncodeHistoryMetadata(const HistoryMetadata& metadata);
HistoryMetadata DecodeHistoryMetadata(std::string_view json);
std::string EncodeHistoryConfig(const HistoryConfig& config);
HistoryConfig DecodeHistoryConfig(std::string_view json);
bool WriteHistoryPng(const std::filesystem::path& file, const unsigned char* rgba, int width, int height, std::string& error);
HistoryImage ReadHistoryPng(const std::filesystem::path& file, std::string& error, int expectedWidth = 0,
    int expectedHeight = 0, size_t maxBytes = CaptureDocument::MaxPixelBytes);
HistoryImage MakeHistoryThumbnail(const HistoryImage& image, int maxWidth = 192, int maxHeight = 128);
} // namespace shotlet
