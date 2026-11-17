#include "GifWriter.h"
#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace shotlet {
namespace {
constexpr int ColorBits = 5;
constexpr int ChannelBits = 8;
constexpr int ChannelShift = ChannelBits - ColorBits;
constexpr int HistogramSize = 1 << (ColorBits * 3);
constexpr int PaletteColors = 256;
constexpr int BgraChannels = 4;
constexpr int PaletteChannels = 3;
constexpr int GifBlockBytes = 255;
constexpr int InitialCodeBits = 9;
constexpr int MaximumCodeBits = 12;
constexpr int ClearCode = 256, EndCode = 257, FirstDictionaryCode = 258;
constexpr int MaximumDictionaryCodes = 1 << MaximumCodeBits;
constexpr int MaximumFrameDelayCentiseconds = 65535;
constexpr int MillisecondsPerCentisecond = 10;
struct ColorBin { uint32_t count = 0; uint64_t red = 0, green = 0, blue = 0; };
struct ColorBox { size_t begin = 0, end = 0; uint64_t weight = 0; int channel = 0, range = 0; };
int Channel(int bin, int channel) { return (bin >> ((2 - channel) * ColorBits)) & ((1 << ColorBits) - 1); }
ColorBox MeasureBox(const std::vector<int>& colors, const std::vector<ColorBin>& bins, size_t begin, size_t end) {
    ColorBox box{begin, end};
    std::array<int, 3> low{PaletteColors, PaletteColors, PaletteColors}, high{};
    for (size_t i = begin; i < end; ++i) {
        box.weight += bins[colors[i]].count;
        for (int channel = 0; channel < PaletteChannels; ++channel) {
            const auto value = Channel(colors[i], channel);
            low[channel] = std::min(low[channel], value); high[channel] = std::max(high[channel], value);
        }
    }
    for (int channel = 0; channel < PaletteChannels; ++channel)
        if (high[channel] - low[channel] > box.range) { box.range = high[channel] - low[channel]; box.channel = channel; }
    return box;
}
}

void GifWriter::Byte(uint8_t value) { m_output.put(static_cast<char>(value)); }
void GifWriter::Word(uint16_t value) { Byte(uint8_t(value)); Byte(uint8_t(value >> ChannelBits)); }

bool GifWriter::Open(const std::filesystem::path& path, int width, int height, std::string& error) {
    constexpr int MaximumGifDimension = 65535;
    error.clear();
    if (width <= 0 || height <= 0 || width > MaximumGifDimension || height > MaximumGifDimension) {
        error = "GIF 画面尺寸不受支持"; return false;
    }
    m_width = width; m_height = height; m_pending = false;
    m_output.open(path, std::ios::binary | std::ios::trunc);
    m_output.write("GIF89a", 6); Word(uint16_t(width)); Word(uint16_t(height));
    constexpr uint8_t ColorResolutionWithoutGlobalPalette = 0x70;
    Byte(ColorResolutionWithoutGlobalPalette); Byte(0); Byte(0);
    // NETSCAPE 扩展的循环次数为零，表示持续循环播放。
    const uint8_t loop[] = {0x21, 0xff, 11, 'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0};
    m_output.write(reinterpret_cast<const char*>(loop), sizeof(loop));
    if (!m_output) { error = "无法创建 GIF 文件"; return false; }
    return true;
}

void GifWriter::Quantize(const uint8_t* pixels, int pitchBytes, bool bottomUp) {
    std::vector<ColorBin> bins(HistogramSize);
    const auto binFor = [](const uint8_t* pixel) {
        return (int(pixel[2] >> ChannelShift) << (ColorBits * 2)) |
            (int(pixel[1] >> ChannelShift) << ColorBits) | int(pixel[0] >> ChannelShift);
    };
    for (int y = 0; y < m_height; ++y) {
        const auto* row = pixels + size_t(bottomUp ? m_height - y - 1 : y) * pitchBytes;
        for (int x = 0; x < m_width; ++x) {
            const auto* pixel = row + size_t(x) * BgraChannels;
            auto& bin = bins[binFor(pixel)];
            ++bin.count; bin.red += pixel[2]; bin.green += pixel[1]; bin.blue += pixel[0];
        }
    }
    std::vector<int> colors;
    for (int i = 0; i < HistogramSize; ++i) if (bins[i].count) colors.push_back(i);
    std::vector<ColorBox> boxes{MeasureBox(colors, bins, 0, colors.size())};
    while (boxes.size() < PaletteColors) {
        size_t best = boxes.size(); uint64_t score = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const auto value = boxes[i].weight * boxes[i].range;
            if (boxes[i].end - boxes[i].begin > 1 && value > score) { best = i; score = value; }
        }
        if (best == boxes.size()) break;
        const auto box = boxes[best];
        std::sort(colors.begin() + box.begin, colors.begin() + box.end,
            [&](int a, int b) { return Channel(a, box.channel) < Channel(b, box.channel); });
        size_t middle = box.begin; uint64_t weight = 0;
        while (middle + 1 < box.end && weight < box.weight / 2) weight += bins[colors[middle++]].count;
        middle = std::clamp(middle, box.begin + 1, box.end - 1);
        boxes[best] = MeasureBox(colors, bins, box.begin, middle);
        boxes.push_back(MeasureBox(colors, bins, middle, box.end));
    }
    std::array<uint8_t, HistogramSize> lookup{};
    m_palette.fill(0);
    for (size_t i = 0; i < boxes.size(); ++i) {
        uint64_t red = 0, green = 0, blue = 0;
        const auto box = boxes[i];
        for (size_t j = box.begin; j < box.end; ++j) {
            const auto& bin = bins[colors[j]];
            red += bin.red; green += bin.green; blue += bin.blue; lookup[colors[j]] = uint8_t(i);
        }
        m_palette[i * PaletteChannels] = uint8_t(red / box.weight);
        m_palette[i * PaletteChannels + 1] = uint8_t(green / box.weight);
        m_palette[i * PaletteChannels + 2] = uint8_t(blue / box.weight);
    }
    m_indices.resize(size_t(m_width) * m_height);
    for (int y = 0; y < m_height; ++y) {
        const auto* row = pixels + size_t(bottomUp ? m_height - y - 1 : y) * pitchBytes;
        for (int x = 0; x < m_width; ++x) m_indices[size_t(y) * m_width + x] = lookup[binFor(row + size_t(x) * BgraChannels)];
    }
}

void GifWriter::WriteImage() {
    Byte(0x2c); Word(0); Word(0); Word(uint16_t(m_width)); Word(uint16_t(m_height));
    constexpr uint8_t LocalPaletteWith256Colors = 0x87;
    Byte(LocalPaletteWith256Colors);
    m_output.write(reinterpret_cast<const char*>(m_palette.data()), m_palette.size());
    Byte(ChannelBits);
    std::unordered_map<uint32_t, uint16_t> dictionary;
    dictionary.reserve(MaximumDictionaryCodes);
    std::vector<uint8_t> encoded;
    int nextCode = FirstDictionaryCode, codeBits = InitialCodeBits, bufferedBits = 0;
    uint32_t accumulator = 0;
    const auto emit = [&](int code) {
        accumulator |= uint32_t(code) << bufferedBits; bufferedBits += codeBits;
        while (bufferedBits >= ChannelBits) {
            encoded.push_back(uint8_t(accumulator)); accumulator >>= ChannelBits; bufferedBits -= ChannelBits;
        }
        // 解码器在读到下一个代码后才补词典，位宽增长须比编码端建词典晚一次输出。
        if (nextCode == (1 << codeBits) && codeBits < MaximumCodeBits) ++codeBits;
    };
    emit(ClearCode);
    int prefix = m_indices.front();
    for (size_t i = 1; i < m_indices.size(); ++i) {
        const auto symbol = m_indices[i];
        const uint32_t key = (uint32_t(prefix) << ChannelBits) | symbol;
        if (const auto found = dictionary.find(key); found != dictionary.end()) { prefix = found->second; continue; }
        emit(prefix);
        if (nextCode < MaximumDictionaryCodes) dictionary.emplace(key, uint16_t(nextCode++));
        else { emit(ClearCode); dictionary.clear(); nextCode = FirstDictionaryCode; codeBits = InitialCodeBits; }
        prefix = symbol;
    }
    emit(prefix); emit(EndCode);
    if (bufferedBits) encoded.push_back(uint8_t(accumulator));
    for (size_t offset = 0; offset < encoded.size(); offset += GifBlockBytes) {
        const auto count = std::min<size_t>(GifBlockBytes, encoded.size() - offset);
        Byte(uint8_t(count)); m_output.write(reinterpret_cast<const char*>(encoded.data() + offset), count);
    }
    Byte(0);
}

void GifWriter::WriteFrame(int64_t delayMs) {
    auto delay = std::max<int64_t>(1, (delayMs + MillisecondsPerCentisecond / 2) / MillisecondsPerCentisecond);
    while (delay > 0) {
        const auto part = std::min<int64_t>(delay, MaximumFrameDelayCentiseconds);
        constexpr uint8_t KeepPreviousImageDisposal = 0x04;
        Byte(0x21); Byte(0xf9); Byte(4); Byte(KeepPreviousImageDisposal); Word(uint16_t(part)); Byte(0); Byte(0);
        WriteImage(); delay -= part;
    }
}

bool GifWriter::AddBgraFrame(const uint8_t* pixels, int pitchBytes, bool bottomUp, int64_t timestampMs, std::string& error) {
    if (!pixels || pitchBytes < m_width * BgraChannels || !m_output.is_open()) { error = "GIF 帧数据无效"; return false; }
    if (m_pending && timestampMs <= m_timestampMs) return true;
    if (m_pending) WriteFrame(timestampMs - m_timestampMs);
    Quantize(pixels, pitchBytes, bottomUp);
    m_timestampMs = timestampMs; m_pending = true;
    if (!m_output) { error = "GIF 写入失败"; return false; }
    return true;
}

bool GifWriter::Finish(int64_t durationMs, std::string& error) {
    if (!m_pending) { error = "没有可保存的 GIF 画面"; return false; }
    WriteFrame(durationMs - m_timestampMs); Byte(0x3b);
    m_output.close(); m_pending = false;
    if (!m_output) { error = "GIF 文件保存失败"; return false; }
    return true;
}
}
