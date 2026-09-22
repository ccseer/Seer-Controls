# Seer Controls

Official control plugins for [Seer](https://1218.io/).

## Plugins

### Shipped

- [Explorer Open With](./plugins/explorer-open-with): Opens the Windows "Open With" dialog for the current regular file.
- [Explorer Properties](./plugins/explorer-properties): Opens the Windows Properties dialog for the current regular file.
- [Explorer Share](./plugins/explorer-share): Opens the Windows system Share UI for the current regular file.

### Control packages

These five packages implement the Seer Canonical v1 Control contract: one
`plugin.json`, one package-relative helper, `capabilities: ["control"]`, and a
whole-file or whole-folder matcher. Each has its own README documenting every
option, its dependency policy, what a successful invocation means, what happens
on cancellation, and a manual acceptance checklist.

- [Terminal Here](./plugins/terminal-here): Opens a terminal window in the current folder. Chooses between Windows Terminal, PowerShell 7, Windows PowerShell and Command Prompt, and documents the Windows Terminal `;` escaping that CRT quoting does not solve.
- [Quick Folder Compression](./plugins/folder-quick-compress): Creates a ZIP archive beside the current folder using Seer's own `${7z}`, with an explicit inventory that refuses reparse points and a publish step that never replaces an existing archive.
- [Copy Folder Listing](./plugins/copy-folder-listing): Copies a bounded, deterministic plain-text listing of the current folder's first level to the clipboard, one entry per line, with visible link, inaccessible and truncation markers.
- [Who Is Locking This File](./plugins/file-lock-info): A read-only Restart Manager report for the current file, with Refresh, Copy Report and an explicit consent-driven administrator retry.
- [Image Optimization](./plugins/image-optimize): Optimizes the current PNG, JPEG or WebP image through pngquant, cwebp or jpegoptim, with a self-contained structural validator, explicit lossy consent, a retained copy-mode result and a recovery-recorded in-place replacement.

## Shared code

`plugins/common` is shared build code, not a deployable package and not a
separately discoverable plugin. It is header-only and contains:

- `shelluiworker.h` — the UI-readiness handoff used by the three controls above.
- `winui.h`, `wincmd.h`, `winpath.h`, `winproc.h`, `winclip.h`, `winexit.h`,
  `wintext.h` — the dedicated launcher and shared utilities used by the five
  Control packages. `ShellUiWorker` is untouched: its worker layout is a fixed
  `--input <path>` triple, so the newer helpers use a launcher that accepts
  arbitrary option lists.
- `test/` — a minimal check harness, a cross-process probe fixture, the shared
  assertions that run in every new package, and the manifest/staging assertions
  each package builds on.

## Dependency policy

The native packages depend on nothing but the C++17 standard library and the
Win32 API, and link the VC++ runtime statically, so each package ships as
`plugin.json` plus one executable. No Qt, no third-party library, and no
source-vendored dependency. `dumpbin -dependents` on every shipped helper lists
only operating-system DLLs; `file_lock_info.exe` additionally imports
`Rstrtmgr.dll`, the Windows Restart Manager.

The optimisers used by Image Optimization are the one deliberate exception:
they are third-party executables, they are never bundled or downloaded, and the
package reports which one is missing instead of guessing.

## Building and testing

Each package is configured standalone from inside its package directory. A
machine-local `CMakeUserPresets.json` in every package directory points the
build out of the repository, to a per-package directory outside the working
tree (Ninja, Release); the Ninja generator needs an MSVC environment on PATH:

```powershell
cd plugins/<name>
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Without the local preset file, configure with an explicit out-of-repo `-B`
directory instead. `explorer-share` additionally needs
`-DCPPWINRT_INCLUDE_DIR="<Windows SDK>/Include/<version>/cppwinrt"`.

Sources are UTF-8 without a byte order mark, so the MSVC builds pass `/utf-8`
rather than letting the compiler guess the active code page.
