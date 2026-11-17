#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace shotlet {
// 每次只保留一帧和调色板；按实际帧时间写入，静止画面不会积压在内存中。
class GifWriter {
public:
    bool Open(const std::filesystem::path& path, int width, int height, std::string& error);
    bool AddBgraFrame(const uint8_t* pixels, int pitchBytes, bool bottomUp, int64_t timestampMs, std::string& error);
    bool Finish(int64_t durationMs, std::string& error);
private:
    void Quantize(const uint8_t* pixels, int pitchBytes, bool bottomUp);
    void WriteFrame(int64_t delayMs);
    void WriteImage();
    void Byte(uint8_t value);
    void Word(uint16_t value);
    std::ofstream m_output;
    int m_width = 0, m_height = 0;
    int64_t m_timestampMs = 0;
    bool m_pending = false;
    std::array<uint8_t, 256 * 3> m_palette{};
    std::vector<uint8_t> m_indices;
};
}
