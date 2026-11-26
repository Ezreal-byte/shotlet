# Tests CMakeLists.txt
add_executable(scroll_toolbar_bitmap_tests test_scroll_toolbar_bitmap.cpp)
target_link_libraries(scroll_toolbar_bitmap_tests PRIVATE shotlet_core)
add_test(NAME scroll_toolbar_bitmap_tests COMMAND scroll_toolbar_bitmap_tests)

# 宿主截图工具栏（含编辑器模式）回归，随默认测试运行。
add_executable(capture_toolbar_tests test_capture_toolbar.cpp)
target_link_libraries(capture_toolbar_tests PRIVATE shotlet_core)
add_test(NAME capture_toolbar_tests COMMAND capture_toolbar_tests)

# 真实进程测试不依赖 GTest，也不初始化 SDL 或桌面窗口。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME cmake_patch_tests COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cmake_patch.py
        "${SDL3_DIR}" "${CMAKE_COMMAND}" "${GIT_EXECUTABLE}")
    add_executable(daemon_process_probe
        daemon_process_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/i18n/Localization.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/DaemonProcess.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/PlatformMainContext.cpp)
    target_include_directories(daemon_process_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(daemon_process_probe PRIVATE Threads::Threads)
    if(WIN32)
        target_compile_definitions(daemon_process_probe PRIVATE SHOTLET_PLATFORM_WINDOWS)
        target_link_libraries(daemon_process_probe PRIVATE shell32)
        if(MSVC)
            target_compile_options(daemon_process_probe PRIVATE /utf-8)
        endif()
    endif()
    add_test(NAME daemon_process_tests COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_daemon_process.py
        $<TARGET_FILE:daemon_process_probe>)
    set_tests_properties(daemon_process_tests PROPERTIES TIMEOUT 30)
endif()

find_package(GTest QUIET)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SHOTLET_BUILD_LINUX_GUI_TESTS AND NOT GTest_FOUND)
    message(FATAL_ERROR "Linux GUI tests require GoogleTest")
endif()

if(GTest_FOUND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(software_icon_tests test_software_icons.cpp)
        target_link_libraries(software_icon_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME software_icon_tests COMMAND software_icon_tests)
        add_executable(linux_desktop_tests test_linux_desktop.cpp)
        target_link_libraries(linux_desktop_tests PRIVATE shotlet_core GTest::gtest_main)
        find_program(XVFB_RUN_EXECUTABLE xvfb-run)
        if(XVFB_RUN_EXECUTABLE)
            add_test(NAME linux_desktop_tests COMMAND ${XVFB_RUN_EXECUTABLE} -a
                "--server-args=-screen 0 1280x800x24 -noreset" $<TARGET_FILE:linux_desktop_tests>)
            add_test(NAME linux_desktop_rgb565_tests COMMAND ${XVFB_RUN_EXECUTABLE} -a
                "--server-args=-screen 0 1280x800x16 -noreset" $<TARGET_FILE:linux_desktop_tests>)
            set_tests_properties(linux_desktop_rgb565_tests PROPERTIES TIMEOUT 45
                ENVIRONMENT "XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=")
        else()
            add_test(NAME linux_desktop_tests COMMAND linux_desktop_tests)
        endif()
        set_tests_properties(linux_desktop_tests PROPERTIES TIMEOUT 45
            ENVIRONMENT "XDG_SESSION_TYPE=x11;WAYLAND_DISPLAY=")
        if(SHOTLET_BUILD_LINUX_GUI_TESTS)
            find_program(XDOTOOL_EXECUTABLE xdotool REQUIRED)
            if(NOT XVFB_RUN_EXECUTABLE OR NOT Python3_Interpreter_FOUND)
                message(FATAL_ERROR "Linux GUI tests require xvfb-run, Python 3 with Pillow, xdotool, Zenity and a Vulkan driver")
            endif()
            add_test(NAME linux_gui_tests COMMAND ${XVFB_RUN_EXECUTABLE} -a
                "--server-args=-screen 0 1280x800x24 -noreset"
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_linux_gui.py
                --app $<TARGET_FILE:shotlet> --artifacts ${CMAKE_CURRENT_BINARY_DIR}/linux-gui-artifacts)
            set_tests_properties(linux_gui_tests PROPERTIES TIMEOUT 90)
            foreach(mode software fallback)
                add_test(NAME linux_gui_${mode}_tests COMMAND ${XVFB_RUN_EXECUTABLE} -a
                    "--server-args=-screen 0 1280x800x24 -noreset"
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_linux_gui.py
                    --app $<TARGET_FILE:shotlet> --expect-software
                    --artifacts ${CMAKE_CURRENT_BINARY_DIR}/linux-gui-${mode}-artifacts)
                # A nonexistent ICD disables Vulkan even on machines with lavapipe.
                set_tests_properties(linux_gui_${mode}_tests PROPERTIES TIMEOUT 90
                    ENVIRONMENT "SHOTLET_RENDERER=$<IF:$<STREQUAL:${mode},software>,software,auto>;VK_DRIVER_FILES=/nonexistent-shotlet-icd.json;VK_ICD_FILENAMES=/nonexistent-shotlet-icd.json")
            endforeach()
        endif()
    endif()
    add_executable(recording_tests test_recording.cpp test_async_mailbox.cpp)
    target_link_libraries(recording_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME recording_tests COMMAND recording_tests)
    set_tests_properties(recording_tests PROPERTIES TIMEOUT 30)
    if(WIN32)
        add_executable(recording_windows_tests test_recording_stop_win.cpp test_recording_playback_win.cpp)
        target_link_libraries(recording_windows_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME recording_windows_tests COMMAND recording_windows_tests)
        set_tests_properties(recording_windows_tests PROPERTIES TIMEOUT 45)
    endif()

    # 使用 SDL/GPU 接口桩验证最小化与恢复时的命令生命周期，无需桌面和显卡。
    add_executable(imgui_gpu_frame_tests test_imgui_gpu_frame.cpp ${IMGUI_CORE})
    target_include_directories(imgui_gpu_frame_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${SDL3_DIR}/include ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(imgui_gpu_frame_tests PRIVATE GTest::gtest_main)
    if(MSVC)
        target_compile_options(imgui_gpu_frame_tests PRIVATE /utf-8)
    endif()
    add_test(NAME imgui_gpu_frame_tests COMMAND imgui_gpu_frame_tests)

    # 使用共享 ImGui 控件验证 Windows 标题栏，不依赖特定桌面或 GPU 后端。
    add_executable(editor_titlebar_tests test_editor_titlebar.cpp)
    target_link_libraries(editor_titlebar_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME editor_titlebar_tests COMMAND editor_titlebar_tests)
    # 水印工具与子工具栏输入框回归，复用真实 ImGui 控件，无需桌面窗口或 GPU。
    add_executable(watermark_tests test_watermark.cpp)
    target_link_libraries(watermark_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME watermark_tests COMMAND watermark_tests)
    add_executable(history_process_probe history_process_probe.cpp)
    target_link_libraries(history_process_probe PRIVATE shotlet_core)
    add_executable(history_tests test_history.cpp)
    target_link_libraries(history_tests PRIVATE shotlet_core GTest::gtest_main)
    add_dependencies(history_tests history_process_probe)
    target_compile_definitions(history_tests PRIVATE SHOTLET_HISTORY_PROCESS_PROBE="$<TARGET_FILE:history_process_probe>")
    add_test(NAME history_tests COMMAND history_tests)
    # 使用真实 ImGui 控件回归历史窗口状态与布局，不创建桌面窗口或 GPU 设备。
    add_executable(history_window_tests test_history_window.cpp)
    target_link_libraries(history_window_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME history_window_tests COMMAND history_window_tests)
    if(APPLE OR WIN32)
        # 使用真实窗口与平台 GPU，回归任务栏/Dock 最小化、恢复和最小化期间关闭。
        add_executable(history_window_lifecycle_tests test_history_window_lifecycle.cpp)
        target_link_libraries(history_window_lifecycle_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME history_window_lifecycle_tests COMMAND history_window_lifecycle_tests)
    endif()
    # 各窗口共用标题栏常量与控件组回归，防止窗口样式再次漂移。
    add_executable(window_chrome_tests test_window_chrome.cpp)
    target_link_libraries(window_chrome_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME window_chrome_tests COMMAND window_chrome_tests)
    add_executable(hotkey_binding_tests test_hotkey_binding.cpp)
    target_link_libraries(hotkey_binding_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME hotkey_binding_tests COMMAND hotkey_binding_tests)

    add_executable(preferences_tests test_preferences.cpp)
    target_link_libraries(preferences_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME preferences_tests COMMAND preferences_tests)

    add_executable(theme_tests test_theme.cpp)
    target_link_libraries(theme_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME theme_tests COMMAND theme_tests)

    add_executable(localization_tests test_localization.cpp)
    target_link_libraries(localization_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME localization_tests COMMAND localization_tests)

    # 使用 SDL 接口桩验证托盘生命周期和退出回调，不创建桌面图标。
    add_executable(system_tray_tests
        test_system_tray.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/SystemTray.cpp)
    target_include_directories(system_tray_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${SDL3_DIR}/include ${IMGUI_DIR})
    target_link_libraries(system_tray_tests PRIVATE GTest::gtest_main)
    target_compile_definitions(system_tray_tests PRIVATE SHOTLET_TRAY_SDL_BACKEND)
    if(WIN32)
        # 此目标直接编译托盘源码，需与主程序选择相同的平台画布尺寸。
        target_compile_definitions(system_tray_tests PRIVATE SHOTLET_PLATFORM_WINDOWS)
    endif()
    if(MSVC)
        target_compile_options(system_tray_tests PRIVATE /utf-8)
    endif()
    add_test(NAME system_tray_tests COMMAND system_tray_tests)

    # 复用同一图形的像素回归；Windows 额外检查真实窗口的大、小原生图标及其生命周期。
    add_executable(application_icon_tests test_application_icon.cpp)
    target_link_libraries(application_icon_tests PRIVATE shotlet_core GTest::gtest_main)
    add_test(NAME application_icon_tests COMMAND application_icon_tests)

    # Test sources - pure logic tests, no SDL/ImGui/OpenGL dependencies
    set(TEST_SOURCES
        test_main.cpp
        test_math.cpp
        test_screen_overlay.cpp
        test_overlay_manager.cpp
        test_screen_capture.cpp
        test_magnifier_overlay.cpp
        test_logger.cpp
        test_canvas.cpp
        test_capture_document.cpp
        test_image_editor.cpp
        test_text_layer.cpp
        test_arrow_layer.cpp
        test_number_marker.cpp
        test_highlight.cpp
        test_smart_region_detector.cpp
        test_long_image_store.cpp
        test_scroll_capture_stitcher.cpp
        test_scroll_capture_real_image.cpp
        test_scroll_planner.cpp
        test_smooth_scroll_input.cpp
        test_scroll_capture_controller.cpp
        test_frame_stability.cpp
        test_scroll_wheel_units.cpp
        test_ocr_window_layout.cpp
        test_window_scale.cpp
        test_application_structure.cpp
        test_overlay_reclaim_timer.cpp
        test_capture_safety_state.cpp
    )

    # OverlayManager 链入真实现（编辑器路由/多屏逻辑必须与生产同源，桩仅保留测试缝隙）
    add_executable(shotlet_tests ${TEST_SOURCES}
        screen_overlay_test_impl.cpp
        overlay_manager_test_impl.cpp
        magnifier_overlay_test_impl.cpp
        image_editor_test_impl.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/OverlayManager.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/TransparentWindow_stub.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/GpuTextureUpload.cpp
        ${CMAKE_SOURCE_DIR}/src/capture/ScreenCapture.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CanvasState.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CanvasTextInput.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CanvasInteraction.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CanvasRender.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CanvasImage.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CaptureDocument.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CanvasDocument.cpp
        ${CMAKE_SOURCE_DIR}/src/core/LayerManager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/ArrowLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/BrushLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/EllipseLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/LayerRasterUtils.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/MosaicLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/RectangleLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/TextLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/NumberLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/layers/HighlightLayer.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/MaskGeometry.cpp
        ${CMAKE_SOURCE_DIR}/src/core/CursorManager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SmartRegionDetector.cpp
        ${CMAKE_SOURCE_DIR}/src/core/SmartRegionDetector_stub.cpp
        ${CMAKE_SOURCE_DIR}/src/scrollcapture/LongImageStore.cpp
        ${CMAKE_SOURCE_DIR}/src/scrollcapture/ScrollCaptureStitcher.cpp
        ${CMAKE_SOURCE_DIR}/src/scrollcapture/ScrollPlanner.cpp
        ${CMAKE_SOURCE_DIR}/src/scrollcapture/ScrollCaptureController.cpp
        ${CMAKE_SOURCE_DIR}/src/scrollcapture/FrameStability.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/DashedLine.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/Theme.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/OcrWindowLayout.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/FontProvider_stub.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
    )

    # TransparentWindow_stub.cpp 内部 include "TransparentWindow.h"（无目录前缀）
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/platform/TransparentWindow_stub.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/platform")

    # Logger 使用真实平台分支，宏仅作用于该源文件，保留其他测试的平台桩。
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp PROPERTIES
        COMPILE_DEFINITIONS "$<$<PLATFORM_ID:Windows>:SHOTLET_PLATFORM_WINDOWS>;$<$<PLATFORM_ID:Darwin>:SHOTLET_PLATFORM_MACOS>;$<$<PLATFORM_ID:Linux>:SHOTLET_PLATFORM_LINUX>")

    target_include_directories(shotlet_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
    )

    target_link_libraries(shotlet_tests
        GTest::gtest
        GTest::gtest_main
        ${SDL3_TARGET}
        Threads::Threads
    )

    if(MSVC)
        target_compile_options(shotlet_tests PRIVATE /W3 /MP /Zc:__cplusplus /utf-8)
        target_compile_definitions(shotlet_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_compile_definitions(shotlet_tests PRIVATE SHOTLET_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    add_test(NAME shotlet_tests COMMAND shotlet_tests)

    if(WIN32)
        add_executable(windows_capture_guard_tests test_windows_capture_guard.cpp)
        target_link_libraries(windows_capture_guard_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME windows_capture_guard_tests COMMAND windows_capture_guard_tests)
        set_tests_properties(windows_capture_guard_tests PROPERTIES TIMEOUT 20)

        add_executable(windows_command_line_tests test_windows_command_line.cpp)
        target_link_libraries(windows_command_line_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME windows_command_line_tests COMMAND windows_command_line_tests)
    endif()

    if(APPLE)
        add_executable(recording_native_tests MACOSX_BUNDLE test_recording_native.mm test_recording_playback.mm test_recording_continuity.mm test_recording_pause.mm)
        set_target_properties(recording_native_tests PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/macos/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.shotlet.recording-tests"
            MACOSX_BUNDLE_BUNDLE_NAME "Shotlet Recording Tests")
        set_source_files_properties(test_recording_native.mm test_recording_playback.mm test_recording_continuity.mm test_recording_pause.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(recording_native_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME recording_native_tests COMMAND recording_native_tests)

        add_executable(settings_window_tests test_settings_window.mm)
        set_source_files_properties(test_settings_window.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(settings_window_tests PRIVATE shotlet_core GTest::gtest_main)
        target_compile_definitions(settings_window_tests PRIVATE SHOTLET_SETTINGS_PREVIEW_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        add_test(NAME settings_window_tests COMMAND settings_window_tests)

        add_executable(clipboard_copy_tests test_clipboard_copy.mm)
        set_source_files_properties(test_clipboard_copy.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(clipboard_copy_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME clipboard_copy_tests COMMAND clipboard_copy_tests)

        # 真实验证 macOS pool 排空、嵌套和显式所有权，不创建桌面窗口。
        add_executable(platform_autorelease_pool_tests test_platform_autorelease_pool.mm)
        target_link_libraries(platform_autorelease_pool_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME platform_autorelease_pool_tests COMMAND platform_autorelease_pool_tests)

        # 平台测试只构造事件和内存帧，不安装 tap、不向桌面注入滚轮。
        add_executable(scroll_capture_platform_tests test_scroll_capture_platform.cpp)
        target_link_libraries(scroll_capture_platform_tests PRIVATE
            shotlet_core GTest::gtest_main)
        add_test(NAME scroll_capture_platform_tests COMMAND scroll_capture_platform_tests)
        add_executable(scroll_capture_output_tests test_scroll_capture_output.mm)
        target_compile_definitions(scroll_capture_output_tests PRIVATE SHOTLET_HISTORY_PREVIEW_DIR="${CMAKE_CURRENT_BINARY_DIR}")
        set_source_files_properties(test_scroll_capture_output.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(scroll_capture_output_tests PRIVATE shotlet_core GTest::gtest_main)
        add_test(NAME scroll_capture_output_tests COMMAND scroll_capture_output_tests)
    endif()
else()
    message(STATUS "GTest not found, skipping tests")
endif()

if(APPLE AND TARGET shotlet_node)
    add_test(
        NAME shotlet_node_macos_symbols
        COMMAND ${CMAKE_COMMAND}
            -DSHOTLET_NODE_PATH=$<TARGET_FILE:shotlet_node>
            -DCMAKE_NM=${CMAKE_NM}
            "-DSHOTLET_CHECK_EXPORTS=$<IF:$<CONFIG:Release,MinSizeRel>,ON,OFF>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_macos_node_symbols.cmake
    )
endif()
