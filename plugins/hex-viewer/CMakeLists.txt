cmake_minimum_required(VERSION 3.21)
project(seer_hex LANGUAGES CXX)

cmake_policy(SET CMP0091 NEW)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The package must run on a machine that has Seer but not necessarily the VC++
# redistributable, so every target (helper and tests alike) links the runtime
# statically; mixing static and dynamic CRT TUs would trip LNK4098.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/utf-8 /W4 /WX /EHsc)
endif()

add_compile_definitions(UNICODE _UNICODE NOMINMAX)

include(CTest)
enable_testing()

set(SEER_CONTROL_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(SEER_CONTROL_TEST_DIR ${SEER_CONTROL_COMMON_DIR}/test)

set(HEXVIEWER_CORE_SOURCES
    src/hexformat.cpp
    src/hexoptions.cpp
    src/hexpaths.cpp
    src/atomicfilewriter.cpp
    src/hexdump.cpp
    src/hexlauncher.cpp
    src/hexsweep.cpp
)

set(HEXVIEWER_CORE_HEADERS
    src/hexformat.h
    src/hexoptions.h
    src/hexpaths.h
    src/atomicfilewriter.h
    src/hexdump.h
    src/hexlauncher.h
    src/hexsweep.h
)

add_library(hexviewer_core STATIC ${HEXVIEWER_CORE_SOURCES} ${HEXVIEWER_CORE_HEADERS})
target_include_directories(hexviewer_core PUBLIC src ${SEER_CONTROL_COMMON_DIR})
# ole32 serves CoInitializeEx from controlentry.h, shell32 serves
# CommandLineToArgvW and ShellExecuteExW, user32 serves the error MessageBoxW.
# KERNEL32 is linked by the MSVC default set and still shows up in the import
# table, which the packaging check expects.
target_link_libraries(hexviewer_core PUBLIC ole32 shell32 user32)

add_executable(seer_hex WIN32 src/main.cpp res/seer_hex.rc)
# The .rc embeds hex.ico, which CMake cannot infer from the script text; without
# this an edited icon would keep the previous resource in an incremental build.
set_source_files_properties(res/seer_hex.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/hex.ico")
target_link_libraries(seer_hex PRIVATE hexviewer_core)

add_executable(fake_viewer WIN32 test/fake_viewer.cpp)
target_link_libraries(fake_viewer PRIVATE shell32)

add_executable(hexformat_test test/hexformat_test.cpp)
target_link_libraries(hexformat_test PRIVATE hexviewer_core)
target_include_directories(hexformat_test PRIVATE ${SEER_CONTROL_TEST_DIR})
add_test(NAME hexformat_test COMMAND hexformat_test)

add_executable(hexpaths_test test/hexpaths_test.cpp)
target_link_libraries(hexpaths_test PRIVATE hexviewer_core)
target_include_directories(hexpaths_test PRIVATE ${SEER_CONTROL_TEST_DIR})
add_test(NAME hexpaths_test COMMAND hexpaths_test)

add_executable(hexdump_test test/hexdump_test.cpp)
add_dependencies(hexdump_test seer_hex fake_viewer)
target_link_libraries(hexdump_test PRIVATE hexviewer_core)
target_include_directories(hexdump_test PRIVATE ${SEER_CONTROL_TEST_DIR})
add_test(NAME hexdump_test
    COMMAND hexdump_test
        $<TARGET_FILE:seer_hex>
        $<TARGET_FILE:fake_viewer>)

add_executable(hexviewer_manifest_test test/manifest_test.cpp)
target_include_directories(hexviewer_manifest_test PRIVATE ${SEER_CONTROL_TEST_DIR} ${SEER_CONTROL_COMMON_DIR})

set(HEX_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
include(${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake)
# No icon_path: the control bar takes the button icon from the helper
# executable's embedded icon resource, exactly like the OCR package, so the
# package stages only the manifest, the helper and the README.
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.hex-viewer
    PACKAGE_NAME "Hex View"
    COMMAND_NAME seer_hex.exe
    COMMAND_TARGET seer_hex
    EXTENSION_TOKEN "\${type_file}"
    TEST_TARGET hexviewer_manifest_test
    STAGE_TARGET hexviewer_manifest_stage
    PACKAGE_TARGET hexviewer_package
    STAGING_DIR ${HEX_PACKAGE_STAGING_DIR}
)

# The HexDump definition ships inside the package: Seer's text viewer claims
# *.hextxt through the syntax contribution mechanism only when the installed
# package root carries syntax/hextxt.xml, so the definition is part of the
# staged tree, the install tree and the manifest test's contract.
set(HEXVIEWER_STAGED_SYNTAX ${HEX_PACKAGE_STAGING_DIR}/syntax/hextxt.xml)
add_custom_command(
    OUTPUT ${HEXVIEWER_STAGED_SYNTAX}
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${HEX_PACKAGE_STAGING_DIR}/syntax
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/syntax/hextxt.xml
        ${HEXVIEWER_STAGED_SYNTAX}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/syntax/hextxt.xml
    VERBATIM
)
add_custom_target(hexviewer_syntax_stage DEPENDS ${HEXVIEWER_STAGED_SYNTAX})
add_dependencies(hexviewer_manifest_stage hexviewer_syntax_stage)
install(FILES syntax/hextxt.xml DESTINATION syntax)

add_executable(hexsweep_test test/hexsweep_test.cpp)
target_link_libraries(hexsweep_test PRIVATE hexviewer_core)
target_include_directories(hexsweep_test PRIVATE ${SEER_CONTROL_TEST_DIR})
add_test(NAME hexsweep_test COMMAND hexsweep_test)
