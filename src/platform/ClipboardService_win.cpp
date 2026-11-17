#include "ClipboardService.h"
#include "utils/Logger.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "ClipboardService_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace shotlet {

static bool EncodeRGBAToPNG(const unsigned char* rgbaData, int width, int height,
                            std::vector<uint8_t>& outPng) {
    // WIC PNG encoder natively supports BGRA format (GUID_WICPixelFormat32bppBGRA).
    // SetPixelFormat may change the requested format, so we must use BGRA and
    // convert RGBA→BGRA before WritePixels to avoid color inversion.
    // Pre-convert RGBA input data to BGRA for WIC
    std::vector<uint8_t> bgraData(width * height * 4);
    for (int i = 0; i < width * height; i++) {
        bgraData[i * 4 + 0] = rgbaData[i * 4 + 2];  // B = R
        bgraData[i * 4 + 1] = rgbaData[i * 4 + 1];  // G = G
        bgraData[i * 4 + 2] = rgbaData[i * 4 + 0];  // R = B
        bgraData[i * 4 + 3] = rgbaData[i * 4 + 3];  // A = A
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(nullptr);
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

    HGLOBAL hGlobal = nullptr;
    hr = GetHGlobalFromStream(stream.Get(), &hGlobal);
    if (FAILED(hr)) return false;

    SIZE_T size = GlobalSize(hGlobal);
    void* ptr = GlobalLock(hGlobal);
    if (ptr) {
        outPng.resize(size);
        memcpy(outPng.data(), ptr, size);
        GlobalUnlock(hGlobal);
    }

    return !outPng.empty();
}

bool CopyImageToClipboard(const unsigned char* rgbaData, int width, int height) {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("CopyImageToClipboard: invalid parameters");
        return false;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Encode PNG
    std::vector<uint8_t> pngData;
    if (!EncodeRGBAToPNG(rgbaData, width, height, pngData)) {
        Logger::Error("Failed to encode PNG");
        return false;
    }
    Logger::Info("PNG size: {} bytes", pngData.size());

    // Prepare CF_DIB (BITMAPINFOHEADER + BGR pixels)
    int dibRowPitch = ((width * 3 + 3) / 4) * 4;
    int dibSize = sizeof(BITMAPINFOHEADER) + dibRowPitch * height;
    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (hDib) {
        auto* ptr = (uint8_t*)GlobalLock(hDib);
        auto* bih = (BITMAPINFOHEADER*)ptr;
        ZeroMemory(bih, sizeof(BITMAPINFOHEADER));
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = width;
        bih->biHeight = height;  // positive = bottom-up
        bih->biPlanes = 1;
        bih->biBitCount = 24;
        bih->biCompression = BI_RGB;

        uint8_t* dibPixels = ptr + sizeof(BITMAPINFOHEADER);
        for (int y = 0; y < height; y++) {
            int srcY = height - 1 - y;  // flip for bottom-up
            for (int x = 0; x < width; x++) {
                int srcIdx = (srcY * width + x) * 4;
                int dstIdx = y * dibRowPitch + x * 3;
                dibPixels[dstIdx + 0] = rgbaData[srcIdx + 2]; // B
                dibPixels[dstIdx + 1] = rgbaData[srcIdx + 1]; // G
                dibPixels[dstIdx + 2] = rgbaData[srcIdx + 0]; // R
            }
        }
        GlobalUnlock(hDib);
    }

    // Prepare PNG clipboard data
    UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, pngData.size());
    if (hPng) {
        void* ptr = GlobalLock(hPng);
        memcpy(ptr, pngData.data(), pngData.size());
        GlobalUnlock(hPng);
    }

    // Write to clipboard
    if (!OpenClipboard(nullptr)) {
        Logger::Error("Failed to open clipboard");
        if (hDib) GlobalFree(hDib);
        if (hPng) GlobalFree(hPng);
        return false;
    }

    EmptyClipboard();

    bool success = false;
    if (hDib && SetClipboardData(CF_DIB, hDib)) {
        success = true;
    } else if (hDib) {
        GlobalFree(hDib);
    }

    if (hPng && pngFormat && SetClipboardData(pngFormat, hPng)) {
        success = true;
    } else if (hPng) {
        GlobalFree(hPng);
    }

    CloseClipboard();

    if (success) {
        Logger::Info("Image copied to clipboard (CF_DIB + PNG)");
    }
    return success;
}

} // namespace shotlet
