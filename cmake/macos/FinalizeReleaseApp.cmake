# Debug 和 RelWithDebInfo 保留符号，不执行发布处理。
if(NOT SHOTLET_CONFIG MATCHES "^(Release|MinSizeRel)$")
    return()
endif()

# 此脚本由链接后的构建步骤调用，保留本次链接得到的未剥离文件。
file(COPY "${SHOTLET_APP_BINARY}" DESTINATION "${SHOTLET_UNSTRIPPED_DIR}")
# 旧构建曾将资源放在 MacOS 目录，签名工具会将其误认作代码。
# 新资源已复制到 Contents/Resources，旧目录移至备份处以支持增量构建。
if(EXISTS "${SHOTLET_APP_BUNDLE}/Contents/MacOS/resources")
    file(RENAME "${SHOTLET_APP_BUNDLE}/Contents/MacOS/resources"
        "${SHOTLET_UNSTRIPPED_DIR}/legacy-resources")
endif()
execute_process(
    COMMAND "${SHOTLET_STRIP}" "${SHOTLET_APP_BINARY}"
    RESULT_VARIABLE strip_result
    ERROR_VARIABLE strip_error
)
if(NOT strip_result EQUAL 0)
    message(FATAL_ERROR "Shotlet 符号剥离失败：${strip_error}")
endif()

execute_process(
    COMMAND "${SHOTLET_CODESIGN}" --force --sign - "${SHOTLET_APP_BUNDLE}"
    RESULT_VARIABLE sign_result
    ERROR_VARIABLE sign_error
)
if(NOT sign_result EQUAL 0)
    message(FATAL_ERROR "Shotlet 签名失败：${sign_error}")
endif()

execute_process(
    COMMAND "${SHOTLET_CODESIGN}" --verify --strict "${SHOTLET_APP_BUNDLE}"
    RESULT_VARIABLE verify_result
    ERROR_VARIABLE verify_error
)
if(NOT verify_result EQUAL 0)
    message(FATAL_ERROR "Shotlet 签名验证失败：${verify_error}")
endif()
