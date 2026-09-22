cmake_minimum_required(VERSION 3.21)
project(terminal_here LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The package must run on a machine that has Seer but not necessarily the VC++
# redistributable, so the runtime is linked statically. The helper shares no C++
# objects across a process boundary, so this is a packaging-only decision.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Sources are UTF-8 without a byte order mark, so the compiler must not guess the
# active code page.
if(MSVC)
    add_compile_options(/utf-8)
endif()

include(CTest)
enable_testing()

set(SEER_CONTROL_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(SEER_CONTROL_TEST_DIR ${SEER_CONTROL_COMMON_DIR}/test)

add_library(terminalhere_core STATIC src/terminalhere.cpp)
target_include_directories(terminalhere_core PUBLIC
    src
    ${SEER_CONTROL_COMMON_DIR}
)
target_compile_definitions(terminalhere_core PUBLIC UNICODE _UNICODE)
if(MSVC)
    target_compile_options(terminalhere_core PRIVATE /W4 /WX /utf-8)
endif()
target_link_libraries(terminalhere_core PUBLIC ole32 shell32 user32 advapi32 gdi32)

add_executable(terminal_here WIN32 src/main.cpp)
target_link_libraries(terminal_here PRIVATE terminalhere_core)
if(MSVC)
    target_compile_options(terminal_here PRIVATE /W4 /WX /utf-8)
endif()

# Test-only fixture: a separate process that reports the argument vector, working
# directory and containment job it actually received.
add_executable(probe_child ${SEER_CONTROL_COMMON_DIR}/test/probe_child.cpp)
target_include_directories(probe_child PRIVATE ${SEER_CONTROL_COMMON_DIR})

add_executable(terminalhere_test test/terminalhere_test.cpp)
add_dependencies(terminalhere_test probe_child)
target_include_directories(terminalhere_test PRIVATE ${SEER_CONTROL_TEST_DIR})
target_link_libraries(terminalhere_test PRIVATE terminalhere_core)
add_test(NAME terminalhere_test
    COMMAND terminalhere_test
        $<TARGET_FILE:terminalhere_test>
        $<TARGET_FILE:probe_child>
)

add_executable(terminalhere_live_test test/live_test.cpp)
add_dependencies(terminalhere_live_test probe_child)
target_include_directories(terminalhere_live_test PRIVATE ${SEER_CONTROL_TEST_DIR})
target_link_libraries(terminalhere_live_test PRIVATE terminalhere_core)
add_test(NAME terminalhere_live_test
    COMMAND terminalhere_live_test $<TARGET_FILE:probe_child>
)
set_tests_properties(terminalhere_live_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(terminalhere_manifest_test test/manifest_test.cpp)
target_include_directories(terminalhere_manifest_test PRIVATE ${SEER_CONTROL_TEST_DIR})

# Staging, ctest wiring, install rules and the install-time manifest contract
# are shared by every Control package; only the identity below is fixed here.
set(TERMINAL_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
include(${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.terminal-here
    PACKAGE_NAME "Terminal Here"
    COMMAND_NAME terminal_here.exe
    COMMAND_TARGET terminal_here
    EXTENSION_TOKEN "\${type_folder}"
    TEST_TARGET terminalhere_manifest_test
    STAGE_TARGET terminalhere_manifest_stage
    PACKAGE_TARGET terminalhere_package
    STAGING_DIR ${TERMINAL_PACKAGE_STAGING_DIR}
)
