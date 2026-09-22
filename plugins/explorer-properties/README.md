# Explorer Properties

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

Install the distributable files into one package root, then create the release
archive from that root only:

```powershell
cmake --install "<your-build-dir>" --prefix dist
Compress-Archive -Path dist/* -DestinationPath explorer-properties-1.1.0.zip
```

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

The launcher returns success once the system UI is ready. A separate helper
process keeps the UI alive until it closes, so Seer's command timeout does not
limit how long the user can interact with the dialog.

Seer renders the launcher's stderr only, and that helper process is spawned
detached without handle inheritance, so a refusal it raises cannot be written
straight to the host. The launcher creates a named reason channel before the
helper starts; the helper publishes its message there and the launcher re-emits
it on its own stderr, which is what lets a shell refusal or a dialog that never
appeared reach the user as text instead of a bare exit code.

Build from this repository with `plugins/common` present; its shared launcher
header is compiled into the executable and adds no package runtime dependency.
