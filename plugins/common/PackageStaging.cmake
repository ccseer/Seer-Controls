# Shared staging and install-time contract validation for the native Control
# packages. Every package stages the same two files (plugin.json plus one
# helper executable) and wires the same ctest and package targets, so the
# machinery lives here and each package passes only its fixed identity.
#
# Usage in a package CMakeLists.txt (after the manifest test executable has
# been declared with its own includes and dependencies):
#
#   include(${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake)
#   seer_control_package_staging(
#       PACKAGE_ID io.1218.seer.terminal-here
#       PACKAGE_NAME "Terminal Here"
#       COMMAND_NAME terminal_here.exe
#       COMMAND_TARGET terminal_here
#       EXTENSION_TOKEN "\${type_folder}"
#       TEST_TARGET terminalhere_manifest_test
#       STAGE_TARGET terminalhere_manifest_stage
#       PACKAGE_TARGET terminalhere_package
#       STAGING_DIR ${TERMINAL_PACKAGE_STAGING_DIR})
#
# EXTENSION_TOKEN must be quoted with an escaped dollar sign so CMake passes
# the literal token through instead of expanding it as a variable at configure
# time; the same holds inside the generated install script, where the token is
# compared against the manifest's JSON value.

include_guard(GLOBAL)

function(seer_control_package_staging)
    set(oneValueArgs
        PACKAGE_ID
        PACKAGE_NAME
        COMMAND_NAME
        COMMAND_TARGET
        EXTENSION_TOKEN
        TEST_TARGET
        STAGE_TARGET
        PACKAGE_TARGET
        STAGING_DIR
    )
    cmake_parse_arguments(PKG "" "${oneValueArgs}" "" ${ARGN})

    foreach(_required
            PACKAGE_ID PACKAGE_NAME COMMAND_NAME COMMAND_TARGET
            EXTENSION_TOKEN TEST_TARGET STAGE_TARGET PACKAGE_TARGET
            STAGING_DIR)
        if(NOT PKG_${_required})
            message(FATAL_ERROR "seer_control_package_staging: missing ${_required}")
        endif()
    endforeach()

    add_custom_command(
        OUTPUT
            ${PKG_STAGING_DIR}/plugin.json
            ${PKG_STAGING_DIR}/${PKG_COMMAND_NAME}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_STAGING_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
            ${PKG_STAGING_DIR}/plugin.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PKG_COMMAND_TARGET}>
            ${PKG_STAGING_DIR}/${PKG_COMMAND_NAME}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json ${PKG_COMMAND_TARGET}
        VERBATIM
    )
    add_custom_target(${PKG_STAGE_TARGET}
        DEPENDS
            ${PKG_STAGING_DIR}/plugin.json
            ${PKG_STAGING_DIR}/${PKG_COMMAND_NAME}
    )

    add_dependencies(${PKG_TEST_TARGET} ${PKG_STAGE_TARGET})
    add_test(NAME ${PKG_TEST_TARGET}
        COMMAND ${PKG_TEST_TARGET} ${PKG_STAGING_DIR}
    )
    add_custom_target(${PKG_PACKAGE_TARGET}
        COMMAND $<TARGET_FILE:${PKG_TEST_TARGET}> ${PKG_STAGING_DIR}
        DEPENDS ${PKG_TEST_TARGET}
        VERBATIM
    )

    install(TARGETS ${PKG_COMMAND_TARGET} RUNTIME DESTINATION .)
    install(FILES plugin.json DESTINATION .)

    # Double-quoted string on purpose: ${PKG_*} expands at configure time and
    # every \${...} stays literal until the generated install script runs, so
    # the JSON check executes against the installed tree with the package's
    # fixed identity baked in.
    install(CODE "
        set(_package_root \"\${CMAKE_INSTALL_PREFIX}\")
        file(REAL_PATH \"\${_package_root}\" _package_root_real)
        set(_manifest \"\${_package_root_real}/plugin.json\")
        set(_helper \"\${_package_root_real}/${PKG_COMMAND_NAME}\")

        foreach(_required IN ITEMS \"\${_manifest}\" \"\${_helper}\")
            if(NOT EXISTS \"\${_required}\" OR IS_DIRECTORY \"\${_required}\")
                message(FATAL_ERROR \"${PKG_PACKAGE_NAME} package is missing \${_required}\")
            endif()
        endforeach()

        file(READ \"\${_manifest}\" _manifest_json)
        string(JSON _id GET \"\${_manifest_json}\" id)
        string(JSON _backend GET \"\${_manifest_json}\" backend)
        string(JSON _command GET \"\${_manifest_json}\" command)
        string(JSON _extensions_length LENGTH \"\${_manifest_json}\" extensions)
        string(JSON _capabilities_length LENGTH \"\${_manifest_json}\" capabilities)
        string(JSON _extension GET \"\${_manifest_json}\" extensions 0)
        string(JSON _capability GET \"\${_manifest_json}\" capabilities 0)
        string(JSON _timeout GET \"\${_manifest_json}\" timeout_ms)

        if(NOT _id STREQUAL \"${PKG_PACKAGE_ID}\"
           OR NOT _backend STREQUAL \"process\"
           OR NOT _command STREQUAL \"${PKG_COMMAND_NAME}\"
           OR NOT _extensions_length EQUAL 1
           OR NOT _extension STREQUAL \"\\${PKG_EXTENSION_TOKEN}\"
           OR NOT _capabilities_length EQUAL 1
           OR NOT _capability STREQUAL \"control\"
           OR _timeout LESS 1)
            message(FATAL_ERROR \"${PKG_PACKAGE_NAME} plugin.json does not match its fixed package contract\")
        endif()
    ")
endfunction()
