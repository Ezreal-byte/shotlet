#include "recording/RecordingVisuals.h"
#include "platform/ApplicationPlatform.h"
#include "platform/PlatformWin32.h"
#include "utils/ScopeExit.h"
#include "utils/Win32Utils.h"
#include "native_events_win.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <gtest/gtest.h>
#include <future>
#include <iterator>
#include <tuple>

namespace shotlet {
namespace {
constexpr int CaptureWidthPixels = 320, CaptureHeightPixels = 240;
constexpr auto CameraWarmup = std::chrono::milliseconds(500);
constexpr auto RecordingDuration = std::chrono::milliseconds(500);
constexpr auto MaximumUiCloseTime = std::chrono::milliseconds(500);

class WindowsRecordingStopTest : public testing::Test {
protected:
    void SetUp() override {
        InitializeApplicationPlatform();
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
        ASSERT_TRUE(PrepareRecordingOptions(0, {0, 0, CaptureWidthPixels, CaptureHeightPixels}, 1, m_options));
        m_options.camera = true;
        std::string error;
        m_camera = CreateRecordingCamera(m_options, error);
        ASSERT_NE(m_camera, nullptr) << error;
        TestUtils::PumpFor(CameraWarmup);
        error = m_camera->GetError();
        if (!error.empty()) GTEST_SKIP() << "测试需要可用摄像头：" << error;
    }
    void TearDown() override {
        m_camera.reset();
        SDL_Quit();
    }
    RecordingOptions m_options;
    std::unique_ptr<RecordingCamera> m_camera;
};

TEST_F(WindowsRecordingStopTest, ClosingCameraWithPendingReadKeepsUiResponsive) {
    const auto before = std::chrono::steady_clock::now();
    m_camera.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - before, MaximumUiCloseTime);
    TestUtils::PumpFor(TestUtils::NativePollInterval);
}

TEST_F(WindowsRecordingStopTest, Mp4WithCameraAndMicrophoneStopsAndCommitsFile) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("shotlet-recording-stop-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "recording.mp4";
    const ScopeExit cleanup([&] { std::error_code ignored; std::filesystem::remove_all(directory, ignored); });
    RecordingSession session;
    std::string error;
    auto start = std::async(std::launch::async, [&] { return session.Start(m_options, path, error); });
    if (!TestUtils::Await(start)) GTEST_SKIP() << "测试需要可用的 H.264 编码器及屏幕采集环境：" << error;
    TestUtils::PumpFor(RecordingDuration);
    auto stop = std::async(std::launch::async, [&] { return session.Stop(error); });
    ASSERT_TRUE(TestUtils::Await(stop)) << error;
    const auto before = std::chrono::steady_clock::now();
    m_camera.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - before, MaximumUiCloseTime);
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0);
    // 只在完整封装并原子提交后留下正式 MP4，不得遗留半成品。
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

// 不以音频或摄像头硬件为前提；无音频设备的 Windows 环境也必须执行并成功产出画面。
class WindowsRecordingAudioTest : public testing::TestWithParam<std::tuple<bool, bool>> {
protected:
    void SetUp() override {
        std::string error;
        ASSERT_TRUE(Win32Utils::CheckHResult(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "初始化测试 COM 失败", error)) << error;
        m_comInitialized = true;
        ASSERT_TRUE(Win32Utils::CheckHResult(MFStartup(MF_VERSION), "初始化测试媒体组件失败", error)) << error;
        m_mediaStarted = true;
        InitializeApplicationPlatform();
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
        ASSERT_TRUE(PrepareRecordingOptions(0, {0, 0, CaptureWidthPixels, CaptureHeightPixels}, 1, m_options));
        m_options.systemAudio = std::get<0>(GetParam()); m_options.microphone = std::get<1>(GetParam());
        m_options.showCursor = false;
        m_directory = std::filesystem::temp_directory_path() /
            ("shotlet-recording-audio-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directory(m_directory);
    }
    void TearDown() override {
        std::error_code ignored;
        if (!m_directory.empty()) std::filesystem::remove_all(m_directory, ignored);
        SDL_Quit();
        if (m_mediaStarted) MFShutdown();
        if (m_comInitialized) CoUninitialize();
    }
    bool HasRequestedDevice() const {
        if (!m_options.HasAudio()) return false;
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)))) return false;
        const auto available = [&](EDataFlow flow) {
            Microsoft::WRL::ComPtr<IMMDevice> device;
            return SUCCEEDED(devices->GetDefaultAudioEndpoint(flow, eConsole, &device));
        };
        return (m_options.systemAudio && available(eRender)) || (m_options.microphone && available(eCapture));
    }
    void VerifyVideo(const std::filesystem::path& path, bool requestedDeviceExists) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMFSourceReader> reader;
        ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)));
        ComPtr<IMFMediaType> audio;
        const auto audioResult = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio);
        if (!requestedDeviceExists) EXPECT_TRUE(FAILED(audioResult)) << "无可用音源时不得创建空音轨";
        ComPtr<IMFMediaType> output;
        ASSERT_TRUE(SUCCEEDED(MFCreateMediaType(&output)));
        ASSERT_TRUE(SUCCEEDED(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
        ASSERT_TRUE(SUCCEEDED(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)));
        ASSERT_TRUE(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)));
        ASSERT_TRUE(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)));
        ASSERT_TRUE(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get())));
        int decodedFrames = 0;
        while (true) {
            DWORD flags = 0;
            LONGLONG timestampTicks = 0;
            ComPtr<IMFSample> sample;
            ASSERT_TRUE(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestampTicks, &sample)));
            ASSERT_EQ(flags & MF_SOURCE_READERF_ERROR, 0u);
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> pixels;
            ASSERT_TRUE(SUCCEEDED(sample->ConvertToContiguousBuffer(&pixels)));
            DWORD pixelBytes = 0;
            ASSERT_TRUE(SUCCEEDED(pixels->GetCurrentLength(&pixelBytes)));
            ASSERT_GT(pixelBytes, 0u);
            ++decodedFrames;
        }
        EXPECT_GT(decodedFrames, 0);
    }
    RecordingOptions m_options;
    std::filesystem::path m_directory;
    bool m_comInitialized = false, m_mediaStarted = false;
};

TEST_P(WindowsRecordingAudioTest, RequestedAudioDoesNotBlockVideoAndPausedSave) {
    const bool requestedDeviceExists = HasRequestedDevice();
    const auto path = m_directory / "recording.mp4";
    RecordingSession session;
    std::string error;
    auto start = std::async(std::launch::async, [&] { return session.Start(m_options, path, error); });
    ASSERT_TRUE(TestUtils::Await(start)) << error;
    TestUtils::PumpFor(RecordingDuration);
    for (bool paused : {true, false, true}) {
        auto change = std::async(std::launch::async, [&] { return session.SetPaused(paused, error); });
        ASSERT_TRUE(TestUtils::Await(change)) << error;
        EXPECT_EQ(session.IsPaused(), paused);
        TestUtils::PumpFor(TestUtils::NativePollInterval);
    }
    auto stop = std::async(std::launch::async, [&] { return session.Stop(error); });
    ASSERT_TRUE(TestUtils::Await(stop)) << error;
    EXPECT_TRUE(session.GetError().empty());
    ASSERT_TRUE(std::filesystem::exists(path));
    VerifyVideo(path, requestedDeviceExists);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(m_directory), std::filesystem::directory_iterator()), 1);
}

INSTANTIATE_TEST_SUITE_P(AudioOptions, WindowsRecordingAudioTest, testing::Combine(testing::Bool(), testing::Bool()));
} // namespace
} // namespace shotlet
