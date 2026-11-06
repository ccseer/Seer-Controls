cmake_minimum_required(VERSION 3.21)
project(seer_image_quantize LANGUAGES C CXX)

cmake_policy(SET CMP0091 NEW)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# A Control package ships one executable with no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX)

include(CTest)
enable_testing()

set(SEER_CONTROL_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(SQ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SQ_ENGINE_DIR "${SQ_ROOT}/src/engine")
set(SQ_TOOLS_THIRDPARTY_DIR "${SQ_ROOT}/tools/thirdparty")

# Package-owned code is held to /W4 /WX. Vendored code is compiled by a
# dedicated target with /W0 and its headers marked external, so third-party
# warnings cannot fail a /WX build.
function(sq_apply_package_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /EHsc /fp:precise)
    endif()
endfunction()

# MSVC ships AddressSanitizer only as a dynamic runtime. Copying it beside each
# ASan executable keeps the sanitizer build runnable without the toolchain on
# PATH, which a plain double-click or a CI shell cannot be assumed to provide.
function(sq_stage_asan_runtime target)
endfunction()

if(MSVC AND CMAKE_CXX_FLAGS MATCHES "/fsanitize=address")
    get_filename_component(SQ_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(SQ_ASAN_DLL "${SQ_MSVC_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
    if(EXISTS "${SQ_ASAN_DLL}")
        function(sq_stage_asan_runtime target)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SQ_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endfunction()
    endif()
endif()

add_subdirectory(src/engine)
add_subdirectory(tools)
add_subdirectory(src/helper)
add_subdirectory(test)

include("${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake")
find_program(SQ_SEVEN_ZIP NAMES 7z 7za HINTS "C:/Program Files/7-Zip")
set(SQ_STAGING_DIR "${CMAKE_BINARY_DIR}/stage/image-quantize")
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.image-quantize
    PACKAGE_NAME "Quantize Image"
    COMMAND_NAME image_quantize.exe
    COMMAND_TARGET image_quantize
    EXTENSION_TOKEN "\${type_image}"
    TEST_TARGET image_quantize_manifest_test
    STAGE_TARGET imagequantize_manifest_stage
    PACKAGE_TARGET imagequantize_package
    STAGING_DIR ${SQ_STAGING_DIR})

if(SQ_SEVEN_ZIP)
    add_custom_command(TARGET imagequantize_package POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
        COMMAND "${SQ_SEVEN_ZIP}" a -tzip -mx=9 -mmt=on
                "${CMAKE_SOURCE_DIR}/dist/image-quantize-1.0.0.zip" *
        WORKING_DIRECTORY "${SQ_STAGING_DIR}"
        VERBATIM)
else()
    message(WARNING "7z was not found; the package ZIP will not be produced")
endif()
