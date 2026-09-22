# Seer Controls

Official control plugins for [Seer](https://1218.io/).

## Plugins

### Shipped

- [Explorer Open With](./plugins/explorer-open-with): Opens the Windows "Open With" dialog for the current regular file.
- [Explorer Properties](./plugins/explorer-properties): Opens the Windows Properties dialog for the current regular file.
- [Explorer Share](./plugins/explorer-share): Opens the Windows system Share UI for the current regular file.

### Control packages

These three packages implement the Seer Canonical v1 Control contract: one
`plugin.json`, one package-relative helper, `capabilities: ["control"]`, and a
whole-file or whole-folder matcher. Each has its own README documenting every
option, its dependency policy, what a successful invocation means, what happens
on cancellation, and a manual acceptance checklist.

- [Terminal Here](./plugins/terminal-here): Opens a terminal window in the current folder. Chooses between Windows Terminal, PowerShell 7, Windows PowerShell and Command Prompt, and documents the Windows Terminal `;` escaping that CRT quoting does not solve.
- [Copy Folder Listing](./plugins/copy-folder-listing): Copies a bounded, deterministic plain-text listing of the current folder's first level to the clipboard, one entry per line, with visible link, inaccessible and truncation markers.
- [Who Is Locking This File](./plugins/file-lock-info): A read-only Restart Manager report for the current file, copied to the clipboard.

## Shared code

`plugins/common` is shared build code, not a deployable package and not a
separately discoverable plugin. It is header-only and contains:

- `controlentry.h` — the `wWinMain` every Control helper shares: COM
  initialisation, command-line parsing and the "started with no command line"
  exit code.
- `shelluiworker.h` — the UI-readiness handoff used by the two controls that
  keep a system dialog open (Explorer Properties and Explorer Share): the
  launcher returns as soon as the dialog signals readiness, and the worker owns
  the dialog for its whole lifetime.
- `wincmd.h`, `winpath.h`, `winproc.h`, `winclip.h`, `winexit.h`, `wintext.h` —
  shared utilities used by every package. The helpers draw no UI of their own:
  a successful run is reported by its exit code, its result goes to the
  clipboard or a system dialog, and every failure reason is written to stderr,
  which the host renders as a failure toast.
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
