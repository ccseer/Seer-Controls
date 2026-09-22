cmake_minimum_required(VERSION 3.21)
project(explorer_open_with LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
enable_testing()

add_library(openwith_core STATIC src/openwith.cpp)
target_include_directories(openwith_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)
target_link_libraries(openwith_core PUBLIC shell32)

add_executable(shellopenwith WIN32 src/main.cpp)
target_link_libraries(shellopenwith PRIVATE openwith_core shell32)

add_executable(shellopenwith_test test/openwith_test.cpp)
target_include_directories(shellopenwith_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)
target_link_libraries(shellopenwith_test PRIVATE openwith_core)
add_dependencies(shellopenwith_test shellopenwith)
# The process-level check needs the real helper: stderr is the host's only
# failure channel, so it can only be observed from a redirected child pipe.
add_test(NAME shellopenwith_test
    COMMAND shellopenwith_test $<TARGET_FILE:shellopenwith>
)

add_executable(openwith_manifest_test test/manifest_test.cpp)
target_include_directories(openwith_manifest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)

set(OPENWITH_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.explorer-open-with
    PACKAGE_NAME "Open With"
    COMMAND_NAME shellopenwith.exe
    COMMAND_TARGET shellopenwith
    EXTENSION_TOKEN "\${type_file}"
    TEST_TARGET openwith_manifest_test
    STAGE_TARGET openwith_manifest_stage
    PACKAGE_TARGET openwith_package
    STAGING_DIR ${OPENWITH_MANIFEST_STAGING_DIR}
)
