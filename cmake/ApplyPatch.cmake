include_guard(GLOBAL)

# 将依赖补丁的检查、幂等应用和并发保护集中到构建工具层。
function(shotlet_apply_patch source_dir patch_file)
    find_package(Git REQUIRED)
    set(patch_lock_timeout_seconds 30)
    # 多个架构可以共享依赖源码；锁覆盖检查与应用，避免同时改写同一个文件。
    file(LOCK "${source_dir}/.shotlet-patch.lock" GUARD FUNCTION
        TIMEOUT "${patch_lock_timeout_seconds}")

    # 补丁只匹配实际配置行，避免历史版本的品牌注释影响重复配置。
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET
    )
    if(already_applied EQUAL 0)
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE check_result
        ERROR_VARIABLE check_error
    )
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR
            "补丁与依赖源码不匹配，请核对依赖版本及本地修改：${patch_file}\n${check_error}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE apply_result
        ERROR_VARIABLE apply_error
    )
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "依赖补丁应用失败：${patch_file}\n${apply_error}")
    endif()
    message(STATUS "已应用依赖补丁：${patch_file}")
endfunction()
