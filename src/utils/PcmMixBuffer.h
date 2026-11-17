#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

namespace shotlet {
// 按音频帧的时间位置混合交错 PCM。固定容量限制慢设备或错误时间戳造成的内存增长。
class PcmMixBuffer {
public:
    PcmMixBuffer(int channels, int capacityFrames)
        : m_channels(channels), m_capacityFrames(capacityFrames) {
        if (channels <= 0 || capacityFrames <= 0) throw std::invalid_argument("PCM buffer dimensions must be positive");
        m_samples.resize(size_t(channels) * capacityFrames);
    }
    bool Add(std::span<const float> samples, int64_t firstFrame, float gain) {
        if (samples.size() % m_channels || !std::isfinite(gain)) return false;
        const auto frames = int64_t(samples.size() / m_channels);
        if (frames > m_capacityFrames) return false;
        if (firstFrame < m_nextFrame - frames) return true;
        if (firstFrame + frames > m_nextFrame + m_capacityFrames) return false;
        // 已输出的迟到样本不能回写到环形缓冲区，否则会污染下一圈的声音。
        const auto skip = std::max<int64_t>(0, m_nextFrame - firstFrame);
        for (int64_t frame = skip; frame < frames; ++frame) {
            const auto output = size_t((firstFrame + frame) % m_capacityFrames) * m_channels;
            for (int channel = 0; channel < m_channels; ++channel)
                if (const float sample = samples[size_t(frame) * m_channels + channel]; std::isfinite(sample))
                    m_samples[output + channel] += sample * gain;
        }
        return true;
    }
    void Read(std::span<float> output) {
        if (output.size() % m_channels) throw std::invalid_argument("PCM output must contain complete frames");
        const auto frames = output.size() / m_channels;
        for (size_t frame = 0; frame < frames; ++frame, ++m_nextFrame) {
            const auto input = size_t(m_nextFrame % m_capacityFrames) * m_channels;
            for (int channel = 0; channel < m_channels; ++channel) {
                output[frame * m_channels + channel] = std::clamp(m_samples[input + channel], -1.0f, 1.0f);
                m_samples[input + channel] = 0;
            }
        }
    }
    int64_t NextFrame() const { return m_nextFrame; }
private:
    int m_channels, m_capacityFrames;
    int64_t m_nextFrame = 0;
    std::vector<float> m_samples;
};
}
