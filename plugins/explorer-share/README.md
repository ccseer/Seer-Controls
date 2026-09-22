# Explorer Share Control

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
cmake --install "<your-build-dir>" --prefix dist
Compress-Archive -Path dist/* -DestinationPath explorer-share-1.1.0.zip
```

The manifest test stages the CMake-built `seer_share.exe` with a
CMake-controlled copy of `plugin.json`. It verifies the package contract and
path containment without requiring an installed release executable.

The launcher returns success once the system UI is ready, and a separate helper
process keeps the UI alive until it closes, so Seer's command timeout does not
limit how long the user can interact with the dialog.

The timeout does bound the handoff that happens *before* readiness, though. This
worker waits up to 30 seconds for WinRT to hand over the share payload and cannot
signal readiness before it has, and that wait is spent inside the launcher's own
readiness window rather than added to it. The two therefore have to agree: the
payload wait must fit inside the readiness window the launcher allows, and
`timeout_ms` must outlast the launcher, which is the process the host bounds. See
`kSharePayloadWaitMs`, `handoffOptions()`, and
`ShellUiWorker::totalHandoffBudgetMs()`; the manifest test asserts both
relationships so the numbers cannot drift apart.

A consequence worth knowing: the worker shows the window the share UI is anchored
to before it waits for the payload, and the launcher treats any visible window as
readiness. In practice the launcher therefore returns success as soon as the
worker is up, so a failure raised after that point -- an unavailable share pane,
or the payload timeout -- is reported to nobody, because the host has already
been told the action succeeded.

Build from this repository with `plugins/common` present; its shared launcher
header is compiled into the executable and adds no package runtime dependency.
