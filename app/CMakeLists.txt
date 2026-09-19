cmake_minimum_required(VERSION 3.20)

# Apply patches before build
function(apply_patches patch_dir target_dir)
    file(GLOB_RECURSE files RELATIVE ${CMAKE_SOURCE_DIR} "${patch_dir}/*.patch")
    foreach(file ${files})
        execute_process(
            COMMAND git apply --reverse --check ${CMAKE_CURRENT_SOURCE_DIR}/${file} --unsafe-paths
            WORKING_DIRECTORY ${target_dir}
            RESULT_VARIABLE patch_already_applied
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(patch_already_applied EQUAL 0)
            message("Patch already applied: ${file}")
        else()
            message("Applying patch: ${file}")
            execute_process(
                COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/${file} --unsafe-paths
                WORKING_DIRECTORY ${target_dir}
                RESULT_VARIABLE patch_apply_result
            )
            if(NOT patch_apply_result EQUAL 0)
                message(FATAL_ERROR "Failed to apply patch: ${file}")
            endif()
        endif()
    endforeach()
endfunction()

apply_patches("patches/zephyr" $ENV{ZEPHYR_BASE})
apply_patches("patches/modules/lvgl" $ENV{ZEPHYR_BASE}/../modules/lib/gui/lvgl)

# ZCBOR code generation function
function(generate_zcbor_configuration cddl_path base_name)
    # Convert base_name to PascalCase for the type name
    string(REPLACE "_" ";" name_parts ${base_name})
    set(type_name "")
    foreach(part ${name_parts})
        string(SUBSTRING ${part} 0 1 first_char)
        string(TOUPPER ${first_char} first_char_upper)
        string(SUBSTRING ${part} 1 -1 rest_chars)
        string(APPEND type_name "${first_char_upper}${rest_chars}")
    endforeach()

    # Define output paths
    set(output_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/configuration/cbor/generated/${base_name}")
    set(output_cbor_h "${output_dir}/${base_name}_cbor.h")
    set(output_cbor_c "${output_dir}/${base_name}_cbor.c")
    set(output_cbor_decode_h "${output_dir}/${base_name}_cbor_decode.h")
    set(output_cbor_decode_c "${output_dir}/${base_name}_cbor_decode.c")
    set(output_cbor_encode_h "${output_dir}/${base_name}_cbor_encode.h")
    set(output_cbor_encode_c "${output_dir}/${base_name}_cbor_encode.c")
    set(output_types_h "${output_dir}/${base_name}.h")

    # Define the zcbor code generation command
    add_custom_command(
        OUTPUT
            ${output_cbor_decode_h}
            ${output_cbor_decode_c}
            ${output_cbor_encode_h}
            ${output_cbor_encode_c}
            ${output_types_h}
        COMMAND zcbor code -sed --default-max-qty 24
            -c ${cddl_path}
            -t ${type_name}
            --output-h ${output_cbor_h}
            --output-c ${output_cbor_c}
            --output-h-types ${output_types_h}
        DEPENDS
            ${cddl_path}
        COMMENT "Generating CBOR code from ${cddl_path} for ${base_name}"
        VERBATIM
    )

    # Create a custom target to trigger the code generation automatically
    add_custom_target(generate_${base_name}_cbor ALL
        DEPENDS
            ${output_cbor_decode_h}
            ${output_cbor_decode_c}
            ${output_cbor_encode_h}
            ${output_cbor_encode_c}
            ${output_types_h}
    )
endfunction()

# generate_zcbor_configuration(
#     ${ZEPHYR_BASE}/../project/modules/eerie_leap_rt_core/src/configuration/cbor/cbor_schemas/system_config.cddl
#     "cbor_system_config")
# generate_zcbor_configuration(
#     ${CMAKE_CURRENT_SOURCE_DIR}/src/configuration/cbor/cbor_schemas/ui_config.cddl
#     "cbor_ui_config")

# Add Zephyr modules
set(EXTRA_ZEPHYR_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/../modules/eerie_leap_lua"
    "${CMAKE_CURRENT_SOURCE_DIR}/../modules/eerie_leap_rt_core")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(eerie_leap_round_gauge)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/lvgl_vector.cmake)

# Enable compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C++23 globally (Zephyr-compatible way)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Collect all source files
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
  src/*.cpp
  src/*.c
)

# Include headers
target_include_directories(app PRIVATE src)

# Add sources to the app target
target_sources(app PRIVATE ${APP_SOURCES})
