# Explorer Open With

This is a Canonical v1 Control package for Seer. It opens the Windows Open With
dialog for the current regular file.

The package interface is fixed by `plugin.json`:

- backend: `process`
- capability: `control`
- file matcher: `${type_file}`
- command: package-relative `shellopenwith.exe`
- arguments: `--input ${input_file}`

Build and validate the package with:

```powershell
cmake -S plugins/explorer-open-with -B plugins/explorer-open-with/build -G "Visual Studio 17 2022" -A x64
cmake --build plugins/explorer-open-with/build --config Release
ctest --test-dir plugins/explorer-open-with/build -C Release --output-on-failure
```

Create a distributable package with:

```powershell
cmake --install plugins/explorer-open-with/build --config Release --prefix plugins/explorer-open-with/dist
Compress-Archive -Path plugins/explorer-open-with/dist/* -DestinationPath plugins/explorer-open-with/explorer-open-with-1.0.0.zip
```

The manifest test stages the CMake-built `shellopenwith.exe` with a
CMake-controlled copy of `plugin.json`. It verifies the package contract and
path containment without requiring an installed release executable.
