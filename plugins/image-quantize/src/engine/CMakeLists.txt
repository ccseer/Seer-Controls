add_library(quantize_core STATIC
    include/seer_quantize.h
    sq_color.h
    sq_internal.h
    sq_api.cpp
    sq_workspace.cpp
    sq_scan.cpp
    sq_palette.cpp
    sq_wu.cpp
    sq_kmeans.cpp
    sq_kmeans.h
    sq_dither.cpp
    sq_dither.h
)

# The engine sees only its own directory plus the C++ standard library.
target_include_directories(quantize_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(quantize_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

sq_apply_package_flags(quantize_core)
