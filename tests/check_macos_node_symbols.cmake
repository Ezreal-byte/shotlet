if(NOT DEFINED SHOTLET_NODE_PATH OR NOT EXISTS "${SHOTLET_NODE_PATH}")
    message(FATAL_ERROR "SHOTLET_NODE_PATH does not exist: ${SHOTLET_NODE_PATH}")
endif()

if(NOT DEFINED CMAKE_NM OR CMAKE_NM STREQUAL "")
    set(CMAKE_NM nm)
endif()

if(SHOTLET_CHECK_EXPORTS)
    execute_process(
        COMMAND "${CMAKE_NM}" -gU "${SHOTLET_NODE_PATH}"
        RESULT_VARIABLE exports_result
        OUTPUT_VARIABLE exports_output
        ERROR_VARIABLE exports_error
    )
    if(NOT exports_result EQUAL 0)
        message(FATAL_ERROR "导出符号检查失败：${exports_error}")
    endif()

    # 两个 Node-API 入口必须存在，且不能重新导出 SDL/ImGui 等内部符号。
    string(REGEX MATCHALL "[^\n]+" export_lines "${exports_output}")
    list(LENGTH export_lines export_count)
    if(NOT export_count EQUAL 2 OR
       NOT exports_output MATCHES "[ \t]_napi_register_module_v1([\r\n]|$)" OR
       NOT exports_output MATCHES "[ \t]_node_api_module_get_api_version_v1([\r\n]|$)")
        message(FATAL_ERROR "shotlet.node 必须只导出两个 Node-API 入口：\n${exports_output}")
    endif()
endif()

execute_process(
    COMMAND "${CMAKE_NM}" -u "${SHOTLET_NODE_PATH}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE nm_output
    ERROR_VARIABLE nm_error
)

if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "nm failed with ${nm_result}: ${nm_error}")
endif()

if(nm_output MATCHES "_OBJC_CLASS_\\$_SCScreenshotManager")
    message(FATAL_ERROR
        "shotlet.node strongly references SCScreenshotManager. This breaks dlopen on macOS versions whose ScreenCaptureKit framework does not export that class.")
endif()
