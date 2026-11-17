#pragma once
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace shotlet {
    // 显示器信息
    struct DisplayInfo {
        int id; // 显示器ID
        int displayX, displayY; // 左上角坐标（全局坐标系）
        int displayWidth, displayHeight; // 尺寸
        int refreshRate; // 刷新率
        float scaleFactor; // DPI 缩放 (1.0 = 100%)
        bool isPrimary; // 是否主显示器
        std::string name; // 显示器名称
    };

    // 捕获的图像数据
    struct CaptureImage {
        uint8_t *data = nullptr; // RGBA 数据 (需要释放)
        int width = 0;
        int height = 0;
        int captureX = 0;               // 全局坐标系中的左上角 X
        int captureY = 0;               // 全局坐标系中的左上角 Y
        int rowPitch = 0;        // 每行字节数 (通常 width * 4)
        int displayId = -1;      // 来源显示器
        float scaleFactor = 1.0f; // 像素与逻辑点的缩放比（Retina = 2.0）

        // GDI 句柄（仅 Windows CreateDIBSection 需要，存储 HBITMAP）
        // 当非空时，Free() 应调用 DeleteObject 而不是 delete[]
        void* gdiHandle = nullptr;

        CaptureImage() = default;
        ~CaptureImage();

        CaptureImage(const CaptureImage&) = delete;
        CaptureImage& operator=(const CaptureImage&) = delete;

        CaptureImage(CaptureImage&& other) noexcept;
        CaptureImage& operator=(CaptureImage&& other) noexcept;

        // 释放数据
        void Free();

        // 计算数据大小
        [[nodiscard]] size_t GetDataSize() const { return rowPitch * height; }
    };

    // 捕获选项
    struct CaptureOptions {
        int displayId = -1; // -1 = 所有显示器
        int captureX = 0, captureY = 0; // 捕获区域（相对指定显示器）
        int captureW = 0, captureH = 0; // 0 = 全屏
        bool cursor = false; // 是否包含鼠标光标
        bool hasDisplayInfo = false; // 是否携带调用方已知的显示器信息
        DisplayInfo displayInfo{}; // 避免平台捕获实现重复查询显示器
    };

    // 屏幕捕获接口
    class IScreenCapture {
    public:
        virtual ~IScreenCapture() = default;

        // 初始化
        virtual bool Initialize() = 0;

        // 获取所有显示器信息
        virtual std::vector<DisplayInfo> GetDisplays() = 0;

        // 捕获屏幕
        // 返回的 CaptureImage 包含需要释放的数据
        virtual CaptureImage Capture(const CaptureOptions &options = {}) = 0;

        // Capture() may touch platform state. Backends must opt in before callers
        // issue concurrent captures against the same instance.
        [[nodiscard]] virtual bool SupportsConcurrentCapture() const { return false; }

        // 捕获指定区域
        CaptureImage CaptureRegion(int captureX, int captureY, int w, int h, int displayId = -1) {
            CaptureOptions opts;
            opts.captureX = captureX;
            opts.captureY = captureY;
            opts.captureW = w;
            opts.captureH = h;
            opts.displayId = displayId;
            return Capture(opts);
        }

        // 捕获指定显示器
        CaptureImage CaptureDisplay(int displayId) {
            CaptureOptions opts;
            opts.displayId = displayId;
            return Capture(opts);
        }

        // 获取最后错误信息
        [[nodiscard]] virtual std::string GetLastErrorMessage() const = 0;
    };

    // 工厂函数
    std::unique_ptr<IScreenCapture> CreateScreenCapture();
} // namespace shotlet
