# Applies every patch under app/patches/modules/lvgl to an LVGL checkout, in sorted name order.
#
# Test suites do not run app/CMakeLists.txt, so they call this to build the same LVGL as the
# application (the application's own apply_patches() leaves already applied patches alone).
#
#   rt_gauge_apply_lvgl_patches(<lvgl checkout>)
#
# A patch that is already applied is skipped (`git apply --reverse --check` succeeds). One that
# fails to apply is a FATAL_ERROR. Twister configures several suites at once, and two of them
# could both see "not applied" and then fight over the same `git apply`, so the whole
# check-and-apply runs under a file lock. The lock file lives in the git directory of the
# checkout: that directory exists in every checkout (a fresh `west update` included) and
# nothing there shows up in `git status`.
function(rt_gauge_apply_lvgl_patches lvgl_dir)
    file(GLOB lvgl_patches "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../app/patches/modules/lvgl/*.patch")
    list(SORT lvgl_patches)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${lvgl_patches})

    execute_process(COMMAND git rev-parse --absolute-git-dir
        WORKING_DIRECTORY "${lvgl_dir}"
        OUTPUT_VARIABLE lvgl_git_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE lvgl_git_result ERROR_QUIET)
    if(NOT lvgl_git_result EQUAL 0 OR NOT IS_DIRECTORY "${lvgl_git_dir}")
        message(FATAL_ERROR "${lvgl_dir} is not a git checkout, cannot apply the LVGL patches")
    endif()

    file(LOCK "${lvgl_git_dir}/rt_gauge_patches.lock" GUARD FUNCTION TIMEOUT 600
        RESULT_VARIABLE lvgl_lock_result)
    if(NOT lvgl_lock_result STREQUAL "0")
        message(FATAL_ERROR "Could not lock ${lvgl_git_dir}/rt_gauge_patches.lock: ${lvgl_lock_result}")
    endif()

    foreach(patch IN LISTS lvgl_patches)
        get_filename_component(patch_name "${patch}" NAME)
        execute_process(COMMAND git apply --reverse --check "${patch}"
            WORKING_DIRECTORY "${lvgl_dir}"
            RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
        if(NOT applied EQUAL 0)
            execute_process(COMMAND git apply "${patch}"
                WORKING_DIRECTORY "${lvgl_dir}"
                RESULT_VARIABLE apply_result ERROR_VARIABLE apply_error)
            if(NOT apply_result EQUAL 0)
                message(FATAL_ERROR "Failed to apply ${patch_name} to LVGL: ${apply_error}")
            endif()
        endif()
    endforeach()
endfunction()
