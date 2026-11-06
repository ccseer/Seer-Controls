# Explorer Open With

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


This is a Canonical v1 Control package for Seer. It opens the Windows Open With
dialog for the current regular file.

The package interface is fixed by `plugin.json`:

- backend: `process`
- capability: `control`
- file matcher: `${type_file}`
- command: package-relative `shellopenwith.exe`
- arguments: `["--input", "${input_file}", "${use_backslash}"]`

`${input_file}` and `${use_backslash}` are expanded by the host before the
helper is launched. `${use_backslash}` is a sentinel that asks the host for a
native-separator path; it is consumed by the host and never reaches the helper,
so the helper always receives exactly `--input <path>`.

Build and validate the package with:

```powershell
cd plugins/explorer-open-with
cmake --preset default            # Ninja, Release; binaryDir outside the working tree (machine-local CMakeUserPresets.json)
cmake --build --preset default
ctest --preset default
```

Without the machine-local preset file, configure with an explicit
out-of-repo `-B` directory instead.

Create a distributable package with:

```powershell
cmake --build --preset default --target openwith_package
```

The package target runs the manifest contract test first and then writes
`dist/explorer-open-with-1.2.0.zip`, whose root holds `plugin.json`, the helper
and `README.md`. `cmake --install "<your-build-dir>" --prefix dist` still
produces the same tree when an installed layout is what you need.

The manifest test stages the CMake-built `shellopenwith.exe` with a
CMake-controlled copy of `plugin.json`. It verifies the package contract and
path containment without requiring an installed release executable.

The unit test drives the rejection paths through an injected error sink rather
than the real stderr, because a passing run must leave the test process's own
stderr empty. The `wWinMain` overload supplies the real sink, which shows the
refusal in a system-owned message box and also writes it to stderr. The
process-level checks spawn the helper with `SEER_TEST_NO_UI=1` and read its
redirected stderr to prove the refusal stays observable in tests.
