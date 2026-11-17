#include "Application.h"
#include "core/Canvas.h"
#include "platform/FileService.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"
#include "utils/FilePathUtils.h"
#include <SDL3/SDL.h>

namespace shotlet {
bool Application::RecordingBusy() const {
    return m_recordingSession || (m_recordingWindow && m_recordingWindow->GetState() == RecordingWindow::State::Ready);
}

void Application::OnRecordRequested() {
    const auto* config = m_overlayManager->GetResultConfig();
    const auto* canvas = m_overlayManager->GetResultCanvas();
    RecordingOptions options;
    bool valid = false;
    if (config && canvas && canvas->HasSelection()) {
        const auto selection = canvas->GetSelection();
        const auto origin = m_overlayManager->GetResultSelectionScreenOrigin();
#if defined(SHOTLET_PLATFORM_WINDOWS)
        constexpr double PixelDensity = 1;
#else
        const double PixelDensity = config->scaleFactor;
#endif
        valid = PrepareRecordingOptions(config->displayId,
            {origin.x, origin.y, selection.GetWidth(), selection.GetHeight()}, PixelDensity, options);
    }
    // 释放冻结截图和标注资源后才采集实时桌面，标注不会进入录制内容。
    CleanupAfterCapture("recording-selection");
    auto fail = [&](const std::string& error) {
        m_exitCode = ExitError;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Tr(m_language, TextId::RecordingFailed), error.c_str(), nullptr);
        if (!m_daemonMode) Quit();
    };
    if (!valid) { fail(Tr(m_language, TextId::RecordingInvalidRegion)); return; }
    std::string error;
    m_recordingBorder = CreateRecordingBorder(options, error);
    if (!m_recordingBorder) { fail(error); return; }
    m_recordingWindow = std::make_unique<RecordingWindow>(m_gpuDevice, m_language);
    if (!m_recordingWindow->Show(options)) {
        m_recordingWindow.reset(); m_recordingBorder.reset(); fail(SDL_GetError()); return;
    }
    m_recordingOptions = options;
    m_exitCode = ExitCancelled;
}

void Application::SyncRecordingCamera() {
    if (!m_recordingWindow) return;
    if (m_recordingWindow->GetState() == RecordingWindow::State::Ready) {
        m_recordingOptions = m_recordingWindow->Options();
        if (m_recordingOptions.camera && !m_recordingCamera) {
            std::string error;
            m_recordingCamera = CreateRecordingCamera(m_recordingOptions, error);
            if (!m_recordingCamera) m_recordingWindow->DisableCamera(error);
            else SDL_RaiseWindow(m_recordingWindow->Window());
        } else if (!m_recordingOptions.camera) m_recordingCamera.reset();
    }
    if (m_recordingCamera) {
        const auto error = m_recordingCamera->GetError();
        if (error.empty()) return;
        if (m_recordingWindow->GetState() == RecordingWindow::State::Ready) {
            m_recordingWindow->DisableCamera(error); m_recordingCamera.reset();
        } else StopRecording();
    }
}

void Application::StartRecording() {
    if (!m_recordingWindow || m_recordingWindow->GetState() != RecordingWindow::State::Ready) return;
    auto options = m_recordingWindow->Options();
    if (options.camera && !m_recordingCamera) { m_recordingWindow->SetDetail("摄像头预览尚未就绪"); return; }
    options.cameraWindowId = m_recordingCamera ? m_recordingCamera->CaptureWindowId() : 0;
    options.excludedWindowIds = m_recordingBorder ? m_recordingBorder->CaptureWindowIds() : std::vector<uint32_t>();
    if (const auto control = RecordingControlWindowId(m_recordingWindow->Window())) options.excludedWindowIds.push_back(control);
    std::string error;
    if (!PrepareRecordingPermissions(error, options.HasAudio() && options.microphone)) { m_recordingWindow->SetDetail(error); return; }
    const auto path = CreateRecordingSavePath(DefaultRecordingDirectory(), RecordingExtension(options.format), error);
    if (path.empty()) { m_recordingWindow->SetDetail(error); return; }
    m_recordingOptions = options;
    m_recordingPath = path;
    m_recordingStopPending = false;
    m_recordingSession = std::make_shared<RecordingSession>();
    m_recordingWindow->SetState(RecordingWindow::State::Starting);
    auto session = m_recordingSession;
    try {
        m_recordingTask = std::async(std::launch::async, [session, options, path] {
            RecordingTaskResult result;
            result.success = session->Start(options, path, result.error);
            return result;
        });
    } catch (const std::exception& exception) {
        m_recordingSession.reset();
        m_recordingBorder.reset(); m_recordingCamera.reset();
        m_recordingWindow->SetState(RecordingWindow::State::Failed, exception.what());
        m_exitCode = ExitError;
    }
}

void Application::StopRecording() {
    if (!m_recordingSession) return;
    if (m_recordingTask.valid()) { m_recordingStopPending = true; return; }
    m_recordingStopPending = false;
    Logger::Info("event=recording.stop_requested camera={} audio={}", m_recordingOptions.camera, m_recordingOptions.HasAudio());
    m_recordingWindow->SetState(RecordingWindow::State::Saving);
    auto session = m_recordingSession;
    try {
        m_recordingTask = std::async(std::launch::async, [session] {
            RecordingTaskResult result;
            result.success = session->Stop(result.error);
            return result;
        });
    } catch (const std::exception&) {
        // 无法创建后台线程时同步收尾，确保流和文件不遗留在后台。
        std::string error;
        const bool saved = session->Stop(error);
        CompleteRecording(saved, error);
    }
}

void Application::ToggleRecordingPause() {
    using State = RecordingWindow::State;
    if (!m_recordingSession || !m_recordingWindow || m_recordingTask.valid()) return;
    const auto previous = m_recordingWindow->GetState();
    if (previous != State::Recording && previous != State::Paused) return;
    const bool paused = previous == State::Recording;
    m_recordingWindow->SetState(paused ? State::Pausing : State::Resuming);
    auto session = m_recordingSession;
    try {
        m_recordingTask = std::async(std::launch::async, [session, paused] {
            RecordingTaskResult result;
            result.success = session->SetPaused(paused, result.error);
            return result;
        });
    } catch (const std::exception& exception) {
        m_recordingWindow->SetState(previous, exception.what());
    }
}

void Application::ShowRecordingPlayback() {
    if (!m_recordingWindow || m_recordingWindow->GetState() != RecordingWindow::State::Saved) return;
    Logger::Info("event=recording.playback_opening");
    std::string error;
    auto playback = CreateRecordingPlaybackWindow(m_recordingPath, m_recordingOptions, m_language, m_theme, error);
    if (playback) {
        m_recordingPlayback = std::move(playback);
        m_recordingWindow.reset();
    } else {
        // 预览失败不会改变保存结果，仍可重试播放或在文件夹中定位成片。
        m_recordingWindow->SetDetail(FilePathUtils::ToUtf8(m_recordingPath.filename()) + "\n" +
            Tr(m_language, TextId::RecordingPlaybackFailed) + ": " + error);
        Logger::Warning("event=recording.playback_failed error={}", error);
    }
}

void Application::CompleteRecording(bool saved, const std::string& error) {
    m_recordingSession.reset();
    m_recordingBorder.reset(); m_recordingCamera.reset();
    m_exitCode = saved ? ExitSuccess : ExitError;
    if (saved) m_hostActionHandled = true;
    m_recordingWindow->SetState(saved ? RecordingWindow::State::Saved : RecordingWindow::State::Failed,
        saved ? FilePathUtils::ToUtf8(m_recordingPath.filename()) : error);
    // Stop 已完成原子提交才允许播放；退出收尾期间不再创建任何结果窗口。
    if (saved && m_running && !m_systemTray.IsQuitRequested()) ShowRecordingPlayback();
}

void Application::ProcessRecording() {
    if (m_recordingPlayback) {
        m_recordingPlayback->Update(m_theme);
        if (!m_recordingPlayback->IsOpen()) {
            m_recordingPlayback.reset();
            if (!m_daemonMode) Quit();
        }
    }
    if (!m_recordingWindow) return;
    SyncRecordingCamera();
    using State = RecordingWindow::State;
    constexpr auto Ready = std::chrono::seconds(0);
    if (m_recordingTask.valid() && m_recordingTask.wait_for(Ready) == std::future_status::ready) {
        RecordingTaskResult result;
        try { result = m_recordingTask.get(); }
        catch (const std::exception& exception) { result.error = exception.what(); }
        const auto state = m_recordingWindow->GetState();
        const bool starting = state == State::Starting;
        if (result.success && starting) {
            m_recordingWindow->SetState(State::Recording);
            Logger::Info("event=recording.started format={} system_audio={} microphone={} camera={}",
                RecordingExtension(m_recordingOptions.format), m_recordingOptions.HasAudio() && m_recordingOptions.systemAudio,
                m_recordingOptions.HasAudio() && m_recordingOptions.microphone, m_recordingOptions.camera);
            if (m_recordingStopPending) StopRecording();
        } else if (state == State::Pausing || state == State::Resuming) {
            const bool paused = state == State::Pausing ? result.success : !result.success;
            m_recordingWindow->SetState(paused ? State::Paused : State::Recording, result.error);
            Logger::Info("event=recording.pause_changed paused={} success={} error={}", paused, result.success, result.error);
            // 暂停/继续期间收到的结束请求必须等当前原生操作完成，避免并发访问编码器。
            if (m_recordingStopPending) StopRecording();
        } else {
            CompleteRecording(result.success, result.error);
            Logger::Info("event=recording.finished success={} error={}", result.success, result.error);
        }
    }
    if (!m_recordingWindow) return;
    if (m_recordingSession && !m_recordingTask.valid() &&
        (m_recordingSession->HasStopped() || !m_recordingSession->GetError().empty() || m_recordingWindow->HasReachedDurationLimit())) StopRecording();
    if (!m_recordingWindow) return;
    m_recordingWindow->Render(m_theme);
    // 先取完本帧请求，停止和打开预览均可能销毁录制控制窗口。
    const bool start = m_recordingWindow->ConsumeStartRequest(), stop = m_recordingWindow->ConsumeStopRequest();
    const bool reveal = m_recordingWindow->ConsumeRevealRequest(), play = m_recordingWindow->ConsumePlayRequest();
    const bool close = m_recordingWindow->ConsumeCloseRequest();
    const bool pause = m_recordingWindow->ConsumePauseRequest();
    if (start) StartRecording();
    if (stop) StopRecording();
    else if (pause) ToggleRecordingPause();
    if (reveal) RevealInFileManager(m_recordingPath);
    if (play) ShowRecordingPlayback();
    if (close && m_recordingWindow) {
        if (m_recordingWindow->GetState() == State::Ready) m_exitCode = ExitCancelled;
        m_recordingCamera.reset(); m_recordingBorder.reset();
        m_recordingWindow.reset();
        if (!m_daemonMode) Quit();
    }
}

bool Application::RouteRecordingEvent(const SDL_Event& event) {
    return m_recordingWindow && m_recordingWindow->ProcessEvent(event);
}

void Application::FinishRecordingForShutdown() {
    m_running = false;
    const auto drainTask = [this] {
        if (!m_recordingTask.valid()) return;
        // macOS 的完成回调可能需要主队列；退出等待期间继续泵系统事件。
        constexpr auto PollInterval = std::chrono::milliseconds(10);
        while (m_recordingTask.wait_for(PollInterval) != std::future_status::ready) SDL_PumpEvents();
        try {
            const auto result = m_recordingTask.get();
            if (!result.success) {
                m_exitCode = ExitError;
                Logger::Warning("event=recording.shutdown error={}", result.error);
            }
        } catch (const std::exception& exception) {
            m_exitCode = ExitError;
            Logger::Warning("event=recording.shutdown error={}", exception.what());
        }
    };
    drainTask();
    if (m_recordingSession && m_recordingSession->IsActive()) { StopRecording(); drainTask(); }
    m_recordingSession.reset();
    m_recordingPlayback.reset();
    m_recordingCamera.reset(); m_recordingBorder.reset();
    m_recordingWindow.reset();
}
}
