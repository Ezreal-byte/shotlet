#ifndef SHOTLET_CAPTURE_H
#define SHOTLET_CAPTURE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHOTLET_CAPTURE_ABI_VERSION 2u
#define SHOTLET_CAPTURE_MAX_PIXELS 67108864u
#define SHOTLET_CAPTURE_PIXEL_BYTES 4u

enum shotlet_capture_status {
    SHOTLET_CAPTURE_SUCCESS = 0,
    SHOTLET_CAPTURE_ERROR = 1,
    SHOTLET_CAPTURE_CANCELLED = 2,
    SHOTLET_CAPTURE_PERMISSION_REQUIRED = 3,
    /* 用户在工具栏选择了 OCR、贴图、保存或录制，动作已在 SDK 内部完成，没有图片返回。 */
    SHOTLET_CAPTURE_ACTION_DONE = 4
};

typedef struct shotlet_capture_options {
    uint32_t struct_size;
    uint32_t abi_version;
    /* 字符串为 UTF-8，仅在调用期间借用；主题和语言为空时跟随系统。 */
    const char* theme;       /* auto / light / dark */
    const char* language;    /* auto / zh-CN / en */
    const char* log_directory;
    /* 宿主检测的屏幕录制权限，1 为已授权。SDK 不再自行查询权限；macOS 上为 0 直接返回
     * PERMISSION_REQUIRED，其他平台忽略该字段。 */
    uint32_t screen_recording;
} shotlet_capture_options;

typedef struct shotlet_capture_result {
    uint32_t struct_size;
    uint32_t width_pixels;
    uint32_t height_pixels;
    uint32_t row_bytes;
    uint64_t data_size;
    /* 紧密排列的 RGBA 实际像素；所有权归 SDK，只能通过 release 释放。 */
    const unsigned char* rgba;
    void* owner;
} shotlet_capture_result;

typedef struct shotlet_capture_permissions {
    uint32_t struct_size;
    uint32_t settings_available;
    uint32_t screen_recording;
    uint32_t accessibility;
} shotlet_capture_permissions;

enum shotlet_capture_permission {
    SHOTLET_PERMISSION_SCREEN_RECORDING = 1,
    SHOTLET_PERMISSION_ACCESSIBILITY = 2
};

/* 只读查询实际调用进程的权限，不显示窗口、不发起采集或系统授权。
 * 请求仅由宿主在用户明确点击时调用；引导界面和设置页跳转由宿主负责。 */
int shotlet_capture_query_permissions(shotlet_capture_permissions* permissions);
int shotlet_capture_request_permission(uint32_t permission);

/* 必须从主线程调用，阻塞至本次交互结束；不可与其他 Shotlet Application 同时运行。
 * 调用前将结构体清零并设置 struct_size，options.abi_version 设置为当前 ABI 版本。
 * 完成截图时返回图片；取消返回 CANCELLED，不读写系统剪贴板或独立截图历史。
 * 选择 OCR、贴图、保存、录制时由 SDK 自己打开对应窗口，返回前会阻塞到这些窗口全部关闭，
 * 结束后返回 ACTION_DONE 且不含图片；需要贴图常驻的宿主应使用独立辅助程序。
 * 每次成功调用后都须释放结果，释放后可重用；函数不会向 C 调用方抛出异常。
 * 各宿主仍应优先使用独立辅助程序，避免竞争宿主的原生事件循环。 */
int shotlet_capture_run(const shotlet_capture_options* options, shotlet_capture_result* result);
void shotlet_capture_result_release(shotlet_capture_result* result);
uint32_t shotlet_capture_abi_version(void);

#ifdef __cplusplus
}
#endif
#endif
