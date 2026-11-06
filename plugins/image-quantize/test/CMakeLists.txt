add_executable(quantize_test quantize_test.cpp check.h)
target_link_libraries(quantize_test PRIVATE quantize_core)
target_include_directories(quantize_test PRIVATE "${SQ_ENGINE_DIR}")
sq_apply_package_flags(quantize_test)
sq_stage_asan_runtime(quantize_test)
add_test(NAME quantize_test COMMAND quantize_test)

add_executable(quantize_abi_test quantize_abi_test.c)
target_link_libraries(quantize_abi_test PRIVATE quantize_core)
if(MSVC)
    target_compile_options(quantize_abi_test PRIVATE /W4 /WX /utf-8)
endif()
sq_stage_asan_runtime(quantize_abi_test)
add_test(NAME quantize_abi_test COMMAND quantize_abi_test)

add_executable(runner_test runner_test.cpp check.h)
target_link_libraries(runner_test PRIVATE quantize_core quantize_lodepng_tools)
target_include_directories(runner_test PRIVATE
    "${SQ_ENGINE_DIR}"
    "${SQ_ENGINE_DIR}/include"
    "${SQ_TOOLS_THIRDPARTY_DIR}"
)
sq_apply_package_flags(runner_test)
sq_stage_asan_runtime(runner_test)
add_test(NAME runner_test
    COMMAND runner_test $<TARGET_FILE:image_quantize> "${SQ_ROOT}/test/data/generated")

add_executable(deflate_test deflate_test.cpp "${SQ_ROOT}/src/helper/sq_deflate.cpp")
target_link_libraries(deflate_test PRIVATE quantize_lodepng_tools)
target_include_directories(deflate_test PRIVATE
    "${SQ_ROOT}/src/helper"
    "${SQ_TOOLS_THIRDPARTY_DIR}"
)
sq_apply_package_flags(deflate_test)
sq_stage_asan_runtime(deflate_test)
add_test(NAME deflate_test COMMAND deflate_test)

add_executable(image_quantize_manifest_test manifest_test.cpp)
target_include_directories(image_quantize_manifest_test PRIVATE
    "${SEER_CONTROL_COMMON_DIR}/test")
sq_apply_package_flags(image_quantize_manifest_test)
sq_stage_asan_runtime(image_quantize_manifest_test)
