#include "RecordingPlaybackWindow.h"
#include "FileService.h"
#include "LocalizedText.h"
#include "Appearance.h"
#include "utils/Win32Drawing.h"
#include "utils/Win32Utils.h"
#include "utils/WindowUtils.h"
#include "utils/CoordinateUtils.h"
#include "utils/Logger.h"
#include <commctrl.h>
// PlatformWin32 使用精简 Windows 头文件，GDI+ 所需的 COM 接口必须显式引入。
#include <objidl.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace shotlet {
namespace {
using Microsoft::WRL::ComPtr;
using Win32Utils::CheckHResult;
namespace Layout = RecordingPlaybackLayout;
constexpr wchar_t WindowClass[] = L"ShotletRecordingPlayback", VideoClass[] = L"ShotletRecordingPlaybackVideo";
constexpr int RowHeightLogical = 28, PlayWidthLogical = 72;
constexpr int GifControlsHeightLogical = RowHeightLogical * 2 + RecordingPlaybackLayout::PaddingLogical * 3;
constexpr int VideoControlsHeightLogical = RowHeightLogical * 3 + RecordingPlaybackLayout::PaddingLogical * 4;
constexpr int TimeWidthLogical = 110, VolumeWidthLogical = 80, FullScreenWidthLogical = 72;
constexpr int FontSizePoints = 10, PointsPerInch = 72, DpiAtDefaultScale = 96;
constexpr int SeekUnits = 1000, VolumeUnits = 100, SecondsPerMinute = 60;
constexpr int DefaultGifDelayMs = 100, MinimumGifDelayMs = 10, GifDelayUnitMs = 10;
constexpr auto UiInterval = std::chrono::milliseconds(100);
constexpr UINT_PTR KeyboardSubclassId = 1;
constexpr UINT PlaybackAttributeCount = 2;
constexpr BYTE OpaqueAlpha = 255;
constexpr float ButtonRadiusLogical = 6, ButtonStrokeLogical = 1, ButtonInsetLogical = 1;
constexpr float SliderTrackHeightLogical = 4;
constexpr int SliderThumbHitHeightLogical = 20;
constexpr int DividerHeightLogical = 1;
enum ControlId { Play = 100, Seek, Volume, FullScreen, Reveal, Time, Detail };

struct MediaEvents {
    std::atomic<HRESULT> error{S_OK};
    std::atomic_bool videoChanged{false};
};
// 媒体线程仅发布状态，不持有窗口或 C++ 播放器指针，关闭后到达的回调也安全。
class PlaybackNotify final : public IMFMediaEngineNotify {
public:
    explicit PlaybackNotify(std::shared_ptr<MediaEvents> events) : m_events(std::move(events)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != IID_IUnknown && id != __uuidof(IMFMediaEngineNotify)) return E_NOINTERFACE;
        *result = static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --m_references;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR parameter, DWORD failure) override {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) SetEvent(reinterpret_cast<HANDLE>(parameter));
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR || event == MF_MEDIA_ENGINE_EVENT_STREAMRENDERINGERROR)
            m_events->error.store(failure ? HRESULT(failure) : E_FAIL);
        if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA || event == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE)
            m_events->videoChanged.store(true);
        return S_OK;
    }
private:
    std::atomic<ULONG> m_references{1};
    std::shared_ptr<MediaEvents> m_events;
};

class WinRecordingPlaybackWindow final : public RecordingPlaybackWindow {
public:
    ~WinRecordingPlaybackWindow() override {
        StopMedia();
        if (m_window) DestroyWindow(m_window);
        if (m_font) DeleteObject(m_font);
        // 按钮和滑块也使用 GDI+，其生命周期必须覆盖全部子窗口的销毁绘制。
        if (m_gdiToken) Gdiplus::GdiplusShutdown(m_gdiToken);
    }
    bool Show(const std::filesystem::path& path, const RecordingOptions& options,
              Language language, Theme theme, std::string& error) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = Tr(language, TextId::RecordingPlaybackMissing); return false;
        }
        m_path = path; m_language = language;
        Gdiplus::GdiplusStartupInput startup;
        if (Gdiplus::GdiplusStartup(&m_gdiToken, &startup, nullptr) != Gdiplus::Ok) { error = "初始化播放界面失败"; return false; }
        if (!CreateWindowAndControls(options, error)) return false;
        ApplyTheme(theme);
        const bool opened = options.format == RecordingFormat::Gif ? OpenGif(error) : OpenVideo(error);
        if (!opened) return false;
        LayoutControls();
        Update(theme);
        ShowWindow(m_window, SW_SHOWNORMAL); SetForegroundWindow(m_window);
        return true;
    }
    bool IsOpen() const override { return m_window && IsWindow(m_window); }
    void Update(Theme theme) override {
        if (!IsOpen()) return;
        if (!m_theme || *m_theme != theme) ApplyTheme(theme);
        if (m_events) {
            if (m_events->videoChanged.exchange(false)) InvalidateRect(m_video, nullptr, FALSE);
            const auto failure = m_events->error.exchange(S_OK);
            if (FAILED(failure)) {
                std::string error; CheckHResult(failure, Tr(m_language, TextId::RecordingPlaybackFailed), error);
                m_failed = true;
                SetWindowTextW(m_detail, Utf8ToWide(error).c_str());
                Logger::Warning("event=recording.playback_failed error={}", error);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (m_image && m_playing && m_frameCount > 1 && now >= m_nextFrame) {
            m_frame = (m_frame + 1) % m_frameCount;
            m_image->SelectActiveFrame(&Gdiplus::FrameDimensionTime, m_frame);
            m_nextFrame = now + std::chrono::milliseconds(m_delaysMs[m_frame]);
            InvalidateRect(m_video, nullptr, FALSE);
        }
        if (!m_engine || now - m_lastUi < UiInterval) return;
        m_lastUi = now;
        const double duration = m_engine->GetDuration(), current = m_engine->GetCurrentTime();
        if (std::isfinite(duration) && duration > 0 && std::isfinite(current)) {
            if (GetCapture() != m_seek) SendMessageW(m_seek, TBM_SETPOS, TRUE, LPARAM(std::clamp(current / duration, 0.0, 1.0) * SeekUnits));
            const int elapsedSeconds = int(current), totalSeconds = int(duration);
            const auto label = std::format(L"{:02}:{:02} / {:02}:{:02}", elapsedSeconds / SecondsPerMinute,
                elapsedSeconds % SecondsPerMinute, totalSeconds / SecondsPerMinute, totalSeconds % SecondsPerMinute);
            Win32Utils::SetWindowTextIfChanged(m_time, label);
        }
        UpdatePlayLabel();
    }
private:
    int Scaled(int logical) const { return int(std::lround(logical * m_scale)); }
    const ThemePalette& Palette() const { return GetThemePalette(m_theme.value_or(Theme::Dark)); }
    void ApplyTheme(Theme theme) {
        m_theme = theme;
        ApplyNativeWindowTheme(m_window, theme);
        // 标准子控件默认不会跟随 DWM 标题栏主题，必须同时重绘整个客户区。
        RedrawWindow(m_window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    void PaintBackground(HDC dc) const {
        RECT bounds{}; GetClientRect(m_window, &bounds);
        Win32Drawing::FillBackground(dc, bounds, Palette().panelBackground);
        const RECT separator{bounds.left, m_videoHeightPixels, bounds.right, m_videoHeightPixels + Scaled(DividerHeightLogical)};
        Win32Drawing::FillBackground(dc, separator, Palette().border);
    }
    void PaintButton(const DRAWITEMSTRUCT& item) const {
        const auto& p = Palette();
        const bool primary = item.hwndItem == m_play;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool hovered = m_hoveredControl == item.hwndItem;
        const bool focused = (item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT);
        const auto background = disabled ? p.editorBackground : primary ?
            (pressed ? p.confirmButtonActive : hovered ? p.confirmButtonHovered : p.confirmButton) :
            (pressed ? p.selectedBackground : hovered ? p.hoverBackground : p.editorBackground);
        const auto border = focused ? (primary ? Colors::White : p.confirmButton) : primary ? background : p.border;
        Win32Drawing::FillBackground(item.hDC, item.rcItem, p.panelBackground);
        {
            Gdiplus::Graphics graphics(item.hDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float inset = float(ButtonInsetLogical * m_scale);
            const Gdiplus::RectF bounds(item.rcItem.left + inset, item.rcItem.top + inset,
                item.rcItem.right - item.rcItem.left - inset * 2, item.rcItem.bottom - item.rcItem.top - inset * 2);
            Win32Drawing::RoundedRect(graphics, bounds, float(ButtonRadiusLogical * m_scale), background, border,
                float(ButtonStrokeLogical * m_scale));
        }
        Win32Drawing::CenteredText(item.hDC, item.rcItem, Win32Utils::WindowText(item.hwndItem), m_font,
            disabled ? p.disabledIcon : primary ? Colors::White : p.text);
    }
    void PaintSlider(HWND slider, HDC dc) const {
        RECT bounds{}, channel{}, thumb{};
        GetClientRect(slider, &bounds);
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const auto& p = Palette();
        Win32Drawing::FillBackground(dc, bounds, p.panelBackground);
        if (channel.right <= channel.left) return;
        // 采用原生滑块的实际位置，绘制范围和命中范围共用同一坐标，分数 DPI 下也不会跳动。
        const float centerX = std::clamp((thumb.left + thumb.right) / 2.0f, float(channel.left), float(channel.right));
        const float centerY = (bounds.top + bounds.bottom) / 2.0f;
        const float trackHeight = float(SliderTrackHeightLogical * m_scale);
        const bool active = m_hoveredControl == slider || GetCapture() == slider || GetFocus() == slider;
        const bool enabled = IsWindowEnabled(slider);
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (GetFocus() == slider) {
            const float inset = float(ButtonInsetLogical * m_scale);
            Win32Drawing::RoundedRect(graphics, {bounds.left + inset, bounds.top + inset,
                bounds.right - bounds.left - inset * 2, bounds.bottom - bounds.top - inset * 2},
                float(ButtonRadiusLogical * m_scale), Colors::Transparent, p.confirmButton, float(ButtonStrokeLogical * m_scale));
        }
        const Gdiplus::RectF track(float(channel.left), centerY - trackHeight / 2, float(channel.right - channel.left), trackHeight);
        Win32Drawing::RoundedRect(graphics, track, trackHeight / 2, p.border);
        const Gdiplus::RectF progress(track.X, track.Y, centerX - track.X, track.Height);
        Win32Drawing::RoundedRect(graphics, progress, trackHeight / 2, enabled ? p.confirmButton : p.disabledIcon);
        const float radius = float((active ? Layout::SliderActiveThumbRadiusLogical : Layout::SliderThumbRadiusLogical) * m_scale);
        Gdiplus::SolidBrush handle(Win32Drawing::Color(enabled ? p.confirmButton : p.disabledIcon));
        graphics.FillEllipse(&handle, centerX - radius, centerY - radius, radius * 2, radius * 2);
    }
    void UpdatePlayLabel() {
        const bool playing = m_engine ? !m_engine->IsPaused() : m_playing;
        Win32Utils::SetWindowTextIfChanged(m_play, TrWide(m_language, playing ? TextId::RecordingPause : TextId::RecordingPlay));
    }
    static LRESULT CALLBACK ControlKeys(HWND window, UINT message, WPARAM key, LPARAM value, UINT_PTR, DWORD_PTR owner) {
        auto* self = reinterpret_cast<WinRecordingPlaybackWindow*>(GetWindowLongPtrW(reinterpret_cast<HWND>(owner), GWLP_USERDATA));
        if (self && message == WM_PAINT && (window == self->m_seek || window == self->m_volume)) {
            // 原生滑块移动时只使旧、新滑块区域失效，快速前进时两者之间可能不相连。
            // 自绘填充色覆盖整段轨道，必须在原生 BeginPaint 建立裁剪区前扩大刷新范围。
            InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_KEYDOWN && (key == VK_ESCAPE || key == VK_F11))
            return SendMessageW(reinterpret_cast<HWND>(owner), message, key, value);
        if (self && message == WM_MOUSEMOVE && self->m_hoveredControl != window) {
            if (self->m_hoveredControl) InvalidateRect(self->m_hoveredControl, nullptr, FALSE);
            self->m_hoveredControl = window;
            InvalidateRect(window, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0}; TrackMouseEvent(&tracking);
        }
        if (self && (message == WM_MOUSELEAVE || message == WM_NCDESTROY) && self->m_hoveredControl == window) {
            self->m_hoveredControl = nullptr; InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ControlKeys, KeyboardSubclassId);
        const auto result = DefSubclassProc(window, message, key, value);
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
            message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) InvalidateRect(window, nullptr, FALSE);
        return result;
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM key, LPARAM value) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams));
        auto* self = reinterpret_cast<WinRecordingPlaybackWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return DefWindowProcW(window, message, key, value);
        switch (message) {
            case WM_ERASEBKGND: {
                self->PaintBackground(reinterpret_cast<HDC>(key)); return 1;
            }
            case WM_PAINT: {
                PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
                self->PaintBackground(dc); EndPaint(window, &paint); return 0;
            }
            case WM_PRINTCLIENT: self->PaintBackground(reinterpret_cast<HDC>(key)); return 0;
            case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
                const auto& p = self->Palette();
                const auto dc = reinterpret_cast<HDC>(key);
                const bool detail = reinterpret_cast<HWND>(value) == self->m_detail;
                SetTextColor(dc, Colors::ToNativeRgb(detail && self->m_failed ? p.danger : p.secondaryText));
                SetBkColor(dc, Colors::ToNativeRgb(p.panelBackground));
                SetBkMode(dc, OPAQUE); SetDCBrushColor(dc, Colors::ToNativeRgb(p.panelBackground));
                return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
            }
            case WM_DRAWITEM: {
                const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(value);
                if (item && item->CtlType == ODT_BUTTON) { self->PaintButton(*item); return TRUE; }
                break;
            }
            case WM_NOTIFY: {
                const auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(value);
                if (draw && draw->hdr.code == NM_CUSTOMDRAW &&
                    (draw->hdr.hwndFrom == self->m_seek || draw->hdr.hwndFrom == self->m_volume) && draw->dwDrawStage == CDDS_PREPAINT) {
                    self->PaintSlider(draw->hdr.hwndFrom, draw->hdc); return CDRF_SKIPDEFAULT;
                }
                break;
            }
            case WM_SIZE: self->LayoutControls(); return 0;
            case WM_DPICHANGED: {
                self->m_scale = double(HIWORD(key)) / DpiAtDefaultScale;
                const auto* bounds = reinterpret_cast<RECT*>(value);
                SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                    bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->UpdateFont(); self->LayoutControls(); return 0;
            }
            case WM_COMMAND:
                if (LOWORD(key) == Play) self->TogglePlayback();
                else if (LOWORD(key) == Reveal) RevealInFileManager(self->m_path);
                else if (LOWORD(key) == FullScreen) self->ToggleFullScreen();
                return 0;
            case WM_HSCROLL: self->OnSlider(reinterpret_cast<HWND>(value)); return 0;
            case WM_KEYDOWN:
                if (key == VK_SPACE) { self->TogglePlayback(); return 0; }
                if (key == VK_F11 || (key == VK_ESCAPE && self->m_fullScreen)) { self->ToggleFullScreen(); return 0; }
                if (key == VK_ESCAPE) { DestroyWindow(window); return 0; }
                break;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(value);
                limits->ptMinTrackSize = {self->Scaled(Layout::MinimumWidthLogical), self->Scaled(Layout::MinimumHeightLogical)};
                return 0;
            }
            case WM_DESTROY: self->StopMedia(); self->m_window = nullptr; return 0;
            case WM_CLOSE: self->StopMedia(); DestroyWindow(window); return 0;
        }
        return DefWindowProcW(window, message, key, value);
    }
    static LRESULT CALLBACK VideoProc(HWND window, UINT message, WPARAM key, LPARAM value) {
        if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams));
        auto* self = reinterpret_cast<WinRecordingPlaybackWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT && self) { self->PaintVideo(window); return 0; }
        return DefWindowProcW(window, message, key, value);
    }
    HWND AddControl(const wchar_t* type, const wchar_t* text, DWORD style, ControlId id) {
        auto window = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        if (window) {
            m_controls.push_back(window);
            SetWindowSubclass(window, ControlKeys, KeyboardSubclassId, reinterpret_cast<DWORD_PTR>(m_window));
        }
        return window;
    }
    bool CreateWindowAndControls(const RecordingOptions& options, std::string& error) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
        if (!InitCommonControlsEx(&controls) || !Win32Utils::RegisterWindowClass(WindowClass, WindowProc) ||
            !Win32Utils::RegisterWindowClass(VideoClass, VideoProc)) { error = "无法初始化播放窗口"; return false; }
        const SDL_Rect region{int(options.region.x), int(options.region.y), int(options.region.width), int(options.region.height)};
        m_scale = WindowUtils::GetContentScaleForRect(region);
        const POINT center{region.x + region.w / 2, region.y + region.h / 2};
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor)) { error = "无法读取显示器工作区"; return false; }
        const auto& work = monitor.rcWork;
        const int width = std::min(Scaled(Layout::PreferredWidthLogical), int((work.right - work.left) * Layout::WorkAreaFraction));
        const int height = std::min(Scaled(Layout::PreferredHeightLogical), int((work.bottom - work.top) * Layout::WorkAreaFraction));
        const auto title = TrWide(m_language, TextId::RecordingPlaybackTitle) + L" · " + m_path.filename().wstring();
        m_window = CreateWindowExW(0, WindowClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
            width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_window) { error = "无法创建播放窗口"; return false; }
        m_video = CreateWindowExW(0, VideoClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
            m_window, nullptr, GetModuleHandleW(nullptr), this);
        m_play = AddControl(L"BUTTON", TrWide(m_language, TextId::RecordingPause).c_str(), BS_OWNERDRAW, Play);
        m_seek = AddControl(TRACKBAR_CLASSW, TrWide(m_language, TextId::RecordingProgress).c_str(), TBS_NOTICKS | TBS_BOTH | TBS_FIXEDLENGTH, Seek);
        m_volume = AddControl(TRACKBAR_CLASSW, TrWide(m_language, TextId::RecordingVolume).c_str(), TBS_NOTICKS | TBS_BOTH | TBS_FIXEDLENGTH, Volume);
        m_fullScreenButton = AddControl(L"BUTTON", TrWide(m_language, TextId::RecordingFullScreen).c_str(), BS_OWNERDRAW, FullScreen);
        m_reveal = AddControl(L"BUTTON", TrWide(m_language, TextId::HistoryReveal).c_str(), BS_OWNERDRAW, Reveal);
        m_time = AddControl(L"STATIC", L"00:00 / 00:00", SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, Time);
        m_detail = AddControl(L"STATIC", m_path.filename().c_str(), SS_PATHELLIPSIS | SS_CENTERIMAGE | SS_NOPREFIX, Detail);
        if (!m_video || m_controls.size() != ExpectedControlCount) { error = "无法创建播放控件"; return false; }
        SendMessageW(m_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, SeekUnits));
        SendMessageW(m_volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, VolumeUnits));
        SendMessageW(m_volume, TBM_SETPOS, TRUE, VolumeUnits);
        UpdateFont();
        return true;
    }
    void UpdateFont() {
        HFONT font = CreateFontW(-MulDiv(FontSizePoints, int(std::lround(m_scale * DpiAtDefaultScale)), PointsPerInch),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) return;
        // owner-draw 可能在 WM_SETFONT 内同步绘制，先发布新字体，再等所有子控件切换后释放旧字体。
        const auto previous = m_font;
        m_font = font;
        for (const auto control : m_controls) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (previous) DeleteObject(previous);
    }
    bool OpenVideo(std::string& error) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInitialized = SUCCEEDED(initialized);
        if (initialized != RPC_E_CHANGED_MODE && !CheckHResult(initialized, "初始化播放 COM 失败", error)) return false;
        if (!CheckHResult(MFStartup(MF_VERSION), "初始化系统播放器失败", error)) return false;
        m_mediaInitialized = true;
        m_events = std::make_shared<MediaEvents>();
        ComPtr<IMFMediaEngineNotify> notify; notify.Attach(new PlaybackNotify(m_events));
        ComPtr<IMFAttributes> attributes;
        if (!CheckHResult(MFCreateAttributes(&attributes, PlaybackAttributeCount), "创建播放参数失败", error)) return false;
        attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
        attributes->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND, reinterpret_cast<UINT64>(m_video));
        ComPtr<IMFMediaEngineClassFactory> factory;
        if (!CheckHResult(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)), "创建系统播放器失败", error)) return false;
        if (!CheckHResult(factory->CreateInstance(0, attributes.Get(), &m_engine), "配置系统播放器失败", error)) return false;
        if (!CheckHResult(m_engine.As(&m_engineEx), "获取视频显示接口失败", error)) return false;
        m_engine->SetAutoPlay(TRUE);
        BSTR source = SysAllocString(m_path.c_str());
        if (!source) { error = "无法分配播放路径"; return false; }
        const auto result = m_engine->SetSource(source); SysFreeString(source);
        return CheckHResult(result, "打开录屏文件失败", error) && CheckHResult(m_engine->Load(), "加载录屏失败", error);
    }
    bool OpenGif(std::string& error) {
        m_image = std::make_unique<Gdiplus::Image>(m_path.c_str());
        if (m_image->GetLastStatus() != Gdiplus::Ok || !m_image->GetWidth() || !m_image->GetHeight()) { error = "无法读取 GIF"; return false; }
        m_frameCount = std::max<UINT>(1, m_image->GetFrameCount(&Gdiplus::FrameDimensionTime));
        m_delaysMs.assign(m_frameCount, DefaultGifDelayMs);
        const UINT bytes = m_image->GetPropertyItemSize(PropertyTagFrameDelay);
        std::vector<unsigned char> storage(bytes);
        if (bytes >= sizeof(Gdiplus::PropertyItem)) {
            auto* property = reinterpret_cast<Gdiplus::PropertyItem*>(storage.data());
            if (m_image->GetPropertyItem(PropertyTagFrameDelay, bytes, property) == Gdiplus::Ok &&
                property->type == PropertyTagTypeLong && property->length / sizeof(UINT) >= m_frameCount) {
                const auto* delays = static_cast<const UINT*>(property->value);
                for (UINT i = 0; i < m_frameCount; ++i) m_delaysMs[i] = std::max(MinimumGifDelayMs, int(delays[i]) * GifDelayUnitMs);
            }
        }
        m_nextFrame = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_delaysMs[0]);
        ShowWindow(m_seek, SW_HIDE); ShowWindow(m_volume, SW_HIDE); ShowWindow(m_time, SW_HIDE);
        return true;
    }
    void StopMedia() {
        // HWND 仍有效时先停止系统渲染；销毁窗口不会向宿主发送 WM_QUIT。
        if (m_engine) { m_engine->Pause(); m_engine->Shutdown(); }
        m_engineEx.Reset(); m_engine.Reset(); m_events.reset();
        m_image.reset();
        if (m_mediaInitialized) { MFShutdown(); m_mediaInitialized = false; }
        if (m_comInitialized) { CoUninitialize(); m_comInitialized = false; }
    }
    void LayoutControls() {
        if (!m_window || !m_video || !m_detail) return;
        RECT client{}; GetClientRect(m_window, &client);
        // RECT 使用 LONG，布局与 MoveWindow 使用 int；统一类型后再参与模板运算。
        const int clientWidthPixels = static_cast<int>(client.right - client.left);
        const int clientHeightPixels = static_cast<int>(client.bottom - client.top);
        const int padding = Scaled(Layout::PaddingLogical), row = Scaled(RowHeightLogical);
        const int controlsHeight = m_image ? GifControlsHeightLogical : VideoControlsHeightLogical;
        const int videoHeight = std::max(0, clientHeightPixels - Scaled(controlsHeight));
        m_videoHeightPixels = videoHeight;
        MoveWindow(m_video, 0, 0, clientWidthPixels, videoHeight, TRUE);
        const int seekTop = videoHeight + padding;
        MoveWindow(m_seek, padding, seekTop, std::max(0, clientWidthPixels - padding * 2), row, TRUE);
        const int top = m_image ? seekTop : seekTop + row + padding;
        int left = padding;
        MoveWindow(m_play, left, top, Scaled(PlayWidthLogical), row, TRUE); left += Scaled(PlayWidthLogical) + padding;
        MoveWindow(m_time, left, top, Scaled(TimeWidthLogical), row, TRUE); left += Scaled(TimeWidthLogical) + padding;
        const int fullWidth = Scaled(FullScreenWidthLogical), volumeWidth = Scaled(VolumeWidthLogical);
        const int fullLeft = clientWidthPixels - padding - fullWidth;
        const int volumeLeft = fullLeft - padding - volumeWidth;
        MoveWindow(m_volume, volumeLeft, top, volumeWidth, row, TRUE);
        for (const auto slider : {m_seek, m_volume}) SendMessageW(slider, TBM_SETTHUMBLENGTH, Scaled(SliderThumbHitHeightLogical), 0);
        MoveWindow(m_fullScreenButton, fullLeft, top, fullWidth, row, TRUE);
        const int bottom = top + row + padding, revealWidth = Scaled(Layout::ButtonWidthLogical);
        MoveWindow(m_reveal, clientWidthPixels - padding - revealWidth, bottom, revealWidth, row, TRUE);
        MoveWindow(m_detail, padding, bottom, std::max(0, clientWidthPixels - padding * 3 - revealWidth), row, TRUE);
        InvalidateRect(m_window, nullptr, FALSE);
        InvalidateRect(m_video, nullptr, FALSE);
    }
    RECT FittedVideoRect(RECT bounds, double width, double height) const {
        if (width <= 0 || height <= 0) return bounds;
        const auto fitted = AspectFitRect({double(bounds.left), double(bounds.top),
            double(bounds.right - bounds.left), double(bounds.bottom - bounds.top)}, width, height);
        return {LONG(std::lround(fitted.x)), LONG(std::lround(fitted.y)),
            LONG(std::lround(fitted.x + fitted.width)), LONG(std::lround(fitted.y + fitted.height))};
    }
    void PaintVideo(HWND window) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds); FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (m_image) {
            const RECT fitted = FittedVideoRect(bounds, m_image->GetWidth(), m_image->GetHeight());
            Gdiplus::Graphics graphics(dc);
            // 明确使用整数像素重载，避免 LONG 同时匹配 INT 与 REAL 产生歧义。
            graphics.DrawImage(m_image.get(), static_cast<INT>(fitted.left), static_cast<INT>(fitted.top),
                static_cast<INT>(fitted.right - fitted.left), static_cast<INT>(fitted.bottom - fitted.top));
        }
        EndPaint(window, &paint);
        if (m_engineEx) {
            DWORD width = 0, height = 0; m_engine->GetNativeVideoSize(&width, &height);
            const RECT fitted = FittedVideoRect(bounds, width, height);
            MFARGB black{}; black.rgbAlpha = OpaqueAlpha;
            m_engineEx->UpdateVideoStream(nullptr, &fitted, &black);
        }
    }
    void TogglePlayback() {
        if (m_engine) {
            if (!m_engine->IsPaused()) m_engine->Pause();
            else { if (m_engine->IsEnded()) m_engine->SetCurrentTime(0); m_engine->Play(); }
        } else if (m_image) {
            m_playing = !m_playing;
            m_nextFrame = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_delaysMs[m_frame]);
        }
        UpdatePlayLabel();
    }
    void OnSlider(HWND slider) {
        if (!m_engine) return;
        const auto value = SendMessageW(slider, TBM_GETPOS, 0, 0);
        if (slider == m_volume) m_engine->SetVolume(double(value) / VolumeUnits);
        else if (slider == m_seek) {
            const double duration = m_engine->GetDuration();
            if (std::isfinite(duration) && duration > 0) m_engine->SetCurrentTime(duration * double(value) / SeekUnits);
        }
    }
    void ToggleFullScreen() {
        if (!m_fullScreen) {
            GetWindowPlacement(m_window, &m_placement);
            m_windowStyle = GetWindowLongPtrW(m_window, GWL_STYLE);
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(m_window, GWL_STYLE, m_windowStyle & ~WS_OVERLAPPEDWINDOW);
            const auto& bounds = monitor.rcMonitor;
            SetWindowPos(m_window, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(m_window, GWL_STYLE, m_windowStyle);
            SetWindowPlacement(m_window, &m_placement);
            SetWindowPos(m_window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        m_fullScreen = !m_fullScreen;
    }
    static constexpr size_t ExpectedControlCount = 7;
    HWND m_window = nullptr, m_video = nullptr, m_play = nullptr, m_seek = nullptr, m_volume = nullptr;
    HWND m_fullScreenButton = nullptr, m_reveal = nullptr, m_time = nullptr, m_detail = nullptr;
    HFONT m_font = nullptr;
    HWND m_hoveredControl = nullptr;
    int m_videoHeightPixels = 0;
    std::vector<HWND> m_controls;
    double m_scale = 1;
    std::filesystem::path m_path;
    Language m_language = Language::English;
    std::optional<Theme> m_theme;
    ComPtr<IMFMediaEngine> m_engine;
    ComPtr<IMFMediaEngineEx> m_engineEx;
    std::shared_ptr<MediaEvents> m_events;
    bool m_comInitialized = false, m_mediaInitialized = false, m_playing = true, m_fullScreen = false, m_failed = false;
    ULONG_PTR m_gdiToken = 0;
    std::unique_ptr<Gdiplus::Image> m_image;
    UINT m_frame = 0, m_frameCount = 0;
    std::vector<int> m_delaysMs;
    std::chrono::steady_clock::time_point m_nextFrame{}, m_lastUi{};
    WINDOWPLACEMENT m_placement{sizeof(m_placement)};
    LONG_PTR m_windowStyle = 0;
};
}

std::unique_ptr<RecordingPlaybackWindow> CreateRecordingPlaybackWindow(const std::filesystem::path& path,
    const RecordingOptions& options, Language language, Theme theme, std::string& error) {
    error.clear();
    auto window = std::make_unique<WinRecordingPlaybackWindow>();
    return window->Show(path, options, language, theme, error) ? std::move(window) : nullptr;
}
}
