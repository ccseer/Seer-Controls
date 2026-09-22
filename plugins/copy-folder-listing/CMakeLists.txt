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

set(LISTING_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
add_custom_command(
    OUTPUT
        ${LISTING_PACKAGE_STAGING_DIR}/plugin.json
        ${LISTING_PACKAGE_STAGING_DIR}/copy_folder_listing.exe
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LISTING_PACKAGE_STAGING_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
        ${LISTING_PACKAGE_STAGING_DIR}/plugin.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:copy_folder_listing>
        ${LISTING_PACKAGE_STAGING_DIR}/copy_folder_listing.exe
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json copy_folder_listing
    VERBATIM
)
add_custom_target(folderlisting_manifest_stage
    DEPENDS
        ${LISTING_PACKAGE_STAGING_DIR}/plugin.json
        ${LISTING_PACKAGE_STAGING_DIR}/copy_folder_listing.exe
)

add_executable(folderlisting_manifest_test test/manifest_test.cpp)
add_dependencies(folderlisting_manifest_test folderlisting_manifest_stage)
target_include_directories(folderlisting_manifest_test PRIVATE ${SEER_CONTROL_TEST_DIR})
if(MSVC)
    target_compile_options(folderlisting_manifest_test PRIVATE /W4 /WX)
endif()
add_test(NAME folderlisting_manifest_test
    COMMAND folderlisting_manifest_test ${LISTING_PACKAGE_STAGING_DIR}
)
add_custom_target(folderlisting_package
    COMMAND $<TARGET_FILE:folderlisting_manifest_test> ${LISTING_PACKAGE_STAGING_DIR}
    DEPENDS folderlisting_manifest_test
    VERBATIM
)

install(TARGETS copy_folder_listing RUNTIME DESTINATION .)
install(FILES plugin.json DESTINATION .)
# The field-level package contract is owned by folderlisting_manifest_test,
# which reads the staged manifest the way the host loader does; the install
# step only proves the shipped files are present.
install(CODE [=[
    set(_package_root "${CMAKE_INSTALL_PREFIX}")
    file(REAL_PATH "${_package_root}" _package_root_real)
    set(_manifest "${_package_root_real}/plugin.json")
    set(_helper "${_package_root_real}/copy_folder_listing.exe")

    foreach(_required IN ITEMS "${_manifest}" "${_helper}")
        if(NOT EXISTS "${_required}" OR IS_DIRECTORY "${_required}")
            message(FATAL_ERROR "Copy Folder Listing package is missing ${_required}")
        endif()
    endforeach()
]=])
