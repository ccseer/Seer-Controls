cmake_minimum_required(VERSION 3.21)
project(seer_ocr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The package must run on a machine that has Seer but not necessarily the VC++
# redistributable, so the runtime is linked statically.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Sources are UTF-8 without BOM
if(MSVC)
    # /EHsc is added explicitly: the worker uses try/catch and std::fstream, and
    # a configure that leaves CMAKE_CXX_FLAGS empty (Ninja + custom env) would
    # otherwise build without unwind semantics.
    add_compile_options(/utf-8 /EHsc)
endif()

include(CTest)
enable_testing()

set(SEER_OCR_SOURCES
    src/args.cpp
    src/paths.cpp
    src/launcher.cpp
    src/cleanup.cpp
    src/hash.cpp
    src/image_loader.cpp
    src/geometry.cpp
    src/barcode.cpp
    src/ort_loader.cpp
    src/ocr_pipeline.cpp
    src/output_writer.cpp
)

set(SEER_OCR_HEADERS
    src/args.h
    src/paths.h
    src/launcher.h
    src/cleanup.h
    src/hash.h
    src/image_loader.h
    src/geometry.h
    src/barcode.h
    src/ort_types.h
    src/ort_loader.h
    src/ocr_pipeline.h
    src/output_writer.h
    third_party/onnxruntime/onnxruntime_c_api.h
)

# Vendored ZXing-C++ (see third_party/zxing-cpp/VENDOR.md), built from source
# as a static library so the shipped package stays a single executable with
# no extra runtime DLLs. Readers only: the writer backends, examples, tests
# and language wrappers are all switched off.
# EXCLUDE_FROM_ALL keeps the vendored project's own install rules (ZXing.lib,
# include/ZXing, lib/cmake/ZXing) out of the package install prefix; ZXing is
# still built because ocr_core links it.
set(ZXING_EXAMPLES         OFF CACHE BOOL   "" FORCE)
set(ZXING_WRITERS          OFF CACHE STRING "" FORCE)
set(ZXING_C_API            OFF CACHE BOOL   "" FORCE)
set(ZXING_EXPERIMENTAL_API OFF CACHE BOOL   "" FORCE)
set(ZXING_BLACKBOX_TESTS   OFF CACHE BOOL   "" FORCE)
set(ZXING_UNIT_TESTS       OFF CACHE BOOL   "" FORCE)
set(ZXING_PYTHON_MODULE    OFF CACHE BOOL   "" FORCE)
add_subdirectory(third_party/zxing-cpp EXCLUDE_FROM_ALL)

add_compile_definitions(UNICODE _UNICODE NOMINMAX)

add_library(ocr_core STATIC ${SEER_OCR_SOURCES} ${SEER_OCR_HEADERS})
# The vendored header is the ABI authority for backend/onnxruntime.dll; it must
# be the same 1.22.1 revision as the DLL (see third_party/onnxruntime/README.md).
target_include_directories(ocr_core PUBLIC src third_party/onnxruntime)
# Third-party headers are not held to the /W4 /WX contract of package targets;
# the /external: family (VS 2019 16.10 and later) marks them as system headers,
# so their warnings cannot fail the build of the TUs that include them.
if(MSVC)
    target_compile_options(ocr_core PRIVATE
        /external:W0
        /external:I ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zxing-cpp/core/src
        /W4 /WX /utf-8 /EHsc)
endif()
target_link_libraries(ocr_core PUBLIC ZXing::ZXing windowscodecs ole32 shell32 user32 advapi32 bcrypt)

add_executable(SeerOcrWorker WIN32 src/main.cpp res/seer_ocr.rc)
target_link_libraries(SeerOcrWorker PRIVATE ocr_core)
if(MSVC)
    target_compile_options(SeerOcrWorker PRIVATE /W4 /WX /utf-8 /EHsc)
endif()

add_executable(ocr_test test/ocr_test.cpp)
target_include_directories(ocr_test PRIVATE src)
target_compile_definitions(ocr_test PRIVATE
    SEER_OCR_TEST_FIXTURES_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
target_link_libraries(ocr_test PRIVATE ocr_core)
if(MSVC)
    target_compile_options(ocr_test PRIVATE /W4 /WX /utf-8 /EHsc)
endif()

add_test(NAME ocr_test COMMAND ocr_test)

# Staging, ctest wiring, install rules and the install-time manifest contract
# are shared by every Control package; only the identity below is fixed here.
# OCR is the repository's only multi-type package, so it passes
# EXTENSION_TOKENS instead of the single EXTENSION_TOKEN.
set(SEER_CONTROL_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set(SEER_CONTROL_TEST_DIR ${SEER_CONTROL_COMMON_DIR}/test)

add_executable(ocr_manifest_test test/manifest_test.cpp)
target_include_directories(ocr_manifest_test PRIVATE ${SEER_CONTROL_TEST_DIR})
if(MSVC)
    target_compile_options(ocr_manifest_test PRIVATE /W4 /WX /utf-8 /EHsc)
endif()

set(OCR_PACKAGE_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/package-stage)
include(${SEER_CONTROL_COMMON_DIR}/PackageStaging.cmake)
seer_control_package_staging(
    PACKAGE_ID io.1218.seer.ocr
    PACKAGE_NAME "OCR Text"
    COMMAND_NAME SeerOcrWorker.exe
    COMMAND_TARGET SeerOcrWorker
    EXTENSION_TOKENS "\${type_pdf}" "\${type_image}"
    TEST_TARGET ocr_manifest_test
    STAGE_TARGET ocr_manifest_stage
    PACKAGE_TARGET ocr_package
    STAGING_DIR ${OCR_PACKAGE_STAGING_DIR}
)
