#pragma once

struct SDL_GPUDevice;

namespace shotlet {

class IScreenCapture;

// 初始化应用所需的平台环境。
void InitializeApplicationPlatform();

// 设置各操作系统所需的 SDL 提示。
void ApplyApplicationPlatformSDLHints();

// 检查截图权限并预热平台后端；保留平台原有的授权等待策略。
void PrepareApplicationScreenCapture(IScreenCapture* capture);

// 当前仅 macOS 需要在 daemon 空闲到期后回收截图窗口。
bool SupportsIdleOverlayReclamation();

// 按平台策略控制活动截图的帧间隔。
void DelayAfterCaptureFrame();

// 按平台运行策略创建或复用 SDL GPU 设备。
SDL_GPUDevice* CreateApplicationGpuDevice(bool embeddedMode);

// 释放设备；平台要求共享时仅解除当前应用的引用。
void DestroyApplicationGpuDevice(SDL_GPUDevice*& gpuDevice, bool embeddedMode);

// 截图窗口销毁或复用前恢复原生光标。
void ResetPlatformCursor();

// 在需要的平台等待 GPU 完成对截图资源的访问。
void WaitForOverlayGpuIdle(SDL_GPUDevice* gpuDevice);

} // namespace shotlet
