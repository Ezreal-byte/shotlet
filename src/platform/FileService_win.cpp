#include "platform/LocalizedText.h"
#include "FileService.h"
#include "utils/Logger.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "FileService_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

namespace shotlet {

std::filesystem::path ChooseExportDirectory(Language language, TextId title) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IFileOpenDialog> dialog; std::filesystem::path path;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        FILEOPENDIALOGOPTIONS options{}; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(TrWide(language,title).c_str());
        dialog->SetOkButtonLabel(TrWide(language,TextId::HistoryConfirm).c_str());
        if (SUCCEEDED(dialog->Show(nullptr))) {
            ComPtr<IShellItem> item; PWSTR name = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                path = name; CoTaskMemFree(name);
            }
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return path;
}
bool RevealInFileManager(const std::filesystem::path& file) {
    if (file.empty()) return false;
    const std::wstring parameters = L"/select,\"" + file.wstring() + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}
bool OpenFileWithDefaultApplication(const std::filesystem::path& file) {
    if (file.empty()) return false;
    constexpr INT_PTR ShellExecuteSuccessThreshold = 32;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr,
                                                  SW_SHOWNORMAL)) > ShellExecuteSuccessThreshold;
}
CaptureSourceInfo GetForegroundCaptureSource() {
    CaptureSourceInfo result; const HWND window = GetForegroundWindow();
    if (!window) return result;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) return result;
    const auto utf8 = [](const wchar_t* text) {
        int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        std::string value(size > 0 ? size : 0, '\0');
        if (size > 0) { WideCharToMultiByte(CP_UTF8, 0, text, -1, value.data(), size, nullptr, nullptr); value.pop_back(); }
        return value;
    };
    wchar_t title[4096]{}; GetWindowTextW(window, title, 4096); result.window = utf8(title);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t path[4096]{}; DWORD size = 4096;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) result.application = utf8(std::filesystem::path(path).stem().c_str());
        CloseHandle(process);
    }
    return result;
}

void ActivateAppForSaveDialog() {}

// ─── Encode RGBA data to PNG using WIC ───
static bool EncodeRGBAToPNGFile(const unsigned char* rgbaData, int width,
                                int height, const wchar_t* filePath) {
    // WIC PNG 编码器原生支持 BGRA 格式，需将 RGBA→BGRA 以避免红蓝通道互换
    std::vector<uint8_t> bgraData(width * height * 4);
    for (int i = 0; i < width * height; i++) {
        bgraData[i * 4 + 0] = rgbaData[i * 4 + 2];  // B = R
        bgraData[i * 4 + 1] = rgbaData[i * 4 + 1];  // G = G
        bgraData[i * 4 + 2] = rgbaData[i * 4 + 0];  // R = B
        bgraData[i * 4 + 3] = rgbaData[i * 4 + 3];  // A = A
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Logger::Error("CoCreateInstance WICImagingFactory failed: {:x}",
                      static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(filePath, GENERIC_WRITE);
    if (FAILED(hr)) {
        Logger::Error("Failed to create stream for file");
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) return false;

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) return false;

    UINT stride = width * 4;
    UINT bufferSize = stride * height;
    hr = frame->WritePixels(height, stride, bufferSize,
                            bgraData.data());
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    if (FAILED(hr)) return false;

    return true;
}

// ─── SaveFileWithDialog ───
std::string SaveFileWithDialog(const unsigned char* rgbaData, int width,
                               int height, Language language) {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("SaveFileWithDialog: invalid parameters");
        return "";
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
        comInitialized = false;
    }

    std::string resultPath;

    ComPtr<IFileDialog> fileDialog;
    hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&fileDialog));
    if (FAILED(hr)) {
        Logger::Error("Failed to create IFileDialog");
        if (comInitialized) CoUninitialize();
        return "";
    }

    FILEOPENDIALOGOPTIONS opts;
    fileDialog->GetOptions(&opts);
    fileDialog->SetOptions(opts | FOS_OVERWRITEPROMPT);

    fileDialog->SetFileName(L"Screenshot.png");

    const auto pngType = TrWide(language, TextId::PngType);
    const auto allFilesType = TrWide(language, TextId::AllFilesType);
    COMDLG_FILTERSPEC filterSpec[] = {
        {pngType.c_str(), L"*.png"},
        {allFilesType.c_str(), L"*.*"},
    };
    fileDialog->SetFileTypes(2, filterSpec);
    fileDialog->SetFileTypeIndex(1);
    fileDialog->SetDefaultExtension(L"png");
    fileDialog->SetTitle(TrWide(language, TextId::SaveTitle).c_str());

    hr = fileDialog->Show(nullptr);
    if (FAILED(hr)) {
        if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            Logger::Error("IFileDialog::Show failed: {:x}",
                          static_cast<unsigned>(hr));
        }
        if (comInitialized) CoUninitialize();
        return "";
    }

    ComPtr<IShellItem> result;
    hr = fileDialog->GetResult(&result);
    if (FAILED(hr)) {
        Logger::Error("Failed to get dialog result");
        if (comInitialized) CoUninitialize();
        return "";
    }

    PWSTR filePath = nullptr;
    hr = result->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
    if (FAILED(hr) || !filePath) {
        Logger::Error("Failed to get file path from dialog");
        if (comInitialized) CoUninitialize();
        return "";
    }

    bool saved = EncodeRGBAToPNGFile(rgbaData, width, height, filePath);
    if (saved) {
        int len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0,
                                      nullptr, nullptr);
        if (len > 0) {
            resultPath.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, filePath, -1, resultPath.data(),
                                len, nullptr, nullptr);
        }
        Logger::Info("Screenshot saved to: {}", resultPath);
    } else {
        Logger::Error("Failed to encode/save PNG");
    }

    CoTaskMemFree(filePath);
    if (comInitialized) CoUninitialize();

    return resultPath;
}

} // namespace shotlet
