# AGENTS.md

## 1. Project Identity

Seer-Controls is the official Control-plugin repository for
[Seer](https://1218.io/). Every shipped package is a Canonical v1 process
Control package: one `plugin.json`, one package-relative native helper
executable, `capabilities: ["control"]`, and a whole-file or whole-folder
matcher token in `extensions`.

Agent boundary:

- Keep changes local to the requested task and to this repository.
- Treat the Seer host repository as read-only reference material; check it out
  alongside this repository if you need it. Do not change the host's plugin
  schema, registry, lifecycle, installation-root behavior, Legacy
  compatibility, or Custom Controls to make a plugin work.
- Do not commit, push, create PRs, or modify remote state unless the user
  explicitly asks for it.

## 2. Repository Layout

- `plugins/<name>/` — one directory per deployable package. Each is a
  standalone CMake project configured and tested on its own.
- `plugins/common/` — header-only shared build code (`controlentry.h`,
  `shelluiworker.h`, `winui.h`, `wincmd.h`, `winpath.h`, `winproc.h`,
  `winclip.h`, `winexit.h`, `wintext.h`) plus `test/` (check harness,
  cross-process probe fixture, shared and manifest/staging assertions). It is
  NOT a deployable package and NOT a separately discoverable plugin.
  `controlentry.h` holds the `wWinMain` every Control helper shares; a package
  supplies only its own runner.
- `docs/` — implementation prompts, acceptance checklists, and review
  findings for the Control packages.

## 3. Critical Rule: Scratch and Writable Files

When a plugin helper needs a scratch location — temporary output, intermediate
files, extracted data, or any other writable working area — it MUST resolve
that location in this order, stopping at the first one that is usable:

1. **A location the Seer host assigns for this request, when it assigns one.**
   Canonical v1 passes the per-request directory to the helper through the
   `${output_dir}` / `${output_file}` placeholders (host side: the
   `ctrl_<uuid>` directory under the host's auto-delete temp root). A host
   assignment always wins, because it is the location the host manages and
   cleans.
   **Constraint:** the host deletes that directory (and the output files it
   tracks) as soon as the operation finishes or is cancelled. Use it only for
   files whose life ends with the request. Anything the user is meant to keep
   — retained results, reports, backups — belongs to step 2 or 3, never here.
2. **The plugin's own executable directory.** Otherwise use the directory the
   running helper lives in (the package folder), so package-scoped work stays
   package-scoped, survives between invocations, and a portable install keeps
   all of its state local to the package.
3. **The system temporary directory.** Only when the plugin's own directory is
   not writable or cannot be created (the usual case for a `Program Files` or
   read-only-media install), fall back to `GetTempPathW()`, namespaced as
   `<temp>\SeerControlPlugins\<packageName>`.

Additional constraints:

- Never hardcode any of these locations. Resolve them at runtime and probe
  writability before use: a directory can exist and still refuse writes, so
  existence is not the test. `WinPath::isWritableDirectory` creates a probe
  file and deletes it; `WinPath::resolveOwnedRoot` implements steps 2 and 3
  and reports which one it used and why the preferred one was rejected.
- If neither step 2 nor step 3 is writable, fail with a message that names
  both locations and the reason each was rejected. Never silently continue
  with no working directory.
- **Same-volume exception:** work that must be moved into the target's
  location with an atomic rename may use a scratch directory in the target's
  own directory, because a cross-volume move cannot be atomic (Image
  Optimization in-place mode does this). Such a scratch directory MUST be
  name-prefixed and ownership-marked (`WinPath::OwnedScratch`) so the package
  can recognize and reclaim it and never mistake an unrelated directory for
  its own.
- Never write into the Seer install directory, into the plugin root above the
  package's own folder, or into any location outside these choices without an
  explicit user request.
- Clean up scratch files the helper creates unless the package's documented
  contract says the output is retained.

## 4. Dependency and Packaging Policy

- Language floor: C++17 is the minimum standard (`CMAKE_CXX_STANDARD 17`); do
  not use post-C++17 features in package code.
- OS floor: Windows 10 is the minimum supported system. Helpers must not call
  APIs unavailable on Windows 10, and manifest `appMinVersion`/matcher choices
  must keep packages working on a Windows 10 baseline.
- Third-party libraries are allowed, but the existing packages depend only on
  the C++17 standard library and the Win32 API. Link the VC++ runtime
  statically (`MultiThreaded`) so each package ships as `plugin.json` plus one
  executable.
- **When a third-party library is needed, prefer one that can be statically
  linked or built from source**, so the dependency compiles into the single
  helper executable and the shipped package stays minimal (no extra DLLs, no
  additional runtime files). Avoid dependencies that force dynamically linked
  DLLs or external downloads; if no such library exists and a dynamic or
  external dependency is unavoidable, document why in the package README.
- The optimizers used by Image Optimization (pngquant, cwebp, jpegoptim) are
  the one deliberate exception: third-party executables that are never bundled
  or downloaded; the package reports which one is missing instead of guessing.
- Sources are UTF-8 without a byte order mark; MSVC builds pass `/utf-8`.
- Treat warnings as errors (`/W4 /WX`) in package targets.

### 4.1 Distributable ZIP packing

A package is shipped as a `.zip` that Seer installs (`PluginZipReader` reads it
with 7-Zip, `-tzip`). **Always pack it at the maximum compression level** — the
archive is downloaded and stored by end users, so size beats packing time:

- Preferred: 7-Zip, e.g.
  `7z a -tzip -mx=9 -mmt=on <package>.zip .\<stage-dir>\*`
- If PowerShell is the only tool available,
  `Compress-Archive -CompressionLevel Optimal` (its highest level).
- Keep the standard Deflate method. Do not switch to LZMA, Deflate64, PPMd, or
  zstd inside the ZIP even when the packer advertises a better ratio: the host
  reads archives with 7-Zip, but users and other tools may not.
- Pack the staged layout as it is — one consistent package root holding
  `plugin.json` and the helper. Never mix top-level and nested files; the host
  rejects an archive with an ambiguous package root.
- Run the packer from inside the stage directory and give the archive an
  absolute path, e.g.
  `cd dist && 7z a -tzip -mx=9 -mmt=on <abs-path>/<package>-<version>.zip *`.
  That is what keeps the root flat; a wildcard expanded in the package
  directory can store the stage directory name and break the package root.
- Refresh the stage directory explicitly before packing: copy the helper from
  the build tree and `plugin.json` from the source tree. Do not pack straight
  out of a `cmake --install` prefix, which can hold a stale manifest.
- Name the archive `<package directory>-<version>.zip`, e.g.
  `copy-folder-listing-1.0.0.zip`.
- `version` and `appMinVersion` live in `plugin.json` *and* in the package's
  manifest test, which asserts both. Change the value and the assertion in the
  same step; otherwise the package's `ctest` fails on the mismatch before any
  archive is built.
- After packing, verify the archive before shipping it: list the entries (flat
  root, Deflate), read `plugin.json` back out of the archive, and compare the
  helper's hash with the build output.

## 5. Build and Test

Each package is configured standalone. A machine-local `CMakeUserPresets.json`
in every package directory points the build out of the repository, to a
per-package directory outside the working tree (Ninja, Release). Run the
presets from inside the package directory; the Ninja generator needs the MSVC
environment on PATH (for example via a `vcvarsall`-equivalent script that puts
`cl.exe`, `ninja` and the Windows SDK headers on it):

```powershell
cd plugins/<name>
cmake --preset default        # configure: Ninja, Release, out-of-repo binaryDir
cmake --build --preset default
ctest --preset default
```

- `CMakeUserPresets.json` is machine-local and gitignored; on a machine
  without it, configure with an explicit out-of-repo `-B` directory instead.
- `explorer-share` additionally needs `CPPWINRT_INCLUDE_DIR` set to
  `<Windows SDK>/Include/<version>/cppwinrt`; the local preset supplies it.
- Every package has a manifest/staging test that validates its fixed package
  contract; run it before declaring a package done.
- Do not run Seer's full release runbook or full test suite from here.

## 6. Conventions

- One preview/host instance binds to one invocation; helpers never assume
  shared mutable state between invocations.
- Helpers that open a window use the two-process UI handoff (`winui.h` /
  `shelluiworker.h`): the launcher returns as soon as the worker signals
  readiness, and the worker owns the window for its whole lifetime so the
  host timeout never bounds user interaction.
- Completion is reported through the helper's exit code against the manifest's
  `success_exit_codes`; standard output is never rendered by the host.
- Each package README documents every option, the dependency policy, what a
  successful invocation means, what happens on cancellation, and a manual
  acceptance checklist. Update it whenever a package's behavior changes.
