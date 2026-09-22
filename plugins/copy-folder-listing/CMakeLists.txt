cmake_minimum_required(VERSION 3.21)
project(copy_folder_listing LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Self-contained package: the VC++ runtime is linked in, and the compiler is told
# the sources are UTF-8 rather than guessing the active code page.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    add_compile_options(/utf-8)
endif()

include(CTest)
enable_testing()

set(SEER_CONTROL_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(SEER_CONTROL_TEST_DIR ${SEER_CONTROL_COMMON_DIR}/test)

add_library(folderlisting_core STATIC src/folderlisting.cpp)
target_include_directories(folderlisting_core PUBLIC
    src
    ${SEER_CONTROL_COMMON_DIR}
)
target_compile_definitions(folderlisting_core PUBLIC UNICODE _UNICODE)
if(MSVC)
    target_compile_options(folderlisting_core PRIVATE /W4 /WX)
endif()
target_link_libraries(folderlisting_core PUBLIC ole32 shell32 user32 advapi32 gdi32)

add_executable(copy_folder_listing WIN32 src/main.cpp)
target_link_libraries(copy_folder_listing PRIVATE folderlisting_core)
if(MSVC)
    target_compile_options(copy_folder_listing PRIVATE /W4 /WX)
endif()

add_executable(probe_child ${SEER_CONTROL_COMMON_DIR}/test/probe_child.cpp)
target_include_directories(probe_child PRIVATE ${SEER_CONTROL_COMMON_DIR})
if(MSVC)
    target_compile_options(probe_child PRIVATE /W4 /WX)
endif()

add_executable(folderlisting_test test/folderlisting_test.cpp)
add_dependencies(folderlisting_test probe_child copy_folder_listing)
target_include_directories(folderlisting_test PRIVATE ${SEER_CONTROL_TEST_DIR})
target_link_libraries(folderlisting_test PRIVATE folderlisting_core)
if(MSVC)
    target_compile_options(folderlisting_test PRIVATE /W4 /WX)
endif()
add_test(NAME folderlisting_test
    COMMAND folderlisting_test
        $<TARGET_FILE:folderlisting_test>
        $<TARGET_FILE:probe_child>
        $<TARGET_FILE:copy_folder_listing>
)

add_executable(folderlisting_manifest_test test/manifest_test.cpp)
target_include_directories(folderlisting_manifest_test PRIVATE ${SEER_CONTROL_TEST_DIR})
if(MSVC)
    target_compile_options(folderlisting_manifest_test PRIVATE /W4 /WX)
endif()

set(LISTING_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
include(${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.copy-folder-listing
    PACKAGE_NAME "Copy Folder Listing"
    COMMAND_NAME copy_folder_listing.exe
    COMMAND_TARGET copy_folder_listing
    EXTENSION_TOKEN "\${type_folder}"
    TEST_TARGET folderlisting_manifest_test
    STAGE_TARGET folderlisting_manifest_stage
    PACKAGE_TARGET folderlisting_package
    STAGING_DIR ${LISTING_PACKAGE_STAGING_DIR}
)
