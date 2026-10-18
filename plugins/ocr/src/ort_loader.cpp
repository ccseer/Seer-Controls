#include "ort_loader.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace seer_ocr {

OrtSessionWrapper::OrtSessionWrapper(OrtSessionWrapper &&other) noexcept
{
    *this = std::move(other);
}

OrtSessionWrapper &OrtSessionWrapper::operator=(OrtSessionWrapper &&other) noexcept
{
    if (this != &other) {
        release();
        m_session      = other.m_session;
        m_api          = other.m_api;
        m_input_names  = std::move(other.m_input_names);
        m_output_names = std::move(other.m_output_names);
        other.m_session = nullptr;
        other.m_api     = nullptr;
    }
    return *this;
}

OrtSessionWrapper::~OrtSessionWrapper()
{
    release();
}

void OrtSessionWrapper::release()
{
    if (m_session && m_api) {
        m_api->ReleaseSession(m_session);
        m_session = nullptr;
    }
    m_input_names.clear();
    m_output_names.clear();
}

bool OrtSessionWrapper::run(
    const std::vector<const char *> &input_names,
    const std::vector<OrtValue *> &inputs,
    const std::vector<const char *> &output_names,
    std::vector<OrtValue *> &outputs,
    std::string &error_msg)
{
    if (!m_session || !m_api) {
        error_msg = "Session is not initialized";
        return false;
    }

    outputs.resize(output_names.size(), nullptr);

    OrtStatus *status = m_api->Run(
        m_session,
        nullptr,
        input_names.data(),
        inputs.data(),
        inputs.size(),
        output_names.data(),
        output_names.size(),
        outputs.data());

    if (status) {
        const char *msg = m_api->GetErrorMessage(status);
        error_msg = msg ? msg : "Unknown Ort run error";
        m_api->ReleaseStatus(status);
        for (auto *val : outputs) {
            if (val) m_api->ReleaseValue(val);
        }
        outputs.clear();
        return false;
    }

    return true;
}

OrtManager &OrtManager::instance()
{
    static OrtManager s_instance;
    return s_instance;
}

OrtManager::~OrtManager()
{
    if (m_cpu_mem_info && m_api) {
        m_api->ReleaseMemoryInfo(m_cpu_mem_info);
        m_cpu_mem_info = nullptr;
    }
    if (m_env && m_api) {
        m_api->ReleaseEnv(m_env);
        m_env = nullptr;
    }
    if (m_h_ort) {
        FreeLibrary(m_h_ort);
        m_h_ort = nullptr;
    }
}

bool OrtManager::init(const std::wstring &backend_dir, std::wstring &error_msg)
{
    if (m_api) {
        return true;
    }

    std::wstring dll_path = backend_dir + L"\\onnxruntime.dll";

    // Load with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so onnxruntime.dll can resolve DirectML.dll
    m_h_ort = LoadLibraryExW(
        dll_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!m_h_ort) {
        // Fallback standard load
        m_h_ort = LoadLibraryW(dll_path.c_str());
    }

    if (!m_h_ort) {
        DWORD err = GetLastError();
        error_msg = L"Failed to load onnxruntime.dll from " + dll_path + L" (Win32 error: " + std::to_wstring(err) + L")";
        return false;
    }

    auto get_api_base = reinterpret_cast<OrtGetApiBaseFn>(
        GetProcAddress(m_h_ort, "OrtGetApiBase"));

    if (!get_api_base) {
        error_msg = L"OrtGetApiBase not found in " + dll_path;
        return false;
    }

    const OrtApiBase *api_base = get_api_base();
    if (!api_base) {
        error_msg = L"Failed to get OrtApiBase";
        return false;
    }

    // Only the version the vendored header describes. GetApi(N) hands back a
    // table holding exactly N's members, so accepting a lower version would
    // return a shorter table that this translation unit still reads as the
    // ORT_API_VERSION layout: every call to a function added after N would
    // dispatch through a pointer read past the end of the array.
    m_api = api_base->GetApi(kOrtApiVersion);
    if (!m_api) {
        error_msg = L"onnxruntime.dll does not provide ORT API version "
                    + std::to_wstring(kOrtApiVersion)
                    + L". backend/ must hold the matching build (see "
                      L"third_party/onnxruntime/README.md).";
        return false;
    }

    // Look for DirectML extension provider entry point
    m_append_dml = reinterpret_cast<OrtSessionOptionsAppendExecutionProvider_DMLFn>(
        GetProcAddress(m_h_ort, "OrtSessionOptionsAppendExecutionProvider_DML"));

    OrtStatus *status = m_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "SeerOcr", &m_env);
    if (status) {
        const char *msg = m_api->GetErrorMessage(status);
        error_msg = msg ? std::wstring(msg, msg + strlen(msg)) : L"Failed to create ORT Env";
        m_api->ReleaseStatus(status);
        return false;
    }

    status = m_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_cpu_mem_info);
    if (status) {
        const char *msg = m_api->GetErrorMessage(status);
        error_msg = msg ? std::wstring(msg, msg + strlen(msg)) : L"Failed to create CPU memory info";
        m_api->ReleaseStatus(status);
        return false;
    }

    return true;
}

bool OrtManager::createSession(
    const std::wstring &model_path,
    bool use_dml,
    OrtSessionWrapper &out_session,
    std::string &out_log,
    bool force_dml_fail_init)
{
    out_session.release();
    out_session.m_api = m_api;

    if (!m_api || !m_env) {
        out_log += "OrtManager not initialized\n";
        return false;
    }

    OrtSessionOptions *options = nullptr;
    OrtStatus *status = m_api->CreateSessionOptions(&options);
    if (status || !options) {
        if (status) m_api->ReleaseStatus(status);
        out_log += "Failed to create session options\n";
        return false;
    }

    if (use_dml) {
        if (force_dml_fail_init) {
            m_api->ReleaseSessionOptions(options);
            out_log += "Fault injection: force_dml_fail_init simulated\n";
            return false;
        }

        if (!m_append_dml) {
            m_api->ReleaseSessionOptions(options);
            out_log += "DirectML provider symbol not present in onnxruntime.dll\n";
            return false;
        }

        // DirectML specific constraints per specification
        m_api->DisableMemPattern(options);
        m_api->SetSessionExecutionMode(options, ORT_SEQUENTIAL);

        status = m_append_dml(options, 0);
        if (status) {
            const char *msg = m_api->GetErrorMessage(status);
            out_log += msg ? ("DML provider init failed: " + std::string(msg) + "\n") : "DML provider init failed\n";
            m_api->ReleaseStatus(status);
            m_api->ReleaseSessionOptions(options);
            return false;
        }
    }
    else {
        // CPU baseline configuration
        unsigned int hc = std::thread::hardware_concurrency();
        int threads = std::min(4, static_cast<int>(hc > 0 ? hc : 2));
        m_api->SetIntraOpNumThreads(options, threads);
    }

    status = m_api->CreateSession(m_env, model_path.c_str(), options, &out_session.m_session);
    m_api->ReleaseSessionOptions(options);

    if (status || !out_session.m_session) {
        const char *msg = status ? m_api->GetErrorMessage(status) : "Unknown session creation error";
        out_log += msg ? (std::string(msg) + "\n") : "Session creation failed\n";
        if (status) m_api->ReleaseStatus(status);
        return false;
    }

    // Retrieve input & output names. Every Session* call owns its OrtStatus on
    // failure, so a missing name must be released here instead of discarded.
    OrtAllocator *allocator = nullptr;
    OrtStatus *st = m_api->GetAllocatorWithDefaultOptions(&allocator);
    if (st || !allocator) {
        if (st) m_api->ReleaseStatus(st);
        return true;
    }

    size_t in_count = 0;
    st = m_api->SessionGetInputCount(out_session.m_session, &in_count);
    if (st) {
        m_api->ReleaseStatus(st);
        in_count = 0;
    }
    out_session.m_input_names.reserve(in_count);
    for (size_t i = 0; i < in_count; ++i) {
        char *name = nullptr;
        st = m_api->SessionGetInputName(out_session.m_session, i, allocator, &name);
        if (st) {
            m_api->ReleaseStatus(st);
            continue;
        }
        if (name) {
            out_session.m_input_names.emplace_back(name);
            OrtStatus *free_st = m_api->AllocatorFree(allocator, name);
            if (free_st) m_api->ReleaseStatus(free_st);
        }
    }

    size_t out_count = 0;
    st = m_api->SessionGetOutputCount(out_session.m_session, &out_count);
    if (st) {
        m_api->ReleaseStatus(st);
        out_count = 0;
    }
    out_session.m_output_names.reserve(out_count);
    for (size_t i = 0; i < out_count; ++i) {
        char *name = nullptr;
        st = m_api->SessionGetOutputName(out_session.m_session, i, allocator, &name);
        if (st) {
            m_api->ReleaseStatus(st);
            continue;
        }
        if (name) {
            out_session.m_output_names.emplace_back(name);
            OrtStatus *free_st = m_api->AllocatorFree(allocator, name);
            if (free_st) m_api->ReleaseStatus(free_st);
        }
    }

    return true;
}

}  // namespace seer_ocr
