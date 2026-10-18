# ONNX Runtime C API header

`onnxruntime_c_api.h` is vendored verbatim from the ONNX Runtime 1.22.1 release:

- Source: <https://github.com/microsoft/onnxruntime/blob/v1.22.1/include/onnxruntime/core/session/onnxruntime_c_api.h>
- Version: 1.22.1 (`ORT_API_VERSION` = 22)
- License: MIT (copyright Microsoft Corporation; header text retained)

The header must match the `onnxruntime.dll` shipped under `backend/`. The plugin
loads the DLL dynamically and never links `onnxruntime.lib`; this header is a
compile-time-only dependency that defines the `OrtApi` table layout. Do not
hand-copy or reorder its members: any drift makes `GetApi()` return a table whose
slots no longer line up with the call sites.
