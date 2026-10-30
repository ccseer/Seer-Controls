# Hex View

This independently downloadable Seer Canonical v1 Control package renders a
classic 16-byte hex dump of any file — offset column, hexadecimal byte columns
with a double gap between bytes 7 and 8, and a decoded ASCII column — and
presents it in a separate `SeerViewer.exe` window. It is a read-only
inspection: the input file is opened for reading only, and cloud-offline
placeholders are refused before any handle exists so a multi-gigabyte
background download can never be triggered.

The helper follows the repository's only heavy-helper model, the OCR package:
it launches `SeerViewer.exe --wait-file` first so the independent window shows
its native pending state at once, then generates the dump and publishes it
atomically. The host is launch-only (`ControlDetachedLauncher` via
`QProcess::startDetached`); everything that happens after the launch succeeds
is the helper's own responsibility and is reported through the dump file, not
through exit codes or stderr.

The package interface in `plugin.json` is fixed:

- backend: `process`
- capability: `control`
- file matcher: `${type_file}` (any file; folders never match)
- command: package-relative `seer_hex.exe`
- arguments: `${input_file}`, `${seerviewer_exe}`, `--max-kb=64`,
  `--wait-timeout=30000`
- no `icon_path`: the control bar takes the button icon from the helper
  executable's embedded icon resource (`GetFileIcon`), exactly like the OCR
  package; `res/hex.png` stays in the repository as the design source for
  `res/hex.ico` and is not shipped
- `timeout_ms`: 60000
- `success_exit_codes`: `[0]`
- `close_after_success`: `false` — the launch succeeds, the main preview stays
  open. `timeout_ms`/`success_exit_codes` exist for schema completeness and the
  manifest test only; the host never sees the helper's exit code.

## Options

| Option | Values | Default | Meaning |
|---|---|---|---|
| `<input_file>` | path | required (pos 1) | The file to dump. |
| `<seerviewer_exe>` | path | required (pos 2) | `SeerViewer.exe` to present the pending/result window; its existence is verified first. |
| `--max-kb` | integer, clamped to [1, 1048576] | `64` | Hard cap on dumped bytes (1 GiB maximum). `total > cap` renders the Truncated footer; `total <= cap` (including exactly at the cap) renders Fully displayed. |
| `--case` | `upper`, `lower` | `upper` | Case of hex digits in offsets, bytes and column labels. |
| `--uppercase` / `--lowercase` | flags | off | Shorthands for `--case`; neither combines with `--case` or with each other. |
| `--wait-timeout` | integer, clamped to [5000, 600000] | `30000` | Passed to `SeerViewer.exe --wait-file`; the viewer shows its own timeout page on expiry. |

Both `--key=value` and `--key value` spellings work; options may appear
anywhere around the two positionals. Option names start with `-` only (the
`/key` spelling is not an option prefix, so a rooted path such as `/dir/file`
stays a positional), and a bare `--` ends the option list, which is how a
positional that itself starts with `-` (a legal Win32 name, e.g. `-sample.bin`)
is passed. Unknown options, duplicates, conflicting case options, empty or
non-numeric values, and a positional count other than two are refusals with
exit code 2.

## Output location and naming

Dumps live under the owned root's `dumps\` subdirectory per the repository's
scratch rule (AGENTS §3): the package directory when writable, otherwise
`<temp>\SeerControlPlugins\hex-viewer\dumps\`, and a hard failure naming both
candidates when neither is writable. The file name is
`<16-hex FNV-1a of the normalized absolute path> - <capped base name>.hextxt`.
The hash is case-sensitive (NTFS `A.bin` and `a.bin` stay distinct) and
separator/prefix-insensitive; the base name is middle-capped at 64 characters
with surrogate-safe boundaries, and Win32-reserved characters in it become `_`
(a drive root has no base name; an NTFS stream path such as `file.bin:stream`
would otherwise turn the dump into a stream of a stray zero-length file).
When the full output path would reach 240 characters the embedded name shrinks
to 32 first, then the temp root is tried, and the run fails rather than emitting
an unusable path. The tmp file is `<txt>.<pid>_<nonce>.tmp`, unique per
invocation, created and renamed through the extended-length prefix.

Output text is UTF-8 without BOM, CRLF line endings, one 85-byte row per 16
bytes (83 visible characters: 8 offset digits, a 5-space gap, the 16-byte grid,
a 4-space gap, then the decoded column). A dump costs 5.31 bytes of text per
source byte, so the default 64 KB cap is about 4,096 rows / 340 KiB while the
1 GiB maximum is about 5.31 GiB of text — the cap is a disk-space decision, not
a viewer one.

## What a successful invocation means

Exit 0 means a dump was published at the target path — the dump may be hex
data, or one of the diagnostic blocks below. Diagnostics are successful
results by design: the waiting viewer must always get text instead of timing
out.

- **Cloud-offline placeholder** (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, mirroring
  the host's `IsFileOnCloud`): refused before opening, never read. The block
  reads `Error: Online-only cloud file (Recall on data access).` plus a `Hint:`
  line.
- **Cannot open** (missing, access denied, share-locked): block with
  `Error: Cannot open file for reading (<reason>)`; `<reason>` is one of three
  fixed values (`file not found`, `access denied`, `file is locked by another
  process`) with the system message as fallback.
- **Mid-read failure**: rows read so far are kept and a
  `[Read Error] Stopped at 0x…: <reason>` footer replaces the normal footer.
- **Empty file**: header with `Showing 0 bytes` and `[Empty File: 0 Bytes]`.
- **0 < total <= cap**: `[End of File: Fully displayed] (Total: <grouped>)`.
- **total > cap**: `[Truncated] Display capped at <N> KB … (Total: <unit>)`.

Publication is atomic: the staging file is created exclusively, streamed,
flushed, then renamed over the target with three 50 ms-backoff attempts. If
the rename still fails (target held without `FILE_SHARE_DELETE`), a one-line
`[Seer Hex Dump Error] Cannot write dump file (<reason>).` is written directly
over the target as the last resort. Only when even that direct write fails
does the helper show an error dialog and exit 1 — at that point the waiting
viewer shows its own timeout page, which is the accepted residual risk for a
helper the host launched detached.

## Cancellation semantics

There is nothing to cancel: the host does not wait for the helper, and the
helper does not wait for the viewer. A second trigger on the same file runs
concurrently; each invocation uses its own tmp file and deletes the previous
same-name dump before launching, so the viewer's 3-second mtime slack can
never serve a stale result. When that previous dump is still held without
delete sharing, the run refuses before launching (exit 1, error dialog) rather
than opening a window onto the old content, because the publish step could not
replace the file either. `SeerViewer.exe` windows are independent; closing one
does not affect the helper or the main preview.

## Housekeeping

Every run sweeps its dumps directory at the very end (never before the first
frame): `*.hextxt` and orphan `*.tmp` files older than seven days by last
write time are deleted best-effort, with read-only bits cleared first.
Directories, reparse points and anything else are untouched.

## Zero-dependency policy

The helper is a single `/MT`-statically-linked C++17 WIN32 executable using
only the Win32 API and the C++17 standard library — no third-party code, no
CRT DLLs, no models, no resources beyond the embedded icon. `dumpbin
/dependents seer_hex.exe` must show exactly `KERNEL32.dll`, `USER32.dll`,
`ole32.dll`, `SHELL32.dll`. `ole32` serves the shared `ControlEntry` COM init,
`shell32` serves `CommandLineToArgvW`/`ShellExecuteExW`, `user32` serves the
error `MessageBoxW`. Release build size is about 380 KB, roughly 105 KB of
which is the embedded multi-size icon.

## Test hook: SEER_TEST_NO_UI

`SEER_TEST_NO_UI=1` (exact value `1`) switches every helper refusal from the
`MessageBoxW` error dialog to a `WinText::writeStandardError` line on stderr
(ACP-encoded) with the same exit code. It exists so CTest can assert the
failure paths — exit 2 with invalid arguments, exit 1 with a missing viewer —
without a modal window hanging the run. It is a package-internal test hook,
not a user option, and is deliberately not documented in the shipped
`README.md`. Note the helper is a WIN32-subsystem process: without
`STARTF_USESTDHANDLES` redirection the stderr write is silently dropped, so
automated tests must launch it with piped std handles.

## Layout

- `src/hexformat` — pure formatting: rows, column header, header/footer
  blocks, size ladder, cloud predicate. 85-byte stack buffer per row (83
  visible characters plus CRLF), no per-row allocation.
- `src/hexoptions` — pure parsing, conflict checks, clamping.
- `src/hexpaths` — FNV-1a hash, surrogate-safe capping, deterministic output
  path decision.
- `src/atomicfilewriter` — staging/flush/rename/direct-write, structured
  errors only, no UI.
- `src/hexdump` — attributes → cloud intercept → open → header → 64 KB blocks
  → footer; injectable read seam for tests.
- `src/hexlauncher` — `CreateProcessW` then `ShellExecuteExW`.
- `src/hexsweep` — seven-day sweep of the dumps directory.
- `src/main.cpp` — `ControlEntry::run` wiring and the single UI/exit boundary.
- `syntax/hextxt.xml` — KSyntaxHighlighting `HexDump` definition (`*.hextxt`,
  `version="2"`, `priority="100"`, `kateversion="5.0"`) staged under `syntax/`
  so Seer's text viewer claims and highlights dump files. The host validates
  these at install: a missing or too-new `kateversion`, a wildcard padded with
  whitespace, a non-positive `priority` or a file over 512 KB makes the install
  fail instead of shipping a definition that never claims anything.
- `test/` — pure-logic tests, file-level golden integration tests with an
  injectable reader, a `fake_viewer` that records its argv, and the staged
  manifest contract test.

## Manual acceptance checklist

Run on a Seer 4.8.0 build (or a dev build configured with
`-DSEER_AP_VERSION_MAJOR=4 -DSEER_AP_VERSION_MINOR=8 -DSEER_AP_VERSION_PATCH=0`):

- [ ] The control bar shows the hex icon taken from the helper executable's
      embedded icon resource, like every OCR-family button.
- [ ] Clicking opens the independent SeerViewer window with the spinner first,
      then the text.
- [ ] Clicking twice within 3 seconds shows fresh content both times.
- [ ] The three columns are vertically aligned: `00` over byte 1, `0F` over
      byte 16, `Decoded Text` over the `|` boundary; selection and copy work.
- [ ] A > 1 GB file truncates at the cap with the `[Truncated]` footer and the
      real total size.
- [ ] `--max-kb=128` shows `first 128 KB` / `capped at 128 KB` and 8,192 rows.
- [ ] A file exactly at the cap shows `[End of File: Fully displayed]`.
- [ ] An exclusively locked file shows the `[Seer Hex Dump Error]` block at
      once, no 30-second hang.
- [ ] An OneDrive online-only placeholder shows the cloud diagnostic, no
      background download starts.
- [ ] An empty file shows `[Empty File: 0 Bytes]`, exit 0.
- [ ] A missing `${seerviewer_exe}` pops the `Seer Hex View Error` message box
      and exits 1.
- [ ] A 255-character file name is dumped correctly via the extended prefix.
- [ ] A target txt held by another process ends with the one-line diagnostic
      in the window; if even the fallback write fails, the timeout page plus
      the message box is the accepted outcome.
