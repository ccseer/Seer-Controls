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
    src/ort_types.h
    src/ort_loader.h
    src/ocr_pipeline.h
    src/output_writer.h
    third_party/onnxruntime/onnxruntime_c_api.h
)

add_compile_definitions(UNICODE _UNICODE NOMINMAX)

add_library(ocr_core STATIC ${SEER_OCR_SOURCES} ${SEER_OCR_HEADERS})
# The vendored header is the ABI authority for backend/onnxruntime.dll; it must
# be the same 1.22.1 revision as the DLL (see third_party/onnxruntime/README.md).
target_include_directories(ocr_core PUBLIC src third_party/onnxruntime)
if(MSVC)
    target_compile_options(ocr_core PRIVATE /W4 /WX /utf-8 /EHsc)
endif()
target_link_libraries(ocr_core PUBLIC windowscodecs ole32 shell32 user32 advapi32 bcrypt)

add_executable(SeerOcrWorker WIN32 src/main.cpp)
target_link_libraries(SeerOcrWorker PRIVATE ocr_core)
if(MSVC)
    target_compile_options(SeerOcrWorker PRIVATE /W4 /WX /utf-8 /EHsc)
endif()

add_executable(ocr_test test/ocr_test.cpp)
target_include_directories(ocr_test PRIVATE src)
target_link_libraries(ocr_test PRIVATE ocr_core)
if(MSVC)
    target_compile_options(ocr_test PRIVATE /W4 /WX /utf-8 /EHsc)
endif()

add_test(NAME ocr_test COMMAND ocr_test)
