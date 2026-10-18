#pragma once

#include <string>
#include <vector>
#include "image_loader.h"
#include "geometry.h"
#include "ort_loader.h"
#include "args.h"

namespace seer_ocr {

struct OcrModelPaths {
    std::wstring det_model_path;
    std::wstring ori_model_path;
    std::wstring rec_model_path;
    std::wstring dict_path;
};

class OcrPipeline {
public:
    OcrPipeline() = default;
    ~OcrPipeline() = default;

    // Initializes models and dictionary
    bool init(
        const OcrModelPaths &paths,
        ExecutionProvider ep,
        std::string &out_log,
        bool force_dml_fail_init = false);

    // Runs full OCR on the input image
    bool processImage(
        const ImageRgb &image,
        bool enable_cls,
        int max_side,
        std::vector<OcrLineResult> &out_lines,
        std::string &ep_used,
        std::string &fallback_reason,
        std::string &out_log,
        bool force_dml_fail_run = false);

private:
    bool loadDictionary(const std::wstring &dict_path);

    bool runPipelineOnce(
        const ImageRgb &image,
        bool enable_cls,
        int max_side,
        std::vector<OcrLineResult> &out_lines,
        std::string &err,
        bool &run_error,
        bool force_dml_fail_run);

    bool runDetection(
        const ImageRgb &image,
        int max_side,
        std::vector<QuadBox> &out_boxes,
        std::string &err,
        bool &run_error);

    bool runOrientation(
        const ImageRgb &line_crop,
        int &out_orientation, // 0 or 180
        std::string &err);

    bool runRecognition(
        const ImageRgb &line_crop,
        std::string &out_text,
        float &out_score,
        std::string &err,
        bool &run_error);

    OcrModelPaths m_paths;
    ExecutionProvider m_target_ep = ExecutionProvider::Auto;
    bool m_using_dml              = false;

    // Set when init() had to abandon DirectML; surfaced through
    // processImage()'s fallback_reason so the JSON records it too.
    std::string m_init_fallback_reason;

    OrtSessionWrapper m_det_session;
    OrtSessionWrapper m_ori_session;
    OrtSessionWrapper m_rec_session;

    std::vector<std::string> m_dictionary;
};

}  // namespace seer_ocr
