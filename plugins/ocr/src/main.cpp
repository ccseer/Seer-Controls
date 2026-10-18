#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <exception>
#include <string>
#include <vector>
#include <iostream>

#include "args.h"
#include "paths.h"
#include "launcher.h"
#include "cleanup.h"
#include "hash.h"
#include "image_loader.h"
#include "ort_loader.h"
#include "ocr_pipeline.h"
#include "output_writer.h"

namespace seer_ocr {

namespace {

std::string toUtf8(const std::wstring &wstr)
{
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool fileExists(const std::wstring &path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// Control plugins run detached: the host never sees the exit code or stderr, so
// every failure has to land in the .ocr.txt the waiting window is polling.
int failWith(const OcrOutputPaths &paths, const std::string &message, int code, bool console)
{
    writeOcrErrorAtomically(paths.txt_path, message);
    writeOcrLog(paths.log_path, "Error: " + message);
    if (console) {
        std::cerr << message << std::endl;
    }
    return code;
}

int runOcr(const OcrCliOptions &opts, const OcrOutputPaths &paths,
           std::chrono::steady_clock::time_point start_time)
{
    // 1. Clean up previous results and sweep 7-day old files
    cleanupPreviousRun(paths);
    sweepOldOutputs(paths.base_dir);

    // 2. Launch SeerViewer immediately so user sees loading state right away (<=300ms)
    bool launched = launchSeerViewerWaiting(
        opts.seerviewer_exe,
        paths.txt_path,
        opts.wait_timeout_ms);

    if (!launched) {
        return failWith(paths,
                        "Failed to launch SeerViewer: " + toUtf8(opts.seerviewer_exe),
                        5, opts.console);
    }

    // 3. Validate screenshot exists
    if (!fileExists(opts.screenshot_path)) {
        return failWith(paths,
                        "Screenshot image does not exist: " + toUtf8(opts.screenshot_path),
                        2, opts.console);
    }

    // 4. Initialize ONNX Runtime and Models
    std::wstring exe_dir = getModuleDirectory();
    std::wstring backend_dir = exe_dir + L"\\backend";
    std::wstring models_dir  = exe_dir + L"\\models";

    OcrModelPaths model_paths;
    model_paths.det_model_path = models_dir + L"\\PP-OCRv5_server_det.onnx";
    model_paths.ori_model_path = models_dir + L"\\PP-LCNet_x1_0_textline_ori.onnx";
    model_paths.rec_model_path = models_dir + L"\\PP-OCRv5_server_rec.onnx";
    model_paths.dict_path      = models_dir + L"\\ppocrv5_dict.txt";

    // A corrupt model is a hard failure, never a GPU fault: verify before any
    // session exists so the fallback path can not mask it.
    std::string hash_log;
    const bool hashes_ok = verifyModelFiles(
        models_dir,
        {L"PP-OCRv5_server_det.onnx",
         L"PP-OCRv5_server_rec.onnx",
         L"PP-LCNet_x1_0_textline_ori.onnx",
         L"ppocrv5_dict.txt"},
        hash_log);
    if (!hash_log.empty()) {
        writeOcrLog(paths.log_path, hash_log);
    }
    if (!hashes_ok) {
        return failWith(paths, "Model verification failed:\n" + hash_log, 3, opts.console);
    }

    std::wstring ort_err;
    if (!OrtManager::instance().init(backend_dir, ort_err)) {
        return failWith(paths,
                        "ONNX Runtime DLL initialization failed: " + toUtf8(ort_err),
                        3, opts.console);
    }

    OcrPipeline pipeline;
    std::string init_log;
    bool init_ok = pipeline.init(
        model_paths,
        opts.ep,
        init_log,
        opts.force_fail == ForceDmlFail::Init);

    if (!init_ok) {
        return failWith(paths, "Model initialization failed:\n" + init_log, 3, opts.console);
    }

    // 5. Load image
    ImageRgb image;
    std::wstring img_err;
    if (!loadImageRgb(opts.screenshot_path, image, img_err)) {
        return failWith(paths, "Image decoding failed: " + toUtf8(img_err), 2, opts.console);
    }

    // 6. Perform OCR pipeline
    std::vector<OcrLineResult> lines;
    std::string ep_used;
    std::string fallback_reason;
    std::string run_log;

    bool run_ok = pipeline.processImage(
        image,
        opts.enable_cls,
        opts.max_side,
        lines,
        ep_used,
        fallback_reason,
        run_log,
        opts.force_fail == ForceDmlFail::Run);

    if (!run_ok) {
        return failWith(paths, "OCR recognition failed: " + run_log, 4, opts.console);
    }

    // 7. Write outputs
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    OcrJsonMeta meta;
    meta.source_image = toUtf8(opts.screenshot_path);
    meta.img_width    = image.width;
    meta.img_height   = image.height;
    meta.ep_used      = ep_used;
    meta.fallback     = !fallback_reason.empty();
    meta.elapsed_ms   = elapsed_ms;

    if (!writeOcrJsonAtomically(paths.json_path, meta, lines)) {
        return failWith(paths, "Failed to write json output: " + toUtf8(paths.json_path), 6, opts.console);
    }

    // The txt rename is the completion signal for the waiting SeerViewer job, so
    // it stays the last step.
    if (!writeOcrTextAtomically(paths.txt_path, lines)) {
        return failWith(paths, "Failed to write text output: " + toUtf8(paths.txt_path), 6, opts.console);
    }

    std::string log_msg = "Completed: " + std::to_string(lines.size()) + " lines in "
                        + std::to_string(elapsed_ms) + " ms, EP=" + ep_used;
    if (!fallback_reason.empty()) {
        log_msg += " (Fallback: " + fallback_reason + ")";
    }
    writeOcrLog(paths.log_path, log_msg);

    if (opts.console) {
        std::cout << log_msg << std::endl;
    }

    return 0;
}

}  // namespace

int runWorker()
{
    auto start_time = std::chrono::steady_clock::now();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return 1;
    }

    std::vector<std::wstring> raw_args;
    raw_args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        raw_args.emplace_back(argv[i]);
    }
    LocalFree(argv);

    OcrCliOptions opts = parseCommandLine(raw_args);

    if (opts.console) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *fDummy = nullptr;
            freopen_s(&fDummy, "CONOUT$", "w", stdout);
            freopen_s(&fDummy, "CONOUT$", "w", stderr);
        }
    }

    if (!opts.valid) {
        if (!opts.input_file.empty()) {
            auto paths = resolveOutputPaths(opts.input_file);
            return failWith(paths,
                            "Invalid arguments: " + toUtf8(opts.error_msg),
                            1, opts.console);
        }
        if (opts.console) {
            std::wcerr << L"Error: " << opts.error_msg << std::endl;
            std::wcerr << L"Usage: SeerOcrWorker.exe <screenshot> <seerviewer_exe> <input_file> [--ep=auto|cpu|dml] [--cls=0|1] [--wait-timeout=<ms>] [--console]" << std::endl;
        }
        return 1;
    }

    auto paths = resolveOutputPaths(opts.input_file);

    try {
        return runOcr(opts, paths, start_time);
    }
    catch (const std::exception &e) {
        return failWith(paths, std::string("Unexpected failure: ") + e.what(), 4, opts.console);
    }
    catch (...) {
        return failWith(paths, "Unexpected failure", 4, opts.console);
    }
}

}  // namespace seer_ocr

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    return seer_ocr::runWorker();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return seer_ocr::runWorker();
}
