# Explorer Properties

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


This independently downloadable Seer Canonical v1 Control package opens the
Windows Properties dialog for the current regular file. The helper always uses
the fixed non-destructive `properties` Shell verb.

The package interface in `plugin.json` is fixed:

- backend: `process`
- capability: `control`
- file matcher: `${type_file}`
- command: package-relative `shellverb_properties.exe`
- arguments: `--input ${input_file}`

Build and test the standalone package with:

```powershell
cd plugins/explorer-properties
cmake --preset default            # Ninja, Release; binaryDir outside the working tree (machine-local CMakeUserPresets.json)
cmake --build --preset default
ctest --preset default
```

Without the machine-local preset file, configure with an explicit
out-of-repo `-B` directory instead.

The package target runs the manifest contract test first, then writes the
release archive from a staged root that holds nothing else:

```powershell
cmake --build --preset default --target shellverb_package
```

The result is `dist/explorer-properties-1.2.0.zip`, whose root holds
`plugin.json`, the helper and `README.md`. `cmake --install "<your-build-dir>"
--prefix dist` still produces the same tree when an installed layout is what you
need.

The install step validates that the manifest and helper are present under the
same package root and that the fixed Properties contract is intact; it fails if
any of those checks do not pass. Create the archive only from the validated
`dist` directory.

Install the resulting archive through Seer's plugin installation flow. Verify
the following acceptance cases in a fresh Seer profile:

- A regular file opens its Windows Properties dialog.
- A filename containing Unicode characters opens its Properties dialog.
- A filename containing spaces opens its Properties dialog.
- A missing file is rejected without opening a dialog.
- The helper may exit while the Properties dialog remains open.
- Disabling the package prevents the control from running.
- Uninstalling the package removes the control.
- Reinstalling the package restores the control.

The launcher returns success only when a visible window exists in the process
named by the Shell, or in the helper itself when the Shell creates the sheet
there without returning a process handle. The window is the signal in both
cases: a Shell call that is accepted but produces no window within three
seconds is reported as a failure, never as a silent success. When the dialog is
hosted inside the helper, the worker pumps messages until it closes. The host applies no command timeout to a detached Control action, so
the dialog can stay open as long as the user needs.

The launcher owns the failure presentation. It creates a named reason channel
before the helper starts; the helper publishes its message there and the
launcher shows it in a system-owned message box. The launcher also writes the
reason to stderr so CTest can observe it under `SEER_TEST_NO_UI=1`. A shell
refusal or a dialog that never appeared therefore reaches the user as text
instead of a bare exit code.

Also verify that a file type whose Properties handler runs in an existing
Shell process does not report a three-second failure while its dialog is open.

Build from this repository with `plugins/common` present; its shared launcher
header is compiled into the executable and adds no package runtime dependency.
