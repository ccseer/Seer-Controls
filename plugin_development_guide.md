# Seer Control Plugin Development Specification & Reference Guide

This guide instructs AI Agents on how to develop and package custom **Control Plugins** for Seer, the Windows Quick Look tool.

---

## 🔗 Reference Links

- **Official Documentation**: [https://1218.io/docs/seer/create-plugin.html#control-plugins](https://1218.io/docs/seer/create-plugin.html#control-plugins)
- **Control Plugins Repository**: [https://github.com/ccseer/Seer-Controls](https://github.com/ccseer/Seer-Controls)
- **Official Plugin Downloads**: [https://1218.io/docs/seer/download.html#controls](https://1218.io/docs/seer/download.html#controls)

---

## 🛠️ Control Plugin Architecture Overview

Control plugins add custom action buttons to the **left section of the bottom Control Bar** in the Seer preview window. They enable file-level external tool integrations, system dialogs, automation scripts, and clipboard operations.

### Execution Workflow

1. **Trigger**: When a file or folder is previewed, Seer matches its extension/type against installed control plugins. Matching plugins appear as buttons on the Control Bar.
2. **User Action**: The user clicks the control button.
3. **Launch**: Seer executes the configured helper program or script via `QProcess::start`, passing parameters expanded from placeholders (e.g. `${input_file}`).
4. **Execution & UI**:
   - The helper performs its task (e.g. launches a terminal, copies file info to the clipboard, opens a Windows system dialog, or executes a background script).
   - The helper does **not** draw its own persistent main window.
5. **Completion & Error Reporting**:
   - **Success**: The process exits with code `0` (or any code in `success_exit_codes`). If `"close_after_success": true` is set, Seer automatically closes the preview window.
   - **Failure**: The process exits with a non-zero code. Anything written to standard error (`stderr`) is captured by Seer and displayed to the user as an in-app failure Toast notification.

> [!IMPORTANT]
> **Script Execution and Runtimes:**
> - Seer automatically adapts `.ps1` files to run via PowerShell (`powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script>`).
> - For other scripts (`.py`, `.js`, `.bat`), Seer launches the entry directly via `QProcess::start(exe, args)` without wrapping an interpreter.
> - Deliver a standalone `.exe` (e.g. compiled C++, Rust, Go, or Python via PyInstaller) or configure `powershell.exe` / native Windows executables.

---

## 📄 Command-Line Placeholders & Matchers

### Command-Line Placeholders

| Placeholder | Description | Example |
| :--- | :--- | :--- |
| `${input_file}` | Absolute path of the previewed file or folder. | `C:\Users\Name\Desktop\report.pdf` |
| `${use_backslash}` | Optional flag to format path separators using Windows native backslashes (`\`). | `${use_backslash}` |
| `${seer_dir}` | Directory containing `Seer.exe`. | `C:\Program Files\Seer` |
| `${seer_exe}` | Absolute path to `Seer.exe`. | `C:\Program Files\Seer\Seer.exe` |
| `${7z}` | Absolute path to `7z.exe` bundled with Seer. | `C:\Program Files\Seer\plugins\7z.exe` |

> [!WARNING]
> A control helper receives the target path only. `${output_dir}`, `${output_file}`, and `${no_cache}` are **rejected** in a control manifest. A control that needs a scratch location must resolve one itself: its own executable directory first, then `<temp>\SeerControlPlugins\<packageName>`.

### File Matcher Tokens in `extensions`

In addition to standard file extensions (e.g. `"png"`, `"zip"`), control plugins can match broad categories using special tokens:

| Token | Description |
| :--- | :--- |
| `${type_folder}` | Matches directories and folders. |
| `${type_file}` | Matches all files (including extensionless files). |
| `${type_all}` | Matches everything (files and folders). |
| `${type_pdf}` | Matches PDF documents. |
| `${type_image}` | Matches images supported by Seer's native image viewer. |
| `${type_media}` | Matches audio and video formats. |
| `${type_web}` | Matches HTML and Markdown web files. |
| `${type_text}` | Matches text, code, and config files. |
| `${type_none}` | Matches files outside the categories above (e.g. archives, executables). |

---

## 📋 `plugin.json` Schema Specification

Every Seer control plugin must place a strictly valid `plugin.json` in its root directory. **Comments (`//` or `/* */`) and trailing commas are strictly forbidden.**

### Field Definitions

| Field Name | Type | Presence | Description |
| :--- | :--- | :--- | :--- |
| `schema_version` | Integer | **Required** | Must be `1`. |
| `id` | String | **Required** | Unique reverse domain ID (e.g. `"io.1218.seer.terminal-here"`). |
| `name` | String | **Required** | Display name shown in tooltips and settings. |
| `description` | String | Optional | Concise description of the action. |
| `version` | String | **Required** | Semver string (e.g. `"1.0.0"`). |
| `appMinVersion` | String | **Required** | Minimum Seer version required (e.g. `"4.5.10"`). |
| `backend` | String | **Required** | Must be `"process"`. |
| `capabilities` | Array | **Required** | Must contain `["control"]`. |
| `extensions` | Array | **Required** | Supported extensions or matcher tokens (e.g. `["${type_folder}"]`, `["png", "jpg"]`). |
| `command` | String | **Required** | Executable or script filename relative to the package root. |
| `arguments` | Array | **Required** | Array of CLI arguments (e.g. `["--input", "${input_file}"]`). |
| `close_after_success` | Boolean | Optional | If `true`, closes preview window on successful run (default: `false`). |
| `timeout_ms` | Integer | Optional | Process timeout in milliseconds (e.g. `30000`). |
| `success_exit_codes` | Array | Optional | List of exit codes indicating success (default: `[0]`). |

> [!NOTE]
> Control plugins can also declare execution settings under `"invocations": { "control": { "command": "...", "arguments": [...] } }`. Both forms are fully supported.

---

## 📋 `plugin.json` Examples

### Example 1: Folder Action (`Terminal Here`)
```json
{
  "schema_version": 1,
  "id": "io.1218.seer.terminal-here",
  "name": "Terminal Here",
  "description": "Opens a terminal window in the current folder.",
  "version": "1.0.0",
  "appMinVersion": "4.5.10",
  "backend": "process",
  "capabilities": [
    "control"
  ],
  "extensions": [
    "${type_folder}"
  ],
  "command": "terminal_here.exe",
  "arguments": [
    "--input",
    "${input_file}"
  ],
  "close_after_success": false,
  "timeout_ms": 150000,
  "success_exit_codes": [
    0
  ]
}
```

### Example 2: File Action (`Explorer Open With`)
```json
{
  "schema_version": 1,
  "id": "io.1218.seer.explorer-open-with",
  "name": "Open With",
  "description": "Opens the Windows Open With dialog for the current file.",
  "version": "1.1.0",
  "appMinVersion": "4.5.10",
  "backend": "process",
  "capabilities": [
    "control"
  ],
  "extensions": [
    "${type_file}"
  ],
  "command": "shellopenwith.exe",
  "arguments": [
    "--input",
    "${input_file}",
    "${use_backslash}"
  ],
  "timeout_ms": 30000,
  "success_exit_codes": [
    0
  ]
}
```

> [!TIP]
> To automatically close the preview window upon success, set `"close_after_success": true` in the manifest.

---

## 🤖 AI Agent Delivery Requirements

When building a Control Plugin from a user requirement:

1. **Verify the Requirement**:
   - Check what trigger (file extensions or `${type_folder}`/`${type_file}`) is needed.
   - Confirm the exact behavior: launches a program, copies to clipboard, or invokes a system dialog.
   - If ambiguous, ask one clarifying question before writing code.

2. **Implement the Helper**:
   - Write clean, standalone code (C++17 with static CRT, Win32 API, or standalone executable/script).
   - Return exit code `0` on success.
   - If an error occurs, write a helpful error message to `stderr` and exit with a non-zero code.
   - Avoid creating persistent custom GUI windows unless handing off to a native system dialog.

3. **Provide `PACKAGE_README.md`**:
   - Every package must include `PACKAGE_README.md`: one short paragraph describing what the control does, followed by an `## Options & Arguments` Markdown table detailing every option accepted by the helper (columns: `Option`, `Values`, `Default`, `Purpose / Description`).
   - The build system stages and installs `PACKAGE_README.md` as `README.md` into the package ZIP, and manifest tests assert that it exists and is non-empty.
   - If the helper needs an external runtime (PowerShell, Python, Node.js, a VC++ redistributable), state it in `PACKAGE_README.md` and ship the runtime with the package. Prefer a single self-contained executable so the end user installs nothing extra.

4. **Deliver a Complete, Installable Package**:
   - Create a dedicated folder containing `plugin.json`, compiled executable/script, assets, and `PACKAGE_README.md`.
   - Do not stop at code snippets or partial drafts.

5. **Verify Locally**:
   - Validate `plugin.json` with strict JSON parsing: no comments, no trailing commas.
   - Confirm the `command` value matches the packaged executable or script file name **exactly**, including its extension. A name that does not exist in the package root is the most common reason a control fails to start.
   - Keep every `extensions` entry lowercase and free of leading dots.
   - Run the helper in PowerShell with a real file or folder path:
     ```powershell
     # Executable helpers:
     .\my_control.exe --input "C:\Path\To\TestFile.ext"
     $LASTEXITCODE  # Must be 0

     # PowerShell script helpers:
     powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\my_control.ps1 --input "C:\Path\To\TestFile.ext"
     $LASTEXITCODE  # Must be 0
     ```

6. **Package into Flat ZIP**:
   - Pack the contents directly into a `.zip` archive so that `plugin.json`, the executable, and `README.md` (staged from `PACKAGE_README.md`) sit at the archive root:
     ```powershell
     7z a -tzip -mx=9 my-control-plugin-1.0.0.zip .\*
     ```
   - Canonical v1 control packages use a **flat** root. This is the opposite of legacy Convert plugins, whose online-catalog archives wrap everything in a top-level `<archive-name>/` folder; do not reuse that layout here, and never mix flat and nested entries — the host rejects an archive with an ambiguous package root.
   - Verify by importing via **Seer Settings > Controls > +**.
