# SDK 默认不参与既有应用或 Node addon 的构建，启用后提供独立静态接口和辅助进程。
add_library(shotlet_capture_sdk STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/sdk/CaptureSDK.cpp")
target_include_directories(shotlet_capture_sdk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(shotlet_capture_sdk PUBLIC shotlet_core)
add_executable(shotlet_capture "${CMAKE_CURRENT_SOURCE_DIR}/src/sdk/capture_main.cpp")
target_link_libraries(shotlet_capture PRIVATE shotlet_capture_sdk)
set_target_properties(shotlet_capture PROPERTIES OUTPUT_NAME "shotlet-capture")
if(APPLE)
    set_target_properties(shotlet_capture PROPERTIES
        OUTPUT_NAME "ShotletCapture"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.shotlet.capture"
        MACOSX_BUNDLE_BUNDLE_NAME "Shotlet 截图"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    foreach(language en zh-Hans)
        set(localized_info "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/${language}.lproj/InfoPlist.strings")
        target_sources(shotlet_capture PRIVATE "${localized_info}")
    endforeach()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
        set(capture_resources "$<TARGET_BUNDLE_DIR:shotlet_capture>/Contents/Resources")
        add_custom_command(TARGET shotlet_capture POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${capture_resources}"
            VERBATIM)
    endif()
    add_custom_command(TARGET shotlet_capture POST_BUILD
        COMMAND ${SHOTLET_CODESIGN_EXECUTABLE} --force --sign - "$<TARGET_BUNDLE_DIR:shotlet_capture>"
        VERBATIM)
else()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
        add_custom_command(TARGET shotlet_capture POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:shotlet_capture>/resources"
            VERBATIM)
    endif()
endif()
# 导出脚本只读取此文件，避免猜测不同生成器的产物名称及目录。
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/capture-sdk-$<CONFIG>.json" CONTENT
"{\n\"sdk\":\"$<TARGET_FILE:shotlet_capture_sdk>\",\n\"core\":\"$<TARGET_FILE:shotlet_core>\",\n\"sdl\":\"$<TARGET_FILE:SDL3-static>\",\n\"helper\":\"$<TARGET_FILE:shotlet_capture>\",\n\"version\":\"${PROJECT_VERSION}\",\n\"minimum_macos\":\"${CMAKE_OSX_DEPLOYMENT_TARGET}\"\n}\n")
# 无窗口 C 接口契约验证，确保公共头文件可以直接被 C 调用方使用。
add_executable(shotlet_capture_contract_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_sdk.c")
target_link_libraries(shotlet_capture_contract_tests PRIVATE shotlet_capture_sdk)
add_custom_target(capture_sdk_check
    COMMAND $<TARGET_FILE:shotlet_capture_contract_tests>
    DEPENDS shotlet_capture_contract_tests VERBATIM)
