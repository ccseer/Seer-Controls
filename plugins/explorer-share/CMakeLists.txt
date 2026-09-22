cmake_minimum_required(VERSION 3.21)
project(explorer_share LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED CPPWINRT_INCLUDE_DIR OR NOT EXISTS "${CPPWINRT_INCLUDE_DIR}/winrt/base.h")
    message(FATAL_ERROR "CPPWINRT_INCLUDE_DIR must be set to a directory containing winrt/base.h")
endif()

include(CTest)
enable_testing()

add_library(share_core STATIC
    src/share.cpp
    src/sharefile.h
)
target_include_directories(share_core PUBLIC src ${CPPWINRT_INCLUDE_DIR})
if(MSVC)
    target_compile_definitions(share_core PUBLIC
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
endif()
target_include_directories(share_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_link_libraries(share_core PUBLIC windowsapp ole32 user32 shell32)

add_executable(seer_share WIN32
    src/main.cpp
)
target_link_libraries(seer_share PRIVATE share_core)

add_executable(share_test
    test/share_test.cpp
)
target_include_directories(share_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)
target_link_libraries(share_test PRIVATE share_core)
add_test(NAME share_test COMMAND share_test)

add_executable(share_manifest_test test/manifest_test.cpp)
target_include_directories(share_manifest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)

set(SHARE_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.explorer-share
    PACKAGE_NAME "Share"
    COMMAND_NAME seer_share.exe
    COMMAND_TARGET seer_share
    EXTENSION_TOKEN "\${type_file}"
    TEST_TARGET share_manifest_test
    STAGE_TARGET share_manifest_stage
    PACKAGE_TARGET share_package
    STAGING_DIR ${SHARE_MANIFEST_STAGING_DIR}
)
