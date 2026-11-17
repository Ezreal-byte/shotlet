#include "ScreenRecorder.h"
#include "GifWriter.h"
#include "platform/PlatformWin32.h"
#include "utils/PcmMixBuffer.h"
#include "utils/PausableClock.h"
#include "utils/ScopeExit.h"
#include "utils/Logger.h"
#include "utils/Win32Utils.h"
#include <SDL3/SDL.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <limits>
#include <thread>

namespace shotlet {
namespace {
using Microsoft::WRL::ComPtr;
using Win32Utils::CheckHResult;
using Win32Utils::MediaTicksPerSecond;
using Win32Utils::PerformanceCounter100ns;
constexpr int AudioSampleRateHz = 48000;
constexpr int AudioChannels = 2;
constexpr int AudioBitsPerSample = 32;
constexpr int AudioBlockFrames = AudioSampleRateHz / 100;
constexpr int AudioMixCapacityFrames = AudioSampleRateHz * 2;
constexpr int AudioMixLatencyFrames = AudioSampleRateHz / 10;
// 仅在两个音源均可用时衰减，避免叠加削波；单音源保持原始音量。
constexpr float MixedAudioGain = 0.5f, UnityAudioGain = 1.0f;
constexpr DWORD AudioBitrateBitsPerSecond = 160000;
constexpr REFERENCE_TIME AudioBufferDurationTicks = MediaTicksPerSecond / 10;
constexpr auto AudioPollInterval = std::chrono::milliseconds(5);
constexpr int BgraBytesPerPixel = 4;
constexpr int BitsPerByte = 8;
constexpr LONGLONG VideoFrameDurationTicks = MediaTicksPerSecond / RecordingFramesPerSecond;

class GdiRecordingFrame {
public:
    ~GdiRecordingFrame() {
        if (m_previous) SelectObject(m_memory, m_previous);
        if (m_bitmap) DeleteObject(m_bitmap);
        if (m_memory) DeleteDC(m_memory);
        if (m_screen) ReleaseDC(nullptr, m_screen);
    }
    bool Create(const RecordingOptions& options, std::string& error) {
        m_region = options.region; m_width = options.widthPixels; m_height = options.heightPixels; m_showCursor = options.showCursor;
        m_screen = GetDC(nullptr);
        if (m_screen) m_memory = CreateCompatibleDC(m_screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = m_width;
        // RGB32 的默认 Media Foundation 布局与 GDI 一致，使用自底向上的 DIB。
        info.bmiHeader.biHeight = m_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = BgraBytesPerPixel * BitsPerByte;
        info.bmiHeader.biCompression = BI_RGB;
        if (m_memory) m_bitmap = CreateDIBSection(m_screen, &info, DIB_RGB_COLORS, &m_pixels, nullptr, 0);
        if (!m_bitmap || !m_pixels) { error = "无法创建录屏帧缓冲区"; return false; }
        m_previous = SelectObject(m_memory, m_bitmap);
        if (!m_previous || m_previous == HGDI_ERROR) { m_previous = nullptr; error = "无法绑定录屏帧缓冲区"; return false; }
        return true;
    }
    bool Capture(std::string& error) {
        if (!BitBlt(m_memory, 0, 0, m_width, m_height, m_screen, int(m_region.x), int(m_region.y), SRCCOPY | CAPTUREBLT)) {
            error = "屏幕采集失败，请检查显示器或远程桌面状态"; return false;
        }
        CURSORINFO cursor{}; cursor.cbSize = sizeof(cursor);
        ICONINFO icon{};
        if (m_showCursor && GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && GetIconInfo(cursor.hCursor, &icon)) {
            DrawIconEx(m_memory, cursor.ptScreenPos.x - int(m_region.x) - int(icon.xHotspot),
                cursor.ptScreenPos.y - int(m_region.y) - int(icon.yHotspot), cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            if (icon.hbmMask) DeleteObject(icon.hbmMask);
            if (icon.hbmColor) DeleteObject(icon.hbmColor);
        }
        // GDI 可延迟执行；CPU 读取 DIB 前必须提交当前线程的绘图命令。
        GdiFlush();
        return true;
    }
    const void* Pixels() const { return m_pixels; }
    DWORD ByteCount() const { return DWORD(m_width) * m_height * BgraBytesPerPixel; }
private:
    HDC m_screen = nullptr, m_memory = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previous = nullptr;
    void* m_pixels = nullptr;
    RectD m_region;
    int m_width = 0, m_height = 0;
    bool m_showCursor = true;
};

class AudioEndpoint {
public:
    ~AudioEndpoint() { Close(); }
    bool TryStart(IMMDeviceEnumerator* devices, bool loopback) {
        std::string error;
        if (Open(devices, loopback, error) && Start(error)) return true;
        // Activate/Initialize/GetService/Start 任一步失败都释放部分资源，后续暂停和保存才能安全跳过该音源。
        Close();
        Logger::Warning("event=recording.audio_unavailable source={} error={}", loopback ? "system" : "microphone", error);
        return false;
    }
    void SetGain(float gain) { m_gain = gain; }
    bool Start(std::string& error) {
        if (!m_client) return true;
        m_started = CheckHResult(m_client->Start(), "无法启动音频采集", error);
        return m_started;
    }
    bool Stop(std::string& error) {
        if (!m_started) return true;
        if (!CheckHResult(m_client->Stop(), "停止音频采集失败", error)) return false;
        m_started = false;
        return true;
    }
    bool Reset(std::string& error) {
        return !m_client || CheckHResult(m_client->Reset(), "清理暂停期间的音频失败", error);
    }
    bool Drain(PcmMixBuffer& mix, LONGLONG epoch, std::string& error,
               LONGLONG cutoffTicks = std::numeric_limits<LONGLONG>::max()) {
        if (!m_capture) return true;
        UINT32 packetFrames = 0;
        if (!CheckHResult(m_capture->GetNextPacketSize(&packetFrames), "读取音频失败", error)) return false;
        while (packetFrames) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            UINT64 position = 0, clockTicks = 0;
            if (!CheckHResult(m_capture->GetBuffer(&data, &frames, &flags, &position, &clockTicks), "获取音频缓冲区失败", error)) return false;
            {
                const ScopeExit release([&] { m_capture->ReleaseBuffer(frames); });
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const LONGLONG timestamp = flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR
                        ? PerformanceCounter100ns() - LONGLONG(frames) * MediaTicksPerSecond / AudioSampleRateHz
                        : static_cast<LONGLONG>(clockTicks);
                    const auto firstFrame = (timestamp - epoch) * AudioSampleRateHz / MediaTicksPerSecond;
                    // 暂停边界可能落在一个设备包内部，裁掉边界后的样本，避免恢复后混入暂停声音。
                    const auto remainingFrames = cutoffTicks == std::numeric_limits<LONGLONG>::max() ? LONGLONG(frames) :
                        std::clamp((cutoffTicks - timestamp) * AudioSampleRateHz / MediaTicksPerSecond, LONGLONG(0), LONGLONG(frames));
                    if (remainingFrames > 0 && !mix.Add({reinterpret_cast<const float*>(data), size_t(remainingFrames) * AudioChannels}, firstFrame, m_gain)) {
                        error = "音频缓冲区超出同步范围"; return false;
                    }
                }
            }
            if (!CheckHResult(m_capture->GetNextPacketSize(&packetFrames), "读取音频失败", error)) return false;
        }
        return true;
    }
private:
    void Close() {
        if (m_started) m_client->Stop();
        m_started = false;
        m_capture.Reset(); m_client.Reset();
    }
    bool Open(IMMDeviceEnumerator* devices, bool loopback, std::string& error) {
        ComPtr<IMMDevice> device;
        const char* action = loopback ? "无法访问系统播放设备" : "无法访问麦克风设备";
        if (!CheckHResult(devices->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &device), action, error) ||
            !CheckHResult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &m_client), action, error)) return false;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = AudioChannels; format.nSamplesPerSec = AudioSampleRateHz;
        format.wBitsPerSample = AudioBitsPerSample;
        format.nBlockAlign = AudioChannels * sizeof(float);
        format.nAvgBytesPerSec = AudioSampleRateHz * format.nBlockAlign;
        // 共享模式由 Windows 音频引擎统一声道及采样率，不依赖设备原生格式。
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        return CheckHResult(m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, AudioBufferDurationTicks, 0, &format, nullptr), action, error) &&
            CheckHResult(m_client->GetService(IID_PPV_ARGS(&m_capture)), action, error);
    }
    ComPtr<IAudioClient> m_client;
    ComPtr<IAudioCaptureClient> m_capture;
    bool m_started = false;
    float m_gain = UnityAudioGain;
};

class Mp4Writer {
public:
    bool Create(const RecordingOptions& options, const std::filesystem::path& path, std::string& error) {
        constexpr UINT WriterAttributeCount = 1;
        ComPtr<IMFAttributes> attributes;
        if (!CheckHResult(MFCreateAttributes(&attributes, WriterAttributeCount), "创建编码参数失败", error) ||
            !CheckHResult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), "配置硬件编码失败", error)) return false;
        if (!CheckHResult(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &m_writer), "创建 MP4 文件失败", error)) return false;
        if (!CreateVideoStream(options, error) || (options.HasAudio() && !CreateAudioStream(error))) return false;
        m_hasAudio = options.HasAudio();
        return CheckHResult(m_writer->BeginWriting(), "启动 MP4 编码器失败", error);
    }
    bool Video(const GdiRecordingFrame& frame, LONGLONG timestamp, std::string& error) {
        return Write(m_videoStream, frame.Pixels(), frame.ByteCount(), timestamp, VideoFrameDurationTicks, error);
    }
    bool Audio(PcmMixBuffer& mix, int frames, std::string& error) {
        const auto firstFrame = mix.NextFrame();
        m_audio.resize(size_t(frames) * AudioChannels);
        mix.Read(m_audio);
        const auto timestamp = firstFrame * MediaTicksPerSecond / AudioSampleRateHz;
        const auto duration = (firstFrame + frames) * MediaTicksPerSecond / AudioSampleRateHz - timestamp;
        return Write(m_audioStream, m_audio.data(), DWORD(m_audio.size() * sizeof(float)), timestamp, duration, error);
    }
    bool EndVideo(std::string& error) {
        // 尾部只补音频时，封装器不能继续等待已经不会再到来的视频。
        return CheckHResult(m_writer->NotifyEndOfSegment(m_videoStream), "结束视频流失败", error);
    }
    bool Finish(std::string& error) {
        if (m_hasAudio && !CheckHResult(m_writer->NotifyEndOfSegment(m_audioStream), "结束音频流失败", error)) return false;
        Logger::Info("event=recording.mp4_finalizing");
        const bool saved = CheckHResult(m_writer->Finalize(), "完成 MP4 封装失败", error);
        Logger::Info("event=recording.mp4_finalized success={}", saved);
        return saved;
    }
private:
    bool CreateVideoStream(const RecordingOptions& options, std::string& error) {
        constexpr DWORD MinimumVideoBitrate = 2000000, MaximumVideoBitrate = 40000000;
        constexpr double BitsPerPixelPerFrame = 0.14;
        const auto bitrate = std::clamp(DWORD(options.widthPixels * double(options.heightPixels) *
            RecordingFramesPerSecond * BitsPerPixelPerFrame), MinimumVideoBitrate, MaximumVideoBitrate);
        ComPtr<IMFMediaType> output, input;
        if (!CheckHResult(MFCreateMediaType(&output), "创建视频格式失败", error)) return false;
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, bitrate); output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, options.widthPixels, options.heightPixels);
        MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, RecordingFramesPerSecond, 1);
        MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (!CheckHResult(m_writer->AddStream(output.Get(), &m_videoStream), "系统 H.264 编码器不可用", error) ||
            !CheckHResult(MFCreateMediaType(&input), "创建视频输入格式失败", error)) return false;
        output->CopyAllItems(input.Get());
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        input->DeleteItem(MF_MT_AVG_BITRATE);
        return CheckHResult(m_writer->SetInputMediaType(m_videoStream, input.Get(), nullptr), "配置视频编码器失败", error);
    }
    bool CreateAudioStream(std::string& error) {
        constexpr DWORD EncodedAudioBitsPerSample = 16;
        ComPtr<IMFMediaType> output, input;
        if (!CheckHResult(MFCreateMediaType(&output), "创建音频格式失败", error)) return false;
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioChannels);
        output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioSampleRateHz);
        output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, EncodedAudioBitsPerSample);
        output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioBitrateBitsPerSecond / BitsPerByte);
        if (!CheckHResult(m_writer->AddStream(output.Get(), &m_audioStream), "系统 AAC 编码器不可用", error) ||
            !CheckHResult(MFCreateMediaType(&input), "创建音频输入格式失败", error)) return false;
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioChannels);
        input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioSampleRateHz);
        input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AudioBitsPerSample);
        input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AudioChannels * sizeof(float));
        input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioSampleRateHz * AudioChannels * sizeof(float));
        return CheckHResult(m_writer->SetInputMediaType(m_audioStream, input.Get(), nullptr), "配置音频编码器失败", error);
    }
    bool Write(DWORD stream, const void* bytes, DWORD size, LONGLONG timestamp, LONGLONG duration, std::string& error) {
        ComPtr<IMFMediaBuffer> buffer; ComPtr<IMFSample> sample;
        if (!CheckHResult(MFCreateMemoryBuffer(size, &buffer), "创建编码缓冲区失败", error)) return false;
        BYTE* target = nullptr;
        if (!CheckHResult(buffer->Lock(&target, nullptr, nullptr), "锁定编码缓冲区失败", error)) return false;
        {
            const ScopeExit unlock([&] { buffer->Unlock(); });
            std::memcpy(target, bytes, size);
        }
        if (!CheckHResult(buffer->SetCurrentLength(size), "设置编码数据长度失败", error) ||
            !CheckHResult(MFCreateSample(&sample), "创建媒体样本失败", error)) return false;
        sample->AddBuffer(buffer.Get()); sample->SetSampleTime(timestamp); sample->SetSampleDuration(duration);
        return CheckHResult(m_writer->WriteSample(stream, sample.Get()), "写入录制数据失败", error);
    }
    ComPtr<IMFSinkWriter> m_writer;
    DWORD m_videoStream = 0, m_audioStream = 0;
    std::vector<float> m_audio;
    bool m_hasAudio = false;
};

class WindowsScreenRecorder final : public IScreenRecorder {
public:
    ~WindowsScreenRecorder() override { std::string ignored; Stop(ignored); }
    bool Start(const RecordingOptions& options, const std::filesystem::path& path, std::string& error) override {
        if (m_worker.joinable()) { error = "录制已开始"; return false; }
        m_stop.store(false);
        m_pauseRequested.store(false);
        m_paused = false;
        m_stopTimestampTicks.store(0);
        { std::lock_guard lock(m_mutex); m_error.clear(); }
        std::promise<bool> started;
        auto result = started.get_future();
        m_worker = std::thread([this, options, path, started = std::move(started)]() mutable {
            m_running.store(true);
            std::string failure;
            bool reported = false;
            try { Record(options, path, started, reported, failure); }
            catch (const std::exception& exception) { failure = exception.what(); }
            if (!failure.empty()) { std::lock_guard lock(m_mutex); m_error = std::move(failure); }
            if (!reported) started.set_value(false);
            { std::lock_guard lock(m_waitMutex); m_running.store(false); m_wake.notify_all(); }
        });
        if (result.get()) return true;
        m_worker.join(); error = GetError(); return false;
    }
    bool Stop(std::string& error) override {
        {
            std::lock_guard lock(m_waitMutex);
            LONGLONG unset = 0;
            m_stopTimestampTicks.compare_exchange_strong(unset, PerformanceCounter100ns());
            m_stop.store(true); m_wake.notify_all();
        }
        if (m_worker.joinable()) m_worker.join();
        error = GetError(); return error.empty();
    }
    std::string GetError() const override { std::lock_guard lock(m_mutex); return m_error; }
    bool SetPaused(bool paused, std::string& error) override {
        error.clear();
        std::unique_lock lock(m_waitMutex);
        if (m_stop.load() || !m_running.load()) { error = "录制已停止"; return false; }
        m_pauseRequested.store(paused); m_wake.notify_all();
        // 只在工作线程完成音频清理和时间轴切换后确认状态，界面不会阻塞。
        m_wake.wait(lock, [&] { return m_paused == paused || m_stop.load() || !m_running.load(); });
        if (!m_stop.load() && m_running.load()) return true;
        lock.unlock();
        error = GetError();
        if (error.empty()) error = "录制已停止";
        return false;
    }
private:
    RecordingOptions StartAvailableAudio(const RecordingOptions& requested, AudioEndpoint& system, AudioEndpoint& microphone) {
        auto active = requested;
        active.systemAudio = active.microphone = false;
        if (!requested.HasAudio()) return active;
        ComPtr<IMMDeviceEnumerator> devices;
        std::string audioError;
        if (!CheckHResult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)),
            "获取音频设备失败", audioError)) {
            Logger::Warning("event=recording.audio_unavailable source=enumerator error={}", audioError);
            return active;
        }
        // 两个方向分别尝试，不能因为缺少麦克风而丢掉系统声音，也不能因无播放设备而中断画面。
        active.systemAudio = requested.systemAudio && system.TryStart(devices.Get(), true);
        active.microphone = requested.microphone && microphone.TryStart(devices.Get(), false);
        const float gain = active.systemAudio && active.microphone ? MixedAudioGain : UnityAudioGain;
        system.SetGain(gain); microphone.SetGain(gain);
        Logger::Info("event=recording.audio_configured system_audio={} microphone={}", active.systemAudio, active.microphone);
        return active;
    }
    void AcknowledgePause(bool paused) {
        std::lock_guard lock(m_waitMutex);
        m_paused = paused; m_wake.notify_all();
    }
    void WaitForResume() {
        std::unique_lock lock(m_waitMutex);
        m_paused = true; m_wake.notify_all();
        m_wake.wait(lock, [&] { return m_stop.load() || !m_pauseRequested.load(); });
    }
    bool WriteAudioThrough(Mp4Writer& writer, PcmMixBuffer& mix, LONGLONG durationTicks, std::string& error) {
        const auto finalFrame = durationTicks * AudioSampleRateHz / MediaTicksPerSecond;
        while (mix.NextFrame() < finalFrame) {
            const int count = int(std::min<int64_t>(AudioBlockFrames, finalFrame - mix.NextFrame()));
            if (!writer.Audio(mix, count, error)) return false;
        }
        return true;
    }
    bool PauseMp4(AudioEndpoint& system, AudioEndpoint& microphone, Mp4Writer& writer, PcmMixBuffer& mix,
                  LONGLONG& epoch, bool hasAudio, std::string& error) {
        const auto pausedAt = PerformanceCounter100ns();
        if (!system.Stop(error) || !microphone.Stop(error) ||
            !system.Drain(mix, epoch, error, pausedAt) || !microphone.Drain(mix, epoch, error, pausedAt) ||
            (hasAudio && !WriteAudioThrough(writer, mix, pausedAt - epoch, error))) return false;
        WaitForResume();
        if (m_stop.load()) {
            // 暂停状态结束时，音视频的尾部停在暂停边界，不能补入等待期间的静音。
            m_stopTimestampTicks.store(pausedAt);
            return true;
        }
        epoch += PerformanceCounter100ns() - pausedAt;
        if (!system.Reset(error) || !microphone.Reset(error) || !system.Start(error) || !microphone.Start(error)) return false;
        AcknowledgePause(false);
        return true;
    }
    void Record(const RecordingOptions& options, const std::filesystem::path& path,
                std::promise<bool>& started, bool& reported, std::string& error) {
        if (options.format == RecordingFormat::Gif) { RecordGif(options, path, started, reported, error); return; }
        if (!CheckHResult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "初始化录屏 COM 环境失败", error)) return;
        const ScopeExit com([] { CoUninitialize(); });
        if (!CheckHResult(MFStartup(MF_VERSION), "初始化系统媒体组件失败", error)) return;
        const ScopeExit media([] { MFShutdown(); });
        GdiRecordingFrame frame;
        Mp4Writer writer;
        AudioEndpoint system, microphone;
        const auto activeOptions = StartAvailableAudio(options, system, microphone);
        // 只有能实际启动的音源才创建 AAC 音轨；全部不可用时直接封装纯视频，不修改用户的音频偏好。
        if (!frame.Create(activeOptions, error) || !writer.Create(activeOptions, path, error)) return;
        PcmMixBuffer mix(AudioChannels, AudioMixCapacityFrames);
        // 画面开始才确定时间原点，编码器初始化期间已采集的旧音频会按时间戳丢弃。
        auto epoch = PerformanceCounter100ns();
        if (!frame.Capture(error) || !writer.Video(frame, 0, error)) return;
        reported = true; started.set_value(true);
        LONGLONG lastVideoTick = 0;
        while (!m_stop.load()) {
            if (m_pauseRequested.load() && !PauseMp4(system, microphone, writer, mix, epoch, activeOptions.HasAudio(), error)) break;
            if (m_stop.load()) break;
            const auto elapsed = PerformanceCounter100ns() - epoch;
            if (!system.Drain(mix, epoch, error) || !microphone.Drain(mix, epoch, error)) break;
            const auto audioReadyFrame = elapsed * AudioSampleRateHz / MediaTicksPerSecond - AudioMixLatencyFrames;
            while (!m_stop.load() && activeOptions.HasAudio() && mix.NextFrame() + AudioBlockFrames <= audioReadyFrame)
                if (!writer.Audio(mix, AudioBlockFrames, error)) break;
            if (!error.empty()) break;
            if (elapsed - lastVideoTick >= VideoFrameDurationTicks) {
                if (!frame.Capture(error) || !writer.Video(frame, elapsed, error)) break;
                lastVideoTick = elapsed;
            }
            std::unique_lock lock(m_waitMutex);
            m_wake.wait_for(lock, AudioPollInterval, [&] { return m_stop.load() || m_pauseRequested.load(); });
        }
        if (!error.empty()) return;
        Logger::Info("event=recording.audio_stopping");
        // 先停止设备再排空，避免边补尾部边产生新包，收尾时长也不能继续增长。
        if (!system.Stop(error) || !microphone.Stop(error)) return;
        if (!system.Drain(mix, epoch, error) || !microphone.Drain(mix, epoch, error)) return;
        if (!writer.EndVideo(error)) return;
        const auto duration = std::max(m_stopTimestampTicks.load() - epoch, lastVideoTick + VideoFrameDurationTicks);
        if (activeOptions.HasAudio() && !WriteAudioThrough(writer, mix, duration, error)) return;
        writer.Finish(error);
    }
    void RecordGif(const RecordingOptions& options, const std::filesystem::path& path,
                   std::promise<bool>& started, bool& reported, std::string& error) {
        GdiRecordingFrame frame; GifWriter writer;
        if (!frame.Create(options, error) || !writer.Open(path, options.widthPixels, options.heightPixels, error) ||
            !frame.Capture(error) || !writer.AddBgraFrame(static_cast<const uint8_t*>(frame.Pixels()), options.widthPixels * BgraBytesPerPixel, true, 0, error)) return;
        PausableClock clock;
        clock.Reset();
        constexpr auto Interval = std::chrono::milliseconds(1000 / GifFramesPerSecond);
        reported = true; started.set_value(true);
        while (!m_stop.load()) {
            std::unique_lock lock(m_waitMutex);
            m_wake.wait_for(lock, Interval, [&] { return m_stop.load() || m_pauseRequested.load(); });
            if (m_stop.load()) break;
            lock.unlock();
            if (m_pauseRequested.load()) {
                clock.SetPaused(true);
                WaitForResume();
                if (m_stop.load()) break;
                clock.SetPaused(false);
                AcknowledgePause(false);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Elapsed()).count();
            if (!frame.Capture(error) || !writer.AddBgraFrame(static_cast<const uint8_t*>(frame.Pixels()), options.widthPixels * BgraBytesPerPixel, true, elapsed, error)) return;
        }
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Elapsed()).count();
        writer.Finish(duration, error);
    }
    std::thread m_worker;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_pauseRequested{false}, m_running{false};
    bool m_paused = false;
    std::atomic<LONGLONG> m_stopTimestampTicks{0};
    mutable std::mutex m_mutex;
    std::mutex m_waitMutex;
    std::condition_variable m_wake;
    std::string m_error;
};
}

std::unique_ptr<IScreenRecorder> CreateScreenRecorder(RecordingFormat) { return std::make_unique<WindowsScreenRecorder>(); }
bool IsScreenRecordingSupported() { return true; }
bool PrepareRecordingPermissions(std::string& error, bool) { error.clear(); return true; }
uint32_t RecordingControlWindowId(SDL_Window*) { return 0; }
void ExcludeRecordingControlWindow(SDL_Window* window) {
    const auto handle = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (handle) SetWindowDisplayAffinity(handle, WDA_EXCLUDEFROMCAPTURE);
}
}
