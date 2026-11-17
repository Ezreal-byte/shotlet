#include "shotlet/capture.h"
#include <stdio.h>

/* 无效请求必须在创建窗口和请求系统权限之前失败，测试不采集真实屏幕。 */
int main(void) {
    shotlet_capture_options options = {sizeof(options), SHOTLET_CAPTURE_ABI_VERSION, "invalid", "zh-CN", NULL, 1};
    shotlet_capture_result result = {0};
    result.struct_size = sizeof(result);
    if (shotlet_capture_abi_version() != SHOTLET_CAPTURE_ABI_VERSION) return 1;
    if (shotlet_capture_run(NULL, &result) != SHOTLET_CAPTURE_ERROR) return 2;
    if (shotlet_capture_run(&options, NULL) != SHOTLET_CAPTURE_ERROR) return 3;
    if (shotlet_capture_run(&options, &result) != SHOTLET_CAPTURE_ERROR) return 4;
    options.theme = "dark";
    options.abi_version++;
    if (shotlet_capture_run(&options, &result) != SHOTLET_CAPTURE_ERROR) return 5;
    options.abi_version = SHOTLET_CAPTURE_ABI_VERSION;
    options.language = "unsupported";
    if (shotlet_capture_run(&options, &result) != SHOTLET_CAPTURE_ERROR) return 6;
#if defined(__APPLE__)
    /* 宿主声明未授权时在创建窗口前返回，不查询系统权限。 */
    options.theme = "dark";
    options.language = "zh-CN";
    options.screen_recording = 0;
    if (shotlet_capture_run(&options, &result) != SHOTLET_CAPTURE_PERMISSION_REQUIRED) return 11;
#endif
    shotlet_capture_permissions permissions = {0};
    if (shotlet_capture_query_permissions(NULL) != SHOTLET_CAPTURE_ERROR) return 8;
    if (shotlet_capture_query_permissions(&permissions) != SHOTLET_CAPTURE_ERROR) return 9;
    if (shotlet_capture_request_permission(0) != SHOTLET_CAPTURE_ERROR) return 10;
    shotlet_capture_result_release(&result);
    shotlet_capture_result_release(&result);
    shotlet_capture_result_release(NULL);
    if (result.rgba != NULL || result.owner != NULL || result.data_size != 0) return 7;
    puts("截图 SDK 的 C ABI、参数校验与空结果释放契约通过");
    return 0;
}
