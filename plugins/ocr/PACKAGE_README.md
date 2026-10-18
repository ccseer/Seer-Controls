# OCR Text

Extracts text from the current view (PDF or Image) using local on-device OCR powered by PP-OCRv5 and ONNX Runtime DirectML/CPU acceleration. Results are displayed in a SeerViewer window with loading progress -- where they can be selected and copied -- and saved as UTF-8 text and structured JSON.

## Options & Arguments

| Parameter / Flag | Accepted Values | Default | Purpose |
| :--- | :--- | :--- | :--- |
| `<screenshot>` | Path to image | (Required pos 1) | Path to the temporary PNG screenshot captured from the active viewer. |
| `<seerviewer>` | Path to executable | (Required pos 2) | Absolute path to `SeerViewer.exe` used to present the pending/result window. |
| `<input_file>` | File path or name | (Required pos 3) | Target document identifier used to construct output file names. |
| `--ep` | `auto`, `cpu`, `dml` | `auto` | Execution provider: `dml` (DirectML GPU), `cpu` (CPU fallback), or `auto` (try DML first, retry on CPU if needed). |
| `--cls` | `0`, `1` | `1` | Textline orientation classification: `1` to correct 180-degree upside-down lines, `0` to disable. |
| `--max-side` | Positive integer (px) | `4096` | Upper bound for image dimensions to prevent out-of-memory allocations. |
| `--wait-timeout` | 5000–600000 (ms) | `120000` | Timeout passed to `SeerViewer.exe --wait-file` before displaying a timeout notice. The shipped manifest passes `300000`. |
| `--console` | Flag | Off | Attaches to parent console to output verbose diagnostic messages. |
| `--force-dml-fail` | `init`, `run` | None | Fault injection flag for testing CPU fallback when DirectML initialization or execution fails. |

## Outputs and Storage

All outputs are saved under `%TEMP%\SeerOCR\`. The base name is an 8-character
fingerprint of the folder holding the document, then its file name, so two
documents with the same name in different folders never share one output set:

- `<fingerprint> - <filename>.ocr.txt`: Text result (UTF-8 without BOM + CRLF) displayed in SeerViewer.
- `<fingerprint> - <filename>.ocr.json`: Structured detection and recognition data following `seer.ocr/1` schema.
- `<fingerprint> - <filename>.ocr.log`: Diagnostic session log with timings, execution providers, model hash checks, and fallback records.

For example `C:\docs\report.pdf` is written to `%TEMP%\SeerOCR\081f0f17 - report.pdf.ocr.txt`.

## Requirements

- `models/` must contain `PP-OCRv5_server_det.onnx`, `PP-OCRv5_server_rec.onnx`,
  `PP-LCNet_x1_0_textline_ori.onnx` and the full 18,383-entry `ppocrv5_dict.txt`.
  A truncated dictionary is rejected instead of producing partial text.
- `models/SHA256SUMS` pins the artifacts. A pinned file that does not match is a
  hard failure (exit 3); unpinned files only log a warning.
- `backend/` carries `onnxruntime.dll`, `DirectML.dll` and the CRT DLLs that
  `onnxruntime.dll` imports (`msvcp140.dll`, `msvcp140_1.dll`,
  `vcruntime140.dll`, `vcruntime140_1.dll`). `package.ps1` copies them.

## Known Limitations

- **Coordinates**: Bounding boxes in `ocr.json` are in screenshot pixel coordinates, not original document PDF coordinates.
- **Orientation**: Line orientation correction supports 0° and 180° rotations; 90° / 270° rotated documents require pre-rotation.
- **Error Presentation**: Control plugins run detached; runtime and model errors are written directly to `<fingerprint> - <filename>.ocr.txt` so the waiting window displays the explanation to the user.
