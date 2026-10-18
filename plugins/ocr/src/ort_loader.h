#pragma once

#include <windows.h>

#include <string>
#include <vector>
#include <memory>
#include "ort_types.h"

namespace seer_ocr {

class OrtManager;

class OrtSessionWrapper {
public:
    OrtSessionWrapper() = default;
    ~OrtSessionWrapper();

    OrtSessionWrapper(const OrtSessionWrapper &) = delete;
    OrtSessionWrapper &operator=(const OrtSessionWrapper &) = delete;
    OrtSessionWrapper(OrtSessionWrapper &&other) noexcept;
    OrtSessionWrapper &operator=(OrtSessionWrapper &&other) noexcept;

    void release();
    bool isValid() const { return m_session != nullptr; }

    bool run(
        const std::vector<const char *> &input_names,
        const std::vector<OrtValue *> &inputs,
        const std::vector<const char *> &output_names,
        std::vector<OrtValue *> &outputs,
        std::string &error_msg);

    const std::vector<std::string> &inputNames() const { return m_input_names; }
    const std::vector<std::string> &outputNames() const { return m_output_names; }

private:
    friend class OrtManager;
    OrtSession *m_session     = nullptr;
    const OrtApi *m_api       = nullptr;
    std::vector<std::string> m_input_names;
    std::vector<std::string> m_output_names;
};

class OrtManager {
public:
    static OrtManager &instance();

    bool init(const std::wstring &backend_dir, std::wstring &error_msg);
    bool isLoaded() const { return m_api != nullptr; }
    bool isDmlAvailable() const { return m_append_dml != nullptr; }

    const OrtApi *api() const { return m_api; }
    OrtEnv *env() const { return m_env; }
    OrtMemoryInfo *cpuMemoryInfo() const { return m_cpu_mem_info; }

    bool createSession(
        const std::wstring &model_path,
        bool use_dml,
        OrtSessionWrapper &out_session,
        std::string &out_log,
        bool force_dml_fail_init = false);

private:
    OrtManager() = default;
    ~OrtManager();

    HMODULE m_h_ort                                              = nullptr;
    const OrtApi *m_api                                          = nullptr;
    OrtEnv *m_env                                                = nullptr;
    OrtMemoryInfo *m_cpu_mem_info                                = nullptr;
    OrtSessionOptionsAppendExecutionProvider_DMLFn m_append_dml  = nullptr;
};

}  // namespace seer_ocr
