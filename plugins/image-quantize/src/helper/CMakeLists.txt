add_executable(image_quantize WIN32
    main.cpp
    quantizerunner.cpp
    quantizerunner.h
    sq_deflate.cpp
    sq_deflate.h
    "${SQ_ROOT}/res/seer_iq.rc"
)
# The .rc embeds seer_iq.ico, which CMake cannot infer from the script text;
# without this an edited icon would keep the previous resource in an
# incremental build.
set_source_files_properties("${SQ_ROOT}/res/seer_iq.rc" PROPERTIES
    OBJECT_DEPENDS "${SQ_ROOT}/res/seer_iq.ico")

target_link_libraries(image_quantize PRIVATE quantize_core quantize_lodepng_tools windowscodecs ole32 shell32)
target_include_directories(image_quantize PRIVATE
    "${SQ_ROOT}/src/engine/include"
    "${SEER_CONTROL_COMMON_DIR}"
)

sq_apply_package_flags(image_quantize)
sq_stage_asan_runtime(image_quantize)
