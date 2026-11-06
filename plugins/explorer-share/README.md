# Explorer Share Control

## Host execution model

Seer starts Control helpers with `QProcess::startDetached`. A launch counts as
successful as soon as the operating system accepts it; the host does not wait
for the process, does not read its exit code, does not capture stdout or stderr,
applies no `timeout_ms`, and never kills the helper's process tree. Success is
silent: only a launch-time failure produces a host toast.

This helper therefore presents every failure it raises in its own system-owned
message box. The stderr write is kept as a test-observable diagnostic channel;
CTest sets `SEER_TEST_NO_UI=1` so refusals stay on stderr instead of opening a
modal dialog. `timeout_ms` and `success_exit_codes` remain in the manifest for
schema completeness and the manifest test, and `appMinVersion` is `4.8.0`, the
first host with this execution model. `close_after_success` is `false` so the
preview window stays open after the launch.


This is a Canonical v1 Control package for Seer. It opens the Windows system
Share UI for the current regular file using Windows Runtime DataTransferManager.

The package interface is defined by `plugin.json`:

- backend: `process`
- capability: `control`
- file matcher: `${type_file}`
- command: package-relative `seer_share.exe`
- arguments: `--input ${input_file}`

Build and validate the package with:

```powershell
cd plugins/explorer-share
cmake --preset default            # Ninja, Release; binaryDir outside the working tree (machine-local CMakeUserPresets.json, which also supplies CPPWINRT_INCLUDE_DIR)
cmake --build --preset default
ctest --preset default
```

Without the machine-local preset file, configure with an explicit
out-of-repo `-B` directory and pass
`-DCPPWINRT_INCLUDE_DIR="<Windows SDK>/Include/<version>/cppwinrt"`.

Create a distributable package with:

```powershell
cmake --build --preset default --target share_package
```

The package target runs the manifest contract test first and then writes
`dist/explorer-share-1.2.0.zip`, whose root holds `plugin.json`, the helper and
`README.md`. `cmake --install "<your-build-dir>" --prefix dist` still produces
the same tree when an installed layout is what you need.

The manifest test stages the CMake-built `seer_share.exe` with a
CMake-controlled copy of `plugin.json`. It verifies the package contract and
path containment without requiring an installed release executable.

The launcher returns success once the system UI is ready, and a separate helper
process keeps the UI alive until it closes. The host applies no command timeout
to a detached Control action, so the dialog can stay open as long as the user
needs.

The readiness deadline below is the plugin's own handoff budget, not a host
timeout. This worker waits up to 30 seconds for WinRT to hand over the share
payload and cannot signal readiness before it has, and that wait is spent inside
the launcher's own readiness window rather than added to it. The two therefore
have to agree: the payload wait must fit inside the readiness window the
launcher allows. `timeout_ms` in the manifest is not enforced by the host; it
remains for schema completeness and the manifest test. See
`kSharePayloadWaitMs`, `handoffOptions()`, and
`ShellUiWorker::totalHandoffBudgetMs()`; the manifest test asserts the
relationship so the numbers cannot drift apart.

A consequence worth knowing: the worker shows the window the share UI is anchored
to before it waits for the payload, and the launcher treats any visible window as
readiness. In practice the launcher therefore returns as soon as the worker is
up, so a failure raised after that point -- an unavailable share pane, or the
payload timeout -- is presented by the worker through the reason channel in the
launcher's own message box; the host is never involved after the launch.

Build from this repository with `plugins/common` present; its shared launcher
header is compiled into the executable and adds no package runtime dependency.
