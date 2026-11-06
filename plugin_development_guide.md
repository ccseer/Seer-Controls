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
3. **Launch**: Seer resolves the package-relative command, expands the placeholders, and starts the helper with `QProcess::startDetached`. The call returns immediately and the host keeps no process handle.
4. **Execution & UI**:
   - The helper performs its task (e.g. launches a terminal, copies file info to the clipboard, opens a Windows system dialog, or executes a background script).
   - The helper does **not** draw its own persistent main window.
5. **Completion & Error Reporting**:
   - **Success**: The operating system accepted the launch. This is the only outcome the host observes. It stays silent: no success toast, no exit-code check, no stdout or stderr capture, no timeout, and no cancellation of the helper's process tree. The helper may therefore keep running after the preview window is hidden or after Seer exits.
   - **Failure**: Only a launch-time failure is reported to the user: an empty or unresolvable command, a start refused by the operating system, a rejected argument, or a `${screenshot}` capture that could not be prepared. Seer shows the reason in an in-app toast and shakes the preview window, which stays open. Anything the helper writes to stderr after it has started is not seen by Seer.
   - **Close after success**: If `close_after_success` is true, Seer hides the preview window as soon as the launch succeeds, but only while the originating preview container is still visible and the previewed path has not changed.

> [!IMPORTANT]
> **Script Execution and Runtimes:**
> - Seer automatically adapts `.ps1` files to run via PowerShell (`powershell.exe -NoProfile -ExecutionPolicy Bypass -File <script>`).
> - For other scripts (`.py`, `.js`, `.bat`), Seer launches the entry directly via `QProcess::startDetached(exe, args)` without wrapping an interpreter.
> - Deliver a standalone `.exe` (e.g. compiled C++, Rust, Go, or Python via PyInstaller) or configure `powershell.exe` / native Windows executables.

> [!IMPORTANT]
> **Helpers own their own failure UI.** Because the host stops observing a Control helper once the launch succeeds, a helper that starts and then fails (a missing runtime, a denied operation, an empty result) must present that failure itself, for example in a system-owned message box. Writing to stderr is useful for diagnostics but reaches no Seer UI. In a Control manifest, `timeout_ms` and `success_exit_codes` are accepted but ignored; they still apply to Preview and Property process plugins.

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
| `${screenshot}` | Absolute path of a temporary PNG captured from the current view. Captured only when an argument actually uses this token; a failed or blocked capture prevents the launch and shows a toast. | `C:\...\screenshot\capture.png` |
| `${seerviewer_exe}` | Absolute path to `SeerViewer.exe`, for controls that present results in a standalone viewer window. | `C:\Program Files\Seer\SeerViewer.exe` |
| `${theme}` | Current Seer theme. | `dark` or `light` |
| `${lang}` | Current UI language as a BCP-47 tag. | `zh-CN` |

> [!WARNING]
> A control helper receives the target path only. `${output_dir}`, `${output_file}`, and `${no_cache}` are **rejected** in a control manifest (the loader rejects them at import time), and `${use_backslash}` must be a standalone argument because it is removed before launch. A control that needs a scratch location must resolve one itself: its own executable directory first, then `<temp>\SeerControlPlugins\<packageName>`.

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
| `close_after_success` | Boolean | Optional | If `true`, hides the preview window as soon as the detached launch succeeds. **Default: `true`**. Only valid for the Control capability; the window stays open when the originating preview is no longer visible or the previewed path has changed. |
| `timeout_ms` | Integer | Optional | Ignored for Control. Still used by Preview/Property process invocations (e.g. `30000`). |
| `success_exit_codes` | Array | Optional | Ignored for Control: the host never observes a Control helper's exit code. Still used by Preview/Property process invocations (default: `[0]`). |

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
  "close_after_success": false
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
  ]
}
```

> [!TIP]
> The host default is `true`: a successful launch hides the preview window. Set `"close_after_success": false` to keep it open, which is what most helpers that show a dialog or write a result next to the file need.

---

## 🤖 AI Agent Delivery Requirements

When building a Control Plugin from a user requirement:

1. **Verify the Requirement**:
   - Check what trigger (file extensions or `${type_folder}`/`${type_file}`) is needed.
   - Confirm the exact behavior: launches a program, copies to clipboard, or invokes a system dialog.
   - If ambiguous, ask one clarifying question before writing code.

2. **Implement the Helper**:
   - Write clean, standalone code (C++17 with static CRT, Win32 API, or standalone executable/script).
   - Return exit code `0` on success and a non-zero code on failure, and write a helpful message to `stderr`. These serve the helper's own tests and diagnostics: the host does not read either for a Control action.
   - Present any failure the user must see from inside the helper (for example a system-owned message box). The host reports launch failures only; a helper that starts and then fails is otherwise invisible to Seer.
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
     $LASTEXITCODE  # Must be 0 (helper contract; Seer does not read it for Control actions)

     # PowerShell script helpers:
     powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\my_control.ps1 --input "C:\Path\To\TestFile.ext"
     $LASTEXITCODE  # Must be 0 (helper contract; Seer does not read it for Control actions)
     ```

6. **Package into Flat ZIP**:
   - Pack the contents directly into a `.zip` archive so that `plugin.json`, the executable, and `README.md` (staged from `PACKAGE_README.md`) sit at the archive root:
     ```powershell
     7z a -tzip -mx=9 my-control-plugin-1.0.0.zip .\*
     ```
   - Canonical v1 control packages use a **flat** root. This is the opposite of legacy Convert plugins, whose online-catalog archives wrap everything in a top-level `<archive-name>/` folder; do not reuse that layout here, and never mix flat and nested entries — the host rejects an archive with an ambiguous package root.
   - Verify by importing via **Seer Settings > Controls > +**.
