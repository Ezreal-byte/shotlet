#include "RecordingVisuals.h"
#include "utils/Colors.h"
#include "utils/AsyncMailbox.h"
#include "utils/Logger.h"
#include "utils/ScopeExit.h"
#include "utils/Win32Utils.h"
#include "utils/WindowUtils.h"
#include <SDL3/SDL.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace shotlet {
namespace {
using Microsoft::WRL::ComPtr;
using Win32Utils::CheckHResult;
constexpr double BorderWidthLogical = 2, CameraCornerRadiusLogical = 16;
constexpr int PixelChannels = 4, BitsPerChannel = 8;
constexpr int PreviewWidthPixels = 640, PreviewHeightPixels = 480;
constexpr wchar_t BorderClass[] = L"ShotletRecordingBorder";
constexpr wchar_t CameraClass[] = L"ShotletRecordingCamera";
constexpr DWORD VisualWindowStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;

LRESULT CALLBACK BorderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message != WM_PAINT) return DefWindowProcW(window, message, wParam, lParam);
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
    constexpr auto color = Colors::SelectionBorder;
    const auto blue = RGB((color >> IM_COL32_R_SHIFT) & 255, (color >> IM_COL32_G_SHIFT) & 255, (color >> IM_COL32_B_SHIFT) & 255);
    HBRUSH brush = CreateSolidBrush(blue); FillRect(dc, &paint.rcPaint, brush); DeleteObject(brush);
    EndPaint(window, &paint); return 0;
}
HWND VisualWindow(const wchar_t* type, RectD rectangle, void* data) {
    HWND window = CreateWindowExW(VisualWindowStyle, type, L"", WS_POPUP, int(rectangle.x), int(rectangle.y),
        std::max(1, int(rectangle.width)), std::max(1, int(rectangle.height)), nullptr, nullptr, GetModuleHandleW(nullptr), data);
    if (window) { SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA); ShowWindow(window, SW_SHOWNOACTIVATE); }
    return window;
}

class WinRecordingBorder final : public RecordingBorder {
public:
    ~WinRecordingBorder() override { for (HWND window : m_windows) if (window) DestroyWindow(window); }
    bool Show(const RecordingOptions& options) {
        if (!Win32Utils::RegisterWindowClass(BorderClass, BorderProc)) return false;
        SDL_Rect region{int(options.region.x), int(options.region.y), int(options.region.width), int(options.region.height)}, display{};
        if (!SDL_GetDisplayBounds(SDL_GetDisplayForRect(&region), &display)) return false;
        const auto strips = VisibleBorderRects(options.region,
            {double(display.x), double(display.y), double(display.w), double(display.h)}, BorderWidthLogical * WindowUtils::GetContentScaleForRect(region));
        for (size_t i = 0; i < strips.size(); ++i) {
            m_windows[i] = VisualWindow(BorderClass, strips[i], nullptr);
            if (!m_windows[i]) return false;
            SetWindowDisplayAffinity(m_windows[i], WDA_EXCLUDEFROMCAPTURE);
        }
        return true;
    }
    bool IsVisible() const override {
        return std::all_of(m_windows.begin(), m_windows.end(), [](HWND window) { return window && IsWindowVisible(window); });
    }
private:
    std::array<HWND, 4> m_windows{};
};

struct CameraSample {
    HRESULT status = S_OK;
    DWORD flags = 0;
    ComPtr<IMFSample> sample;
};

struct CameraState {
    AsyncMailbox<CameraSample> samples;
    std::mutex mutex;
    HWND window = nullptr;
    std::string error;
    std::vector<BYTE> pixels;
    int width = 0, height = 0;
};

// Source Reader 可以在窗口关闭后交付最后一次回调，只持有独立状态而非预览对象。
class CameraReaderCallback final : public IMFSourceReaderCallback {
public:
    explicit CameraReaderCallback(std::shared_ptr<CameraState> state) : m_state(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFSourceReaderCallback)) return E_NOINTERFACE;
        *result = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --m_references;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override {
        CameraSample result;
        result.status = status; result.flags = flags; result.sample = sample;
        m_state->samples.Publish(std::move(result));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override { return S_OK; }
private:
    std::atomic<ULONG> m_references{1};
    std::shared_ptr<CameraState> m_state;
};

class WinRecordingCamera final : public RecordingCamera {
public:
    ~WinRecordingCamera() override {
        m_state->samples.Cancel();
        {
            std::lock_guard lock(m_state->mutex);
            m_state->window = nullptr;
        }
        if (m_window) DestroyWindow(m_window);
        // 设备关闭可能等待驱动，不能在 UI 线程 Shutdown/join。
        // 线程和回调共同拥有状态，后台释放设备期间不会再访问 this 或已销毁的 HWND。
        if (m_worker.joinable()) m_worker.detach();
    }
    bool Show(const RecordingOptions& options, std::string& error) {
        if (!Win32Utils::RegisterWindowClass(CameraClass, WindowProc)) { error = "无法注册摄像头预览窗口"; return false; }
        const SDL_Rect bounds{int(options.region.x), int(options.region.y), int(options.region.width), int(options.region.height)};
        const double scale = WindowUtils::GetContentScaleForRect(bounds);
        const auto rectangle = RecordingCameraRect(options, scale);
        m_window = VisualWindow(CameraClass, rectangle, this);
        if (!m_window) { error = "无法创建摄像头预览窗口"; return false; }
        const int radius = int(std::min(CameraCornerRadiusLogical * scale, rectangle.width / 4));
        HRGN shape = CreateRoundRectRgn(0, 0, int(rectangle.width), int(rectangle.height), radius * 2, radius * 2);
        if (shape && !SetWindowRgn(m_window, shape, TRUE)) DeleteObject(shape);
        // 摄像头不设置捕获排除，GDI 会把此窗口合成进 MP4 和 GIF。
        { std::lock_guard lock(m_state->mutex); m_state->window = m_window; }
        m_worker = std::thread([state = m_state] {
            std::string error;
            try { Capture(state, error); } catch (const std::exception& exception) { error = exception.what(); }
            if (!error.empty()) {
                std::lock_guard lock(state->mutex);
                if (state->window) state->error = std::move(error);
            }
        });
        return true;
    }
    uint32_t CaptureWindowId() const override { return 0; }
    std::string GetError() const override { std::lock_guard lock(m_state->mutex); return m_state->error; }
private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* camera = reinterpret_cast<WinRecordingCamera*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message != WM_PAINT || !camera) return DefWindowProcW(window, message, wParam, lParam);
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); RECT bounds{}; GetClientRect(window, &bounds);
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        {
            auto& state = *camera->m_state;
            std::lock_guard lock(state.mutex);
            if (!state.pixels.empty()) {
                BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = state.width; info.bmiHeader.biHeight = -state.height;
                info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = PixelChannels * BitsPerChannel; info.bmiHeader.biCompression = BI_RGB;
                const int side = std::min(state.width, state.height);
                SetStretchBltMode(dc, HALFTONE);
                StretchDIBits(dc, 0, 0, bounds.right, bounds.bottom, (state.width - side) / 2,
                    (state.height - side) / 2, side, side, state.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            }
        }
        EndPaint(window, &paint); return 0;
    }
    static bool OpenCamera(const std::shared_ptr<CameraState>& state, ComPtr<IMFMediaSource>& source,
                           ComPtr<IMFSourceReader>& reader, std::string& error) {
        ComPtr<IMFAttributes> attributes;
        if (!CheckHResult(MFCreateAttributes(&attributes, 1), "创建摄像头参数失败", error)) return false;
        attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devices = nullptr; UINT32 count = 0;
        if (!CheckHResult(MFEnumDeviceSources(attributes.Get(), &devices, &count), "读取摄像头列表失败", error)) return false;
        const ScopeExit release([&] { for (UINT32 i = 0; i < count; ++i) devices[i]->Release(); CoTaskMemFree(devices); });
        if (!count) { error = "未找到可用摄像头"; return false; }
        if (!CheckHResult(devices[0]->ActivateObject(IID_PPV_ARGS(&source)), "无法打开摄像头，请检查摄像头隐私权限", error)) return false;
        if (state->samples.IsCancelled()) return false;
        constexpr UINT ReaderAttributeCount = 3;
        ComPtr<IMFAttributes> settings;
        ComPtr<IMFSourceReaderCallback> callback;
        callback.Attach(new CameraReaderCallback(state));
        if (!CheckHResult(MFCreateAttributes(&settings, ReaderAttributeCount), "创建摄像头转换参数失败", error) ||
            !CheckHResult(settings->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "配置摄像头转换失败", error) ||
            !CheckHResult(settings->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get()), "配置摄像头异步取帧失败", error) ||
            !CheckHResult(settings->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE), "配置摄像头关闭方式失败", error)) return false;
        if (!CheckHResult(MFCreateSourceReaderFromMediaSource(source.Get(), settings.Get(), &reader), "创建摄像头采集器失败", error)) return false;
        ComPtr<IMFMediaType> type;
        if (!CheckHResult(MFCreateMediaType(&type), "创建摄像头画面格式失败", error)) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, PreviewWidthPixels, PreviewHeightPixels);
        if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()))) return true;
        type->DeleteItem(MF_MT_FRAME_SIZE);
        return CheckHResult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get()), "配置摄像头画面失败", error);
    }
    static void Capture(const std::shared_ptr<CameraState>& state, std::string& error) {
        if (!CheckHResult(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "初始化摄像头 COM 失败", error)) return;
        const ScopeExit com([] { CoUninitialize(); });
        if (!CheckHResult(MFStartup(MF_VERSION), "初始化摄像头媒体服务失败", error)) return;
        const ScopeExit media([] { MFShutdown(); });
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFSourceReader> reader;
        const ScopeExit close([&] {
            state->samples.Cancel();
            // 先归还信箱里已到达的帧，避免 Shutdown 等待仍由本线程保留的设备缓冲区。
            state->samples.DiscardPending();
            Logger::Info("event=recording.camera_closing");
            // 原生调用必须在状态锁外，并由同一 MTA 工作线程串行释放，避免交叉关闭死锁。
            if (reader) { reader->Flush(MF_SOURCE_READER_ALL_STREAMS); reader.Reset(); }
            if (source) { source->Shutdown(); source.Reset(); }
            Logger::Info("event=recording.camera_closed");
        });
        if (!OpenCamera(state, source, reader, error)) return;
        while (!state->samples.IsCancelled()) {
            if (!CheckHResult(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr), "摄像头取帧请求失败", error)) return;
            CameraSample sample;
            if (!state->samples.Wait(sample)) return;
            if (!CheckHResult(sample.status, "摄像头画面采集失败", error)) return;
            if (sample.flags & MF_SOURCE_READERF_ENDOFSTREAM) { error = "摄像头已断开"; return; }
            if (sample.sample && !CopySample(state, reader.Get(), sample.sample.Get(), error)) return;
        }
    }
    static bool CopySample(const std::shared_ptr<CameraState>& state, IMFSourceReader* reader,
                           IMFSample* sample, std::string& error) {
        ComPtr<IMFMediaType> type;
        if (!CheckHResult(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type), "读取摄像头画面格式失败", error)) return false;
        UINT32 width = 0, height = 0;
        if (!CheckHResult(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height), "读取摄像头尺寸失败", error)) return false;
        if (!width || !height) { error = "摄像头画面尺寸无效"; return false; }
        ComPtr<IMFMediaBuffer> buffer;
        if (!CheckHResult(sample->ConvertToContiguousBuffer(&buffer), "读取摄像头缓冲区失败", error)) return false;
        ComPtr<IMF2DBuffer> image;
        BYTE* pixels = nullptr; LONG stride = 0;
        if (SUCCEEDED(buffer.As(&image)) && SUCCEEDED(image->Lock2D(&pixels, &stride))) {
            const ScopeExit unlock([&] { image->Unlock2D(); });
            Publish(state, pixels, stride, width, height);
            return true;
        }
        DWORD length = 0;
        if (!CheckHResult(buffer->Lock(&pixels, nullptr, &length), "锁定摄像头缓冲区失败", error)) return false;
        const ScopeExit unlock([&] { buffer->Unlock(); });
        stride = LONG(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, UINT32(width * PixelChannels)));
        if (length < size_t(std::abs(stride)) * height) { error = "摄像头缓冲区尺寸无效"; return false; }
        if (stride < 0) pixels += size_t(height - 1) * -stride;
        Publish(state, pixels, stride, width, height);
        return true;
    }
    static void Publish(const std::shared_ptr<CameraState>& state, const BYTE* pixels, LONG stride, UINT32 width, UINT32 height) {
        std::lock_guard lock(state->mutex);
        if (!state->window) return;
        state->width = int(width); state->height = int(height); state->pixels.resize(size_t(width) * height * PixelChannels);
        for (UINT32 y = 0; y < height; ++y)
            std::memcpy(state->pixels.data() + size_t(y) * width * PixelChannels, pixels + ptrdiff_t(y) * stride, size_t(width) * PixelChannels);
        // 在句柄锁内投递非阻塞重绘请求；关闭窗口清空句柄后不再访问旧 HWND。
        InvalidateRect(state->window, nullptr, FALSE);
    }
    HWND m_window = nullptr;
    std::thread m_worker;
    std::shared_ptr<CameraState> m_state = std::make_shared<CameraState>();
};
}

std::unique_ptr<RecordingBorder> CreateRecordingBorder(const RecordingOptions& options, std::string& error) {
    auto border = std::make_unique<WinRecordingBorder>();
    if (!border->Show(options)) { error = "无法显示录制选区边框"; return {}; }
    return border;
}
std::unique_ptr<RecordingCamera> CreateRecordingCamera(const RecordingOptions& options, std::string& error) {
    try {
        auto camera = std::make_unique<WinRecordingCamera>();
        return camera->Show(options, error) ? std::move(camera) : nullptr;
    } catch (const std::exception& exception) { error = exception.what(); return {}; }
}
}
