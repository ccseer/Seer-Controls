cmake_minimum_required(VERSION 3.21)
project(file_lock_info LANGUAGES CXX)

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

add_library(filelock_core STATIC src/filelock.cpp)
target_include_directories(filelock_core PUBLIC
    src
    ${SEER_CONTROL_COMMON_DIR}
)
target_compile_definitions(filelock_core PUBLIC UNICODE _UNICODE)
if(MSVC)
    target_compile_options(filelock_core PRIVATE /W4 /WX)
endif()
target_link_libraries(filelock_core PUBLIC
    ole32 shell32 user32 advapi32 gdi32 Rstrtmgr
)

add_executable(file_lock_info WIN32 src/main.cpp)
target_link_libraries(file_lock_info PRIVATE filelock_core)
if(MSVC)
    target_compile_options(file_lock_info PRIVATE /W4 /WX)
endif()

add_executable(probe_child ${SEER_CONTROL_COMMON_DIR}/test/probe_child.cpp)
target_include_directories(probe_child PRIVATE ${SEER_CONTROL_COMMON_DIR})

add_executable(filelock_test test/filelock_test.cpp)
add_dependencies(filelock_test probe_child)
target_include_directories(filelock_test PRIVATE ${SEER_CONTROL_TEST_DIR})
target_link_libraries(filelock_test PRIVATE filelock_core)
add_test(NAME filelock_test
    COMMAND filelock_test
        $<TARGET_FILE:filelock_test>
        $<TARGET_FILE:probe_child>
)

add_executable(filelock_manifest_test test/manifest_test.cpp)
target_include_directories(filelock_manifest_test PRIVATE ${SEER_CONTROL_TEST_DIR})

# Staging, ctest wiring, install rules and the install-time manifest contract
# are shared by every Control package; only the identity below is fixed here.
set(FILELOCK_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
include(${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.file-lock-info
    PACKAGE_NAME "Who Is Locking This File"
    COMMAND_NAME file_lock_info.exe
    COMMAND_TARGET file_lock_info
    EXTENSION_TOKEN "\${type_file}"
    TEST_TARGET filelock_manifest_test
    STAGE_TARGET filelock_manifest_stage
    PACKAGE_TARGET filelock_package
    STAGING_DIR ${FILELOCK_PACKAGE_STAGING_DIR}
)
