#pragma once

// The OrtApi function table is an ABI surface: it must come from the vendor
// header that matches backend/onnxruntime.dll. A hand-written copy used to live
// here, its member order drifted from the real layout, and every call after
// GetErrorMessage dispatched to the wrong slot.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <onnxruntime_c_api.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace seer_ocr {

// Address of the exported OrtGetApiBase() entry point, resolved with
// GetProcAddress because the DLL is loaded at runtime.
typedef const OrtApiBase *(ORT_API_CALL *OrtGetApiBaseFn)(void);

// Declared in dml_provider_factory.h; resolved with GetProcAddress because the
// provider symbol is exported by onnxruntime.dll but not by any import library
// the plugin links.
typedef OrtStatus *(ORT_API_CALL *OrtSessionOptionsAppendExecutionProvider_DMLFn)(
    OrtSessionOptions *options, int device_id);

inline constexpr uint32_t kOrtApiVersion = ORT_API_VERSION;

}  // namespace seer_ocr
