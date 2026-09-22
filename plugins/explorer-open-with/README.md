# Explorer Open With

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
cmake --install "<your-build-dir>" --prefix dist
Compress-Archive -Path dist/* -DestinationPath explorer-open-with-1.1.0.zip
```

The manifest test stages the CMake-built `shellopenwith.exe` with a
CMake-controlled copy of `plugin.json`. It verifies the package contract and
path containment without requiring an installed release executable.

The unit test drives the rejection paths through an injected error sink rather
than the real stderr, because a passing run must leave the test process's own
stderr empty; only the helper process writes a host-facing refusal there. The
`wWinMain` overload supplies the real sink, and the process-level checks spawn
the helper and read its redirected stderr to prove the refusal still reaches the
host.
