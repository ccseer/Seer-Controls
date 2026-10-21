# OCR Text

This independently downloadable Seer Canonical v1 Control package extracts text from the current document or image view using high-precision on-device OCR powered by PP-OCRv5 and ONNX Runtime DirectML/CPU. QR codes and barcodes in the view are additionally decoded by a statically linked ZXing-C++ engine and reported above the text. The result opens in a separate `SeerViewer` window, where it can be selected and copied.

It is a multi-type Control package: it appears for PDF documents (`${type_pdf}`) and images (`${type_image}`).

**Requires Seer 4.7.10 or later.** The package consumes `${screenshot}` and asks `SeerViewer.exe` to wait for a file it has not written yet (`--wait-file`). On an older host the manifest's `${screenshot}` argument is an unknown token and the launched viewer does not understand `--wait-file`, so the plugin cannot work; `appMinVersion` in `plugin.json` pins this.

The package interface in `plugin.json` is fixed:

- backend: `process`
- capability: `control`
- matchers: `["${type_pdf}", "${type_image}"]`
- command: package-relative `SeerOcrWorker.exe`
- arguments: `["${screenshot}", "${seerviewer_exe}", "${input_file}", "--ep=auto", "--cls=1", "--barcode=1", "--wait-timeout=300000"]`
- `timeout_ms`: 120000
- `appMinVersion`: `4.7.10`
- `success_exit_codes`: `[0]`
- `close_after_success`: `false`

`close_after_success` is `false` on purpose: after clicking OCR, the main preview window stays open while the recognition result is displayed in a separate, dedicated `SeerViewer.exe` window.

## Options & Arguments

| Parameter / Flag | Accepted Values | Default | Purpose |
|---|---|---|---|
| `<screenshot>` | Path to image | (Required pos 1) | Path to the temporary PNG screenshot captured from the active viewer. |
| `<seerviewer>` | Path to executable | (Required pos 2) | Absolute path to `SeerViewer.exe` used to present the pending/result window. |
| `<input_file>` | File path or name | (Required pos 3) | Target document identifier used to construct output file names. |
| `--ep <mode>` | `auto`, `cpu`, `dml` | `auto` | Execution provider: `dml` (DirectML GPU), `cpu` (CPU baseline), or `auto` (try DML first, retry on CPU if needed). |
| `--cls <0\|1>` | `0`, `1` | `1` | Textline orientation classification: `1` to correct 180° upside-down lines, `0` to disable. |
| `--barcode <0\|1>` | `0`, `1` | `1` | ZXing barcode/QR detection on the screenshot with aggregated output; `0` skips the scan and omits the JSON `barcodes` key. |
| `--max-side <px>` | Positive integer | `4096` | Upper bound for image dimensions to prevent out-of-memory allocations. |
| `--wait-timeout <ms>` | 5000–600000 | `120000` | Timeout passed to `SeerViewer.exe --wait-file` before displaying a timeout notice. The shipped manifest passes `300000` because server-model cold start needs the headroom. |
| `--console` | flag | absent | Attaches to parent console to output verbose diagnostic messages. |
| `--force-dml-fail <phase>` | `init`, `run` | absent | Fault injection flag for testing CPU fallback when DirectML initialization or execution fails. |

To change the default behaviour of the control, edit the argument list of the installed control in Seer's Settings:

```json
["${screenshot}", "${seerviewer_exe}", "${input_file}", "--ep=cpu", "--wait-timeout=120000"]
["${screenshot}", "${seerviewer_exe}", "${input_file}", "--cls=0"]
["${screenshot}", "${seerviewer_exe}", "${input_file}", "--barcode=0"]
```

## Architecture & Execution Lifecycle

### 1. Order of Execution (Priority Loading UX)

1. **Parse arguments**: Parses positional arguments and options using `CommandLineToArgvW`.
2. **Resolve output paths**: Maps outputs into `%TEMP%\SeerOCR\` under `<fingerprint> - <filename>`, where the fingerprint is the first 8 hex characters of the MD5 of the folder holding the document (`C:\docs\report.pdf` → `081f0f17 - report.pdf.ocr.txt`). Same-named files in different folders therefore get separate output sets instead of overwriting each other, while a re-run on the same file still replaces its own result. The `.ocr` infix is kept because `sweepOldOutputs()` reclaims aged files with a `*.ocr.*` glob.
3. **Clean previous run**: Deletes prior outputs for this document base name so old results never trigger the file watcher.
4. **Launch SeerViewer immediately**: Launches `SeerViewer.exe --wait-file --wait-timeout=300000 -- "<txt_abs_path>"` **before** model loading or session creation. SeerViewer immediately enters its `seer-pending` asynchronous job and displays the loading progress in ≤300ms.
5. **Dynamic Library Loading**: Resolves `backend/onnxruntime.dll` using `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`. No static linkage to `onnxruntime.lib` exists, ensuring application-local DLL isolation.
6. **ZXing Barcode Scan**: Wraps the decoded RGB buffer in a zero-copy `ZXing::ImageView` and runs `ReadBarcodes` (pure CPU, try-harder/rotate/invert/downscale). Detected symbols are stored for the aggregated output; a scan failure is downgraded to a log entry and never fails the run.
7. **Inference Pipeline**: Runs text detection, perspective textline crop, orientation correction, and text recognition.
8. **Atomic Output Delivery**: Writes structured JSON first (with a `barcodes` array when the scan ran), then writes text — detected barcodes in a highlighted block on top of the OCR text — and renames `<base>.ocr.txt` as the **final step**. The arrival of `<base>.ocr.txt` signals SeerViewer to switch from loading to displaying the result.

### 2. Error Self-Responsibility

Control plugins are launched detached (`QProcess::startDetached`) without pipes or exit-code capture by the host. Therefore, `SeerOcrWorker` owns error presentation:

- If model loading fails, DirectML fails without CPU fallback, or image decoding errors occur, the worker writes the readable failure explanation into `<base>.ocr.txt`.
- Because `SeerViewer.exe` is already waiting on that file path, the window seamlessly opens to present the error message to the user.
- Exit codes 1 through 6 are reserved for command-line self-testing.

### 3. DirectML GPU Acceleration & CPU Fallback

- `--ep=dml` or `--ep=auto` enables DirectML GPU hardware acceleration via DirectX 12.
- DirectML sessions are configured with `DisableMemPattern` and `ORT_SEQUENTIAL`.
- If DirectML initialization or execution fails (due to unsupported driver, remote desktop, or GPU memory exhaustion), `--ep=auto` automatically destroys the DirectML session, constructs a fresh CPU session, and reruns the pipeline from detection.

## Dependencies

- **ONNX Runtime 1.22.1 (DirectML build)**: loaded dynamically at runtime from `backend/onnxruntime.dll`; only the vendored C-API header (`third_party/onnxruntime/`) is compiled in.
- **ZXing-C++ 2.3.0** (`third_party/zxing-cpp/`, Apache-2.0): vendored and built from source as a static library (readers only), linked into `SeerOcrWorker.exe` together with the statically linked VC++ runtime. No extra DLL ships for it; see `third_party/zxing-cpp/VENDOR.md` for the one inert local deviation and the upgrade checks.

## Build and Test

Each package is configured standalone. The machine-local `CMakeUserPresets.json` points the build directory outside the repository tree:

```powershell
cd plugins/ocr
cmake --preset default        # Ninja, Release, out-of-repo binaryDir
cmake --build --preset default
ctest --preset default
```

Without the preset file, configure with an explicit out-of-repo `-B` directory:

```powershell
cmake -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build <build-dir>
ctest --test-dir <build-dir>
```

## Packaging the Distributable ZIP

> [!NOTE]
> **Repository Size & Binary Assets**: To avoid repository bloat, heavy binary assets (`models/*.onnx` and `backend/*.dll`) are distributed outside Git. You can obtain all required `models/` and `backend/` files directly by extracting them from the official downloaded plugin ZIP package.

Run the automated packaging script:

```powershell
powershell -ExecutionPolicy Bypass -File .\package.ps1
```

The script builds the Release binary out-of-tree, stages all required assets, and creates:

```text
dist/ocr-1.0.0.zip
├── plugin.json
├── README.md               # User-facing README staged from PACKAGE_README.md
├── SeerOcrWorker.exe       # Pure Win32 C++17 helper, statically links ZXing-C++ (readers) and the CRT
├── backend/
│   ├── onnxruntime.dll     # ONNX Runtime 1.22.1 DirectML build
│   ├── DirectML.dll        # DirectML library (~46.5 MB)
│   └── onnxruntime_providers_shared.dll
└── models/
    ├── PP-OCRv5_server_det.onnx
    ├── PP-OCRv5_server_rec.onnx
    ├── PP-LCNet_x1_0_textline_ori.onnx
    ├── ppocrv5_dict.txt    # 18,383 tokens
    └── SHA256SUMS
```

## Acceptance Checklist

1. **Loading Window**: Clicking OCR immediately displays a loading window (≤300ms) that seamlessly transitions to text once finished.
2. **PDF Current View**: Clicking on a scrolled PDF captures the currently visible area rather than page 1.
3. **Multi-type Support**: Operates identically on images (`${type_image}`) and PDFs (`${type_pdf}`).
4. **Reading Order**: Multi-column documents separate left and right columns naturally without interleaved lines.
5. **Hardware Acceleration**: Automatically utilizes DirectML when a compatible GPU is present; gracefully retries on CPU if GPU initialization or execution fails.
6. **Error Reporting**: Missing model or corrupted files display an informative error in the pending viewer window.
7. **Clean Teardown**: Closing the pending window terminates SeerViewer cleanly without orphaned processes.
8. **Barcode Report**: A view containing a QR code or barcode shows the decoded `Type`/`Content` block at the top of the text window; the JSON carries the same symbol in `barcodes`.
9. **Barcode Toggle**: With `--barcode=0`, output is byte-identical to the text-only behaviour and the JSON contains no `barcodes` key.
10. **Barcode Robustness**: An image with no barcode (plain photo or text) scans silently and adds nothing to the output.
