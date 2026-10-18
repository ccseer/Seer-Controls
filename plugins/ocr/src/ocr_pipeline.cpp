#include "ocr_pipeline.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <sstream>

namespace seer_ocr {

namespace {

// Bucketing levels for Rec width
const int kRecWidthBuckets[] = {160, 320, 480, 640, 960, 1280, 1920, 2560, 3200};

// PP-OCRv5 ships 18,383 characters; a truncated list still decodes, but every
// index past its end is dropped and the output is silently wrong.
constexpr size_t kMinDictEntries = 18000;

std::string toUtf8(const std::wstring &wstr)
{
    if (wstr.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

float softmaxDenominator(const float *values, int64_t count, float max_value)
{
    float sum = 0.0f;
    for (int64_t i = 0; i < count; ++i) {
        sum += std::exp(values[i] - max_value);
    }
    return sum > 0.0f ? sum : 1.0f;
}

bool fileExists(const std::wstring &path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void resizeBilinear(
    const uint8_t *src, int src_w, int src_h,
    float *dst_chw, int dst_w, int dst_h,
    const float mean[3], const float std[3],
    int pad_w, int pad_h)
{
    // dst_chw is [3, pad_h, pad_w], zero-initialized
    for (int y = 0; y < dst_h; ++y) {
        float src_y = (y + 0.5f) * src_h / dst_h - 0.5f;
        int y0 = std::max(0, std::min(src_h - 1, static_cast<int>(std::floor(src_y))));
        int y1 = std::max(0, std::min(src_h - 1, y0 + 1));
        float fy = src_y - y0;

        for (int x = 0; x < dst_w; ++x) {
            float src_x = (x + 0.5f) * src_w / dst_w - 0.5f;
            int x0 = std::max(0, std::min(src_w - 1, static_cast<int>(std::floor(src_x))));
            int x1 = std::max(0, std::min(src_w - 1, x0 + 1));
            float fx = src_x - x0;

            const uint8_t *p00 = src + (y0 * src_w + x0) * 3;
            const uint8_t *p10 = src + (y0 * src_w + x1) * 3;
            const uint8_t *p01 = src + (y1 * src_w + x0) * 3;
            const uint8_t *p11 = src + (y1 * src_w + x1) * 3;

            for (int c = 0; c < 3; ++c) {
                float v00 = p00[c] / 255.0f;
                float v10 = p10[c] / 255.0f;
                float v01 = p01[c] / 255.0f;
                float v11 = p11[c] / 255.0f;

                float val = (1.0f - fx) * (1.0f - fy) * v00
                          + fx * (1.0f - fy) * v10
                          + (1.0f - fx) * fy * v01
                          + fx * fy * v11;

                val = (val - mean[c]) / std[c];
                dst_chw[c * (pad_h * pad_w) + y * pad_w + x] = val;
            }
        }
    }
}

}  // namespace

bool OcrPipeline::loadDictionary(const std::wstring &dict_path)
{
    m_dictionary.clear();
    m_dictionary.push_back(""); // Index 0 is blank for CTC

    std::ifstream file(dict_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    size_t entries = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        m_dictionary.push_back(line);
        ++entries;
    }
    m_dictionary.push_back(" "); // space character token at end

    return entries >= kMinDictEntries;
}

bool OcrPipeline::init(
    const OcrModelPaths &paths,
    ExecutionProvider ep,
    std::string &out_log,
    bool force_dml_fail_init)
{
    m_paths = paths;
    m_target_ep = ep;

    if (!loadDictionary(paths.dict_path)) {
        out_log = "Dictionary missing, unreadable, or truncated (expected the PP-OCRv5 "
                  "18,383-entry list, need at least "
                  + std::to_string(kMinDictEntries) + " entries): " + toUtf8(paths.dict_path);
        return false;
    }

    // A missing model is a packaging fault, not a GPU fault: check it before any
    // session exists so the DirectML path is not blamed for it.
    const std::wstring *required_models[] = {
        &paths.det_model_path, &paths.rec_model_path, &paths.ori_model_path};
    for (const auto *model : required_models) {
        if (!fileExists(*model)) {
            out_log = "Model file not found: " + toUtf8(*model);
            return false;
        }
    }

    m_using_dml = (ep == ExecutionProvider::Dml || ep == ExecutionProvider::Auto);

    bool ok = false;
    if (m_using_dml) {
        std::string log;
        ok = OrtManager::instance().createSession(
            paths.det_model_path,
            true,
            m_det_session,
            log,
            force_dml_fail_init);

        if (!ok) {
            std::string trimmed = log;
            while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
                trimmed.pop_back();
            }
            out_log += "DML det init failed: " + log + "\n";
            if (ep == ExecutionProvider::Auto) {
                out_log += "Falling back to CPU session for Det\n";
                m_init_fallback_reason = "DML det session init failed: " + trimmed
                                         + "; Det and Rec run on CPU";
                m_using_dml            = false;
                ok = OrtManager::instance().createSession(
                    paths.det_model_path,
                    false,
                    m_det_session,
                    out_log);
            }
        }
    }
    else {
        ok = OrtManager::instance().createSession(
            paths.det_model_path,
            false,
            m_det_session,
            out_log);
    }

    if (!ok) {
        return false;
    }

    // Ori model always runs on CPU per spec
    std::string ori_log;
    OrtManager::instance().createSession(
        paths.ori_model_path,
        false,
        m_ori_session,
        ori_log);

    // Rec model session
    ok = OrtManager::instance().createSession(
        paths.rec_model_path,
        m_using_dml,
        m_rec_session,
        out_log);

    if (!ok && m_using_dml && ep == ExecutionProvider::Auto) {
        out_log += "DML rec init failed, falling back all models to CPU\n";
        m_init_fallback_reason = "DML rec session init failed; Det and Rec rebuilt on CPU";
        m_using_dml = false;
        OrtManager::instance().createSession(paths.det_model_path, false, m_det_session, out_log);
        ok = OrtManager::instance().createSession(paths.rec_model_path, false, m_rec_session, out_log);
    }

    return ok;
}

bool OcrPipeline::runDetection(
    const ImageRgb &image,
    int max_side,
    std::vector<QuadBox> &out_boxes,
    std::string &err,
    bool &run_error)
{
    out_boxes.clear();
    if (image.empty() || !m_det_session.isValid()) {
        err = "Invalid image or detection session";
        return false;
    }

    int src_w = image.width;
    int src_h = image.height;
    int max_dim = std::max(src_w, src_h);
    int target_long = std::min(max_side, 960);

    float scale = (max_dim > target_long) ? (static_cast<float>(target_long) / max_dim) : 1.0f;
    int dst_w = static_cast<int>(std::round(src_w * scale));
    int dst_h = static_cast<int>(std::round(src_h * scale));

    // Round up to multiple of 64
    int pad_w = ((dst_w + 63) / 64) * 64;
    int pad_h = ((dst_h + 63) / 64) * 64;

    std::vector<float> input_tensor(3 * pad_h * pad_w, 0.0f);
    const float kMean[3] = {0.485f, 0.456f, 0.406f};
    const float kStd[3]  = {0.229f, 0.224f, 0.225f};

    resizeBilinear(
        image.data.data(), src_w, src_h,
        input_tensor.data(), dst_w, dst_h,
        kMean, kStd,
        pad_w, pad_h);

    const int64_t input_shape[4] = {1, 3, pad_h, pad_w};
    OrtValue *in_val = nullptr;
    const OrtApi *api = OrtManager::instance().api();

    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
        OrtManager::instance().cpuMemoryInfo(),
        input_tensor.data(),
        input_tensor.size() * sizeof(float),
        input_shape,
        4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &in_val);

    if (st || !in_val) {
        if (st) api->ReleaseStatus(st);
        err = "Failed to create Det input tensor";
        return false;
    }

    std::vector<const char *> in_names = {"x"};
    if (!m_det_session.inputNames().empty()) {
        in_names[0] = m_det_session.inputNames()[0].c_str();
    }
    std::vector<const char *> out_names = {"sigmoid_0.tmp_0"};
    if (!m_det_session.outputNames().empty()) {
        out_names[0] = m_det_session.outputNames()[0].c_str();
    }

    std::vector<OrtValue *> out_vals;
    bool ok = m_det_session.run(in_names, {in_val}, out_names, out_vals, err);
    api->ReleaseValue(in_val);

    if (!ok) {
        run_error = true;
        return false;
    }
    if (out_vals.empty() || !out_vals[0]) {
        err = "Detection produced no output tensor";
        return false;
    }

    // The map is read as [y * pad_w + x] below, so the tensor must actually
    // cover that grid; a det model with a different output stride would be
    // read out of bounds.
    OrtTensorTypeAndShapeInfo *map_info = nullptr;
    OrtStatus *shape_st = api->GetTensorTypeAndShape(out_vals[0], &map_info);
    size_t map_dim_count = 0;
    std::vector<int64_t> map_dims;
    if (!shape_st && map_info) {
        OrtStatus *dims_st = api->GetDimensionsCount(map_info, &map_dim_count);
        if (dims_st) {
            api->ReleaseStatus(dims_st);
            map_dim_count = 0;
        }
        else {
            map_dims.resize(map_dim_count, 0);
            dims_st = api->GetDimensions(map_info, map_dims.data(), map_dim_count);
            if (dims_st) {
                api->ReleaseStatus(dims_st);
                map_dim_count = 0;
            }
        }
    }
    if (shape_st) api->ReleaseStatus(shape_st);
    if (map_info) api->ReleaseTensorTypeAndShapeInfo(map_info);

    if (map_dim_count != 4 || map_dims[2] < dst_h || map_dims[3] < pad_w) {
        api->ReleaseValue(out_vals[0]);
        err = "Unexpected detection output shape (expected [1, 1, H>=" + std::to_string(dst_h)
              + ", W>=" + std::to_string(pad_w) + "])";
        return false;
    }

    float *prob_map = nullptr;
    OrtStatus *data_st = api->GetTensorMutableData(out_vals[0], reinterpret_cast<void **>(&prob_map));
    if (data_st || !prob_map) {
        if (data_st) api->ReleaseStatus(data_st);
        api->ReleaseValue(out_vals[0]);
        err = "Failed to read the detection probability map";
        return false;
    }

    // Simple DB post-processing: extract contours from thresholded probability map
    const float kThresh = 0.3f;
    const float kBoxThresh = 0.6f;
    std::vector<uint8_t> mask(dst_w * dst_h, 0);

    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            float p = prob_map[y * pad_w + x];
            if (p >= kThresh) {
                mask[y * dst_w + x] = 255;
            }
        }
    }

    // Connected components extraction
    std::vector<int> labels(dst_w * dst_h, 0);
    int current_label = 0;
    std::vector<std::vector<PointF>> contours;

    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            if (mask[y * dst_w + x] == 255 && labels[y * dst_w + x] == 0) {
                ++current_label;
                std::vector<PointF> component_pts;
                std::queue<std::pair<int, int>> q;
                q.push({x, y});
                labels[y * dst_w + x] = current_label;

                while (!q.empty()) {
                    auto [cx, cy] = q.front();
                    q.pop();

                    // Check if boundary pixel
                    bool is_boundary = (cx == 0 || cy == 0 || cx == dst_w - 1 || cy == dst_h - 1);
                    const int dxs[] = {1, -1, 0, 0};
                    const int dys[] = {0, 0, 1, -1};

                    for (int k = 0; k < 4; ++k) {
                        int nx = cx + dxs[k];
                        int ny = cy + dys[k];
                        if (nx >= 0 && nx < dst_w && ny >= 0 && ny < dst_h) {
                            if (mask[ny * dst_w + nx] == 255) {
                                if (labels[ny * dst_w + nx] == 0) {
                                    labels[ny * dst_w + nx] = current_label;
                                    q.push({nx, ny});
                                }
                            }
                            else {
                                is_boundary = true;
                            }
                        }
                    }

                    if (is_boundary) {
                        component_pts.push_back({static_cast<float>(cx), static_cast<float>(cy)});
                    }
                }

                if (component_pts.size() >= 4) {
                    // Check average score
                    float score_sum = 0.0f;
                    for (const auto &pt : component_pts) {
                        score_sum += prob_map[static_cast<int>(pt.y) * pad_w + static_cast<int>(pt.x)];
                    }
                    float mean_score = score_sum / component_pts.size();

                    if (mean_score >= kBoxThresh) {
                        QuadBox qbox;
                        if (unclipPolygon(component_pts, 1.5f, qbox)) {
                            // Scale coordinates back to original image
                            for (int k = 0; k < 4; ++k) {
                                qbox.points[k].x = std::max(0.0f, std::min(static_cast<float>(src_w), qbox.points[k].x / scale));
                                qbox.points[k].y = std::max(0.0f, std::min(static_cast<float>(src_h), qbox.points[k].y / scale));
                            }
                            qbox.score = mean_score;
                            out_boxes.push_back(qbox);
                        }
                    }
                }
            }
        }
    }

    api->ReleaseValue(out_vals[0]);
    return true;
}

bool OcrPipeline::runOrientation(
    const ImageRgb &line_crop,
    int &out_orientation,
    std::string &err)
{
    out_orientation = 0;
    if (line_crop.empty() || !m_ori_session.isValid()) {
        return true; // Safe default
    }

    const int kOriH = 48;
    const int kOriW = 192;
    std::vector<float> input_tensor(3 * kOriH * kOriW, 0.0f);
    const float kMean[3] = {0.5f, 0.5f, 0.5f};
    const float kStd[3]  = {0.5f, 0.5f, 0.5f};

    resizeBilinear(
        line_crop.data.data(), line_crop.width, line_crop.height,
        input_tensor.data(), kOriW, kOriH,
        kMean, kStd,
        kOriW, kOriH);

    const int64_t input_shape[4] = {1, 3, kOriH, kOriW};
    OrtValue *in_val = nullptr;
    const OrtApi *api = OrtManager::instance().api();

    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
        OrtManager::instance().cpuMemoryInfo(),
        input_tensor.data(),
        input_tensor.size() * sizeof(float),
        input_shape,
        4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &in_val);

    if (st || !in_val) {
        if (st) api->ReleaseStatus(st);
        return false;
    }

    std::vector<const char *> in_names = {"x"};
    if (!m_ori_session.inputNames().empty()) in_names[0] = m_ori_session.inputNames()[0].c_str();
    std::vector<const char *> out_names = {"softmax_0.tmp_0"};
    if (!m_ori_session.outputNames().empty()) out_names[0] = m_ori_session.outputNames()[0].c_str();

    std::vector<OrtValue *> out_vals;
    bool ok = m_ori_session.run(in_names, {in_val}, out_names, out_vals, err);
    api->ReleaseValue(in_val);

    if (!ok || out_vals.empty() || !out_vals[0]) {
        return false;
    }

    float *logits = nullptr;
    OrtStatus *data_st = api->GetTensorMutableData(out_vals[0], reinterpret_cast<void **>(&logits));
    if (data_st || !logits) {
        if (data_st) api->ReleaseStatus(data_st);
        api->ReleaseValue(out_vals[0]);
        err = "Failed to read the orientation output tensor";
        return false;
    }

    // Two classes: 0 = 0 deg, 1 = 180 deg. The exported graph already ends in
    // softmax, and softmax is monotonic, so the comparison holds for either
    // probabilities or raw logits.
    if (logits[1] > logits[0]) {
        out_orientation = 180;
    }

    api->ReleaseValue(out_vals[0]);
    return true;
}

bool OcrPipeline::runRecognition(
    const ImageRgb &line_crop,
    std::string &out_text,
    float &out_score,
    std::string &err,
    bool &run_error)
{
    out_text.clear();
    out_score = 0.0f;
    if (line_crop.empty() || !m_rec_session.isValid()) {
        err = "Invalid line crop or Rec session";
        return false;
    }

    const int kRecH = 48;
    int target_w = static_cast<int>(std::round(line_crop.width * static_cast<float>(kRecH) / line_crop.height));

    // Find bucket
    int bucket_w = kRecWidthBuckets[8]; // 3200
    for (int b : kRecWidthBuckets) {
        if (target_w <= b) {
            bucket_w = b;
            break;
        }
    }
    int actual_w = std::min(target_w, bucket_w);

    std::vector<float> input_tensor(3 * kRecH * bucket_w, 0.0f);
    const float kMean[3] = {0.5f, 0.5f, 0.5f};
    const float kStd[3]  = {0.5f, 0.5f, 0.5f};

    resizeBilinear(
        line_crop.data.data(), line_crop.width, line_crop.height,
        input_tensor.data(), actual_w, kRecH,
        kMean, kStd,
        bucket_w, kRecH);

    const int64_t input_shape[4] = {1, 3, kRecH, bucket_w};
    OrtValue *in_val = nullptr;
    const OrtApi *api = OrtManager::instance().api();

    OrtStatus *st = api->CreateTensorWithDataAsOrtValue(
        OrtManager::instance().cpuMemoryInfo(),
        input_tensor.data(),
        input_tensor.size() * sizeof(float),
        input_shape,
        4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &in_val);

    if (st || !in_val) {
        if (st) api->ReleaseStatus(st);
        err = "Failed to create Rec input tensor";
        return false;
    }

    std::vector<const char *> in_names = {"x"};
    if (!m_rec_session.inputNames().empty()) in_names[0] = m_rec_session.inputNames()[0].c_str();
    std::vector<const char *> out_names = {"softmax_0.tmp_0"};
    if (!m_rec_session.outputNames().empty()) out_names[0] = m_rec_session.outputNames()[0].c_str();

    std::vector<OrtValue *> out_vals;
    bool ok = m_rec_session.run(in_names, {in_val}, out_names, out_vals, err);
    api->ReleaseValue(in_val);

    if (!ok) {
        run_error = true;
        return false;
    }
    if (out_vals.empty() || !out_vals[0]) {
        err = "Recognition produced no output tensor";
        return false;
    }

    // Inspect tensor shape. Expected [1, seq_len, num_classes].
    OrtTensorTypeAndShapeInfo *info = nullptr;
    st = api->GetTensorTypeAndShape(out_vals[0], &info);
    size_t dim_count = 0;
    if (st || !info) {
        if (st) api->ReleaseStatus(st);
        api->ReleaseValue(out_vals[0]);
        err = "Failed to inspect the recognition output tensor";
        return false;
    }
    st = api->GetDimensionsCount(info, &dim_count);
    if (st) {
        api->ReleaseStatus(st);
        dim_count = 0;
    }
    std::vector<int64_t> dims(dim_count, 0);
    if (dim_count > 0) {
        st = api->GetDimensions(info, dims.data(), dim_count);
        if (st) {
            api->ReleaseStatus(st);
            dim_count = 0;
        }
    }
    api->ReleaseTensorTypeAndShapeInfo(info);

    int64_t seq_len = (dim_count >= 2) ? dims[1] : 0;
    int64_t num_classes = (dim_count >= 3) ? dims[2] : 0;
    if (seq_len <= 0 || num_classes <= 0) {
        api->ReleaseValue(out_vals[0]);
        err = "Unexpected recognition output shape (expected [1, T, C])";
        return false;
    }
    if (static_cast<size_t>(num_classes) > m_dictionary.size() + 1) {
        api->ReleaseValue(out_vals[0]);
        err = "Dictionary/model mismatch: model emits " + std::to_string(num_classes)
              + " classes, dictionary has " + std::to_string(m_dictionary.size())
              + " entries (blank included)";
        return false;
    }

    float *logits = nullptr;
    st = api->GetTensorMutableData(out_vals[0], reinterpret_cast<void **>(&logits));
    if (st || !logits) {
        if (st) api->ReleaseStatus(st);
        api->ReleaseValue(out_vals[0]);
        err = "Failed to read the recognition output tensor";
        return false;
    }

    // CTC Greedy Decoding
    int prev_idx = -1;
    float score_sum = 0.0f;
    int char_count = 0;

    for (int64_t t = 0; t < seq_len; ++t) {
        const float *step_logits = logits + t * num_classes;
        int max_idx = 0;
        float max_val = step_logits[0];
        float row_sum = 0.0f;
        bool row_negative = false;

        for (int64_t c = 0; c < num_classes; ++c) {
            const float v = step_logits[c];
            if (v > max_val) {
                max_val = v;
                max_idx = static_cast<int>(c);
            }
            row_sum += v;
            if (v < 0.0f) {
                row_negative = true;
            }
        }

        // The exported PP-OCR graph already ends in softmax, so a row sums to 1
        // and the max value is the character probability. Applying softmax a
        // second time would collapse every score to ~1/num_classes.
        const bool row_is_probability = !row_negative && std::abs(row_sum - 1.0f) < 0.05f;
        const float prob = row_is_probability
                               ? max_val
                               : (1.0f / softmaxDenominator(step_logits, num_classes, max_val));

        if (max_idx > 0 && max_idx != prev_idx) {
            if (static_cast<size_t>(max_idx) < m_dictionary.size()) {
                out_text += m_dictionary[max_idx];
                score_sum += prob;
                char_count++;
            }
        }
        prev_idx = max_idx;
    }

    out_score = (char_count > 0) ? (score_sum / char_count) : 0.0f;
    api->ReleaseValue(out_vals[0]);
    return true;
}

bool OcrPipeline::runPipelineOnce(
    const ImageRgb &image,
    bool enable_cls,
    int max_side,
    std::vector<OcrLineResult> &out_lines,
    std::string &err,
    bool &run_error,
    bool force_dml_fail_run)
{
    if (force_dml_fail_run) {
        err = "Fault injection: --force-dml-fail=run";
        run_error = true;
        return false;
    }

    std::vector<QuadBox> raw_boxes;
    if (!runDetection(image, max_side, raw_boxes, err, run_error)) {
        return false;
    }

    const auto sorted_boxes = sortReadingOrder(raw_boxes);

    int line_idx = 0;
    std::string first_rec_err;
    bool any_rec_run_error = false;

    for (const auto &box : sorted_boxes) {
        ImageRgb crop;
        if (!cropTextLine(image, box, crop)) {
            continue;
        }

        int orientation = 0;
        if (enable_cls) {
            std::string ori_err;
            runOrientation(crop, orientation, ori_err);
            if (orientation == 180) {
                rotateImage180(crop);
            }
        }

        std::string text;
        float score = 0.0f;
        std::string rec_err;
        bool rec_run_error = false;
        if (!runRecognition(crop, text, score, rec_err, rec_run_error)) {
            if (first_rec_err.empty()) {
                first_rec_err = rec_err;
            }
            any_rec_run_error = any_rec_run_error || rec_run_error;
            continue;
        }
        if (text.empty()) {
            continue;
        }

        OcrLineResult res;
        res.index       = line_idx++;
        res.text        = text;
        res.score       = score;
        res.orientation = orientation;
        for (int k = 0; k < 4; ++k) {
            res.box[k][0] = static_cast<int>(std::round(box.points[k].x));
            res.box[k][1] = static_cast<int>(std::round(box.points[k].y));
        }
        out_lines.push_back(res);
    }

    // A failed Run is a session-level fault: report it so the caller can rebuild
    // the session on CPU. A single unusable crop is not, and keeps the rest of
    // the page.
    if (any_rec_run_error) {
        err = first_rec_err.empty() ? "Recognition Run failed" : first_rec_err;
        run_error = true;
        return false;
    }

    return true;
}

bool OcrPipeline::processImage(
    const ImageRgb &image,
    bool enable_cls,
    int max_side,
    std::vector<OcrLineResult> &out_lines,
    std::string &ep_used,
    std::string &fallback_reason,
    std::string &out_log,
    bool force_dml_fail_run)
{
    out_lines.clear();
    ep_used = m_using_dml ? "dml" : "cpu";

    // A DirectML failure during init() already happened before this call, so
    // report it here: main.cpp derives engine.fallback from this string.
    if (!m_init_fallback_reason.empty() && fallback_reason.empty()) {
        fallback_reason = m_init_fallback_reason;
        out_log += fallback_reason + "\n";
    }

    std::string err;
    bool run_error = false;

    if (runPipelineOnce(image, enable_cls, max_side, out_lines, err, run_error, force_dml_fail_run)) {
        return true;
    }

    out_log += "Pipeline failed: " + err + "\n";

    // Only an execution failure of a live DML session is retryable; model,
    // input and dictionary problems are hard failures.
    if (!run_error || !m_using_dml || m_target_ep != ExecutionProvider::Auto) {
        return false;
    }

    fallback_reason = "DML execution failed: " + err + "; retrying on CPU from Det";
    out_log += fallback_reason + "\n";

    m_using_dml = false;
    ep_used = "cpu";

    std::string cpu_log;
    if (!OrtManager::instance().createSession(m_paths.det_model_path, false, m_det_session, cpu_log)
        || !OrtManager::instance().createSession(m_paths.rec_model_path, false, m_rec_session, cpu_log)) {
        out_log += "CPU session rebuild failed: " + cpu_log + "\n";
        return false;
    }

    out_lines.clear();
    err.clear();
    run_error = false;
    if (!runPipelineOnce(image, enable_cls, max_side, out_lines, err, run_error, false)) {
        out_log += "CPU retry failed: " + err + "\n";
        return false;
    }

    return true;
}

}  // namespace seer_ocr
