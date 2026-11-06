# Vendored lodepng is compiled in its own /W0 target so /W4 /WX never applies
# to its translation unit. Mixing /W4 and /W0 on one command line would itself
# produce D9025, which /WX would turn into an error.
add_library(quantize_lodepng_tools STATIC
    thirdparty/lodepng.cpp
    thirdparty/lodepng.h
)
target_include_directories(quantize_lodepng_tools PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
if(MSVC)
    target_compile_options(quantize_lodepng_tools PRIVATE /W0 /utf-8 /EHsc /fp:precise)
endif()

add_library(quantize_imageio STATIC
    common/imageio.cpp
    common/imageio.h
    common/metrics.cpp
    common/metrics.h
    common/sha256.cpp
    common/sha256.h
)
target_include_directories(quantize_imageio PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty"
    "${SQ_ENGINE_DIR}"
)
if(MSVC)
    # stb_image.h and lodepng.h are included by our translation units, so mark
    # the vendored directory external to keep their warnings out of /W4 /WX.
    target_compile_options(quantize_imageio PRIVATE
        /W4 /WX /utf-8 /EHsc /fp:precise
        /external:W0
        /external:I "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")
endif()
target_link_libraries(quantize_imageio PUBLIC quantize_core quantize_lodepng_tools)

add_executable(quantize_metrics metrics/quantize_metrics.cpp)
target_link_libraries(quantize_metrics PRIVATE quantize_imageio)
sq_apply_package_flags(quantize_metrics)
sq_stage_asan_runtime(quantize_metrics)
add_test(NAME quantize_metrics_selfcheck COMMAND quantize_metrics --selfcheck)

# Reference quantizer: per-pixel Lloyd k-means, used only to measure the gap
# against the histogram objective. It is never linked into the engine or the
# package.
add_executable(quantize_reference reference/quantize_reference.cpp)
target_link_libraries(quantize_reference PRIVATE quantize_imageio)
sq_apply_package_flags(quantize_reference)
sq_stage_asan_runtime(quantize_reference)

# Recorded alpha experiment over an exact RGBA table. It shares only the
# harness I/O and metrics code with the engine; the clustering here is not
# linked into the shipped quantizer.
add_executable(quantize_v4d_experiment reference/quantize_v4d_experiment.cpp)
target_link_libraries(quantize_v4d_experiment PRIVATE quantize_imageio)
sq_apply_package_flags(quantize_v4d_experiment)
sq_stage_asan_runtime(quantize_v4d_experiment)

# Manual full-enumeration round trip for 5.6.1. It is deliberately not a
# CTest entry: the phase report runs it once and records the measurement.
add_executable(quantize_oklab_roundtrip reference/oklab_roundtrip.cpp)
target_include_directories(quantize_oklab_roundtrip PRIVATE "${SQ_ENGINE_DIR}")
sq_apply_package_flags(quantize_oklab_roundtrip)
sq_stage_asan_runtime(quantize_oklab_roundtrip)

add_executable(quantize_bench bench/quantize_bench.cpp)
target_link_libraries(quantize_bench PRIVATE quantize_imageio psapi)
sq_apply_package_flags(quantize_bench)
sq_stage_asan_runtime(quantize_bench)
add_test(NAME quantize_bench_stub COMMAND quantize_bench --selfcheck)

# The PoC uses the same vendored lodepng as the harnesses; the target's PUBLIC
# include directory carries the header, so no separate include path is needed.
add_executable(quantize_wic_poc wicpoc/wic_poc.cpp)
target_link_libraries(quantize_wic_poc PRIVATE quantize_lodepng_tools windowscodecs ole32)
sq_apply_package_flags(quantize_wic_poc)
sq_stage_asan_runtime(quantize_wic_poc)
add_test(NAME quantize_wic_poc COMMAND quantize_wic_poc --selfcheck)

add_executable(quantize_wic_depth wicpoc/wic_depth_probe.cpp)
target_link_libraries(quantize_wic_depth PRIVATE windowscodecs ole32)
sq_apply_package_flags(quantize_wic_depth)
sq_stage_asan_runtime(quantize_wic_depth)
add_test(NAME quantize_wic_depth_probe COMMAND quantize_wic_depth --selfcheck)

# The size step of the WIC PoC needs the pinned baseline binary and one real
# fixture, both of which are external inputs. The test is registered only when
# both cache variables are supplied, so a plain package build still runs the
# external-free suite.
set(SQ_BASELINE_EXE "" CACHE FILEPATH
    "Pinned baseline executable for the WIC PoC size step")
set(SQ_WIC_SIZE_IMAGE "" CACHE FILEPATH
    "Fixture used by the WIC PoC size step")
if(SQ_BASELINE_EXE AND SQ_WIC_SIZE_IMAGE)
    add_test(NAME quantize_wic_size
             COMMAND quantize_wic_poc --selfcheck
                     --baseline "${SQ_BASELINE_EXE}"
                     --image "${SQ_WIC_SIZE_IMAGE}"
                     --work-dir "${CMAKE_BINARY_DIR}/wic-size")
endif()
