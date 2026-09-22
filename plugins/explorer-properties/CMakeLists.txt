cmake_minimum_required(VERSION 3.21)
project(explorer_properties LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
enable_testing()

add_library(shellverb_core STATIC src/shellverb.cpp)
target_include_directories(shellverb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(shellverb_properties WIN32 src/main.cpp)
target_include_directories(shellverb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_link_libraries(shellverb_properties PRIVATE shellverb_core Shell32.lib)

add_executable(shellverb_properties_test test/main_test.cpp)
target_include_directories(shellverb_properties_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)
target_link_libraries(shellverb_properties_test PRIVATE shellverb_core Shell32.lib)
add_test(NAME shellverb_properties_test COMMAND shellverb_properties_test)

# The reason channel the launcher uses to forward a worker-side refusal to the
# host is shared header-only code, so it is exercised here rather than in a
# package-specific test.
add_executable(shelluiworker_test ../common/test/shelluiworker_test.cpp)
target_include_directories(shelluiworker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)
target_link_libraries(shelluiworker_test PRIVATE Shell32.lib)
add_test(NAME shelluiworker_test COMMAND shelluiworker_test)

add_executable(shellverb_manifest_test test/manifest_test.cpp)
target_include_directories(shellverb_manifest_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/test
)

set(SHELLVERB_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.explorer-properties
    PACKAGE_NAME "Explorer Properties"
    COMMAND_NAME shellverb_properties.exe
    COMMAND_TARGET shellverb_properties
    EXTENSION_TOKEN "\${type_file}"
    TEST_TARGET shellverb_manifest_test
    STAGE_TARGET shellverb_manifest_stage
    PACKAGE_TARGET shellverb_package
    STAGING_DIR ${SHELLVERB_PACKAGE_STAGING_DIR}
)
