# Copy Folder Listing

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


This independently downloadable Seer Canonical v1 Control package copies a
flat listing of the current folder's first level to the clipboard. It is a
whole-matcher Control: it appears for folders, not for files.

The package interface in `plugin.json` is fixed:

- backend: `process`
- capability: `control`
- folder matcher: `${type_folder}`
- command: package-relative `copy_folder_listing.exe`
- arguments: `--input ${input_file}`
- `timeout_ms`: 120000
- `success_exit_codes`: `[0]`

## Options

| Option | Values | Default | Meaning |
|---|---|---|---|
| `--input <directory>` | an existing folder | required | The folder to list. |
| `--include-hidden` | flag | off | Include entries with the hidden attribute. |
| `--exclude <pattern>` | `*` and `?` wildcards | none | Skip matching entry names. May be given more than once. |
| `--max-entries <n>` | integer > 0 | `5000` | Maximum rendered entries. |
| `--max-bytes <n>` | integer > 0 | `1048576` | Maximum rendered size, counted in **UTF-8 bytes**, separator included. |
| `--separator <value>` | named form, escape sequence or literal text | `crlf` | Written after every entry, including the last. Named forms: `crlf`, `lf`, `cr`, `tab`, `space`, `comma`, `semicolon`, `pipe`, `none`; `\n`, `\r`, `\t` and `\\` are expanded; at most 16 characters. |

These defaults are product defaults of this helper, not host limits. The
manifest deliberately does not pin them, so a user can change any of them in
Seer's Arguments editor.

Named separators exist because the host passes arguments as JSON strings: a
literal `\t` would have to be written `\\t` there to reach the helper as an
escape. `--separator comma` and `--separator "\\t"` both work, and a value that
matches no name is taken literally, so `--separator " | "` keeps its spaces.
`--separator none` concatenates the entries with nothing between them.

## Scope: one level, never recursive

Only the immediate children of the selected folder are listed. Subfolder
contents are never reached, so the result always matches what a single
Explorer view shows, and a huge tree cannot make the helper slow: there is no
walk below the selected folder.

## Skipped entries

There are no default exclusions. Hidden entries are skipped unless
`--include-hidden` is given, and `--exclude` patterns remove matching names.
Skipped entries leave no trace in the output; the entry and byte limits produce
the one visible omission marker (`... [truncated: ...]`).

## Output shape

The clipboard receives plain text, one entry per line and nothing else. Folders
come first, then files, each group ordered by a case-insensitive ordinal
comparison, so the same folder always produces the same text. Folders carry a
trailing `/`. Every entry, including the last, is followed by the separator,
which is `\r\n` unless `--separator` says otherwise:

```
src/
core/
ui/
README.md
```

Markers:

| Marker | Meaning |
|---|---|
| `name -> [link, not followed]` | a symbolic link, junction or AppExecLink. Listed, never followed. |
| `... [truncated: ...]` | the entry or byte limit stopped the output. |

The byte limit keeps the text bounded and is counted in UTF-8 bytes, the way a
file or a pasted message would be measured; Windows itself holds clipboard text
as UTF-16 (`CF_UNICODETEXT`), so counting UTF-16 units would silently allow about
twice as much text as the option says. The truncation line itself is always
written, even when the limit is already reached, so a truncated result is never
mistaken for a complete one.

## Success and failure

The whole bounded listing is built in memory before the clipboard is touched, and
success is reported only after the clipboard accepts the text. An empty folder
succeeds without touching the clipboard: there is nothing to copy, and the
previous clipboard content is left alone. A non-empty folder whose every entry
is skipped as hidden or excluded is different: it fails with exit `1` and a
visible reason instead of silently keeping the old clipboard content.

| Exit code | Meaning |
|---|---|
| `0` | the text was placed on the clipboard (or the folder was empty and the clipboard was left untouched) |
| `1` | the folder could not be read, every entry was skipped as hidden or excluded, or the clipboard refused the text |
| `2` | invalid arguments |
| `5` | the selection is itself a link; links are never followed |
| `6` | the selection is not an existing folder |

The helper opens no window of its own on success: it exits `0` silently and
the clipboard holds the result. On failure the helper shows a system-owned
message box with the reason and also writes it to stderr for tests; Seer shows
no failure toast because it stopped observing the helper at launch. Nothing is
ever copied from standard output, and no listing text is ever rendered anywhere:
if the clipboard refuses it, the message says to retry, or to retry with a
smaller result (`--max-entries` / `--max-bytes`). The exit codes above are the
helper's own contract; the host does not read them.

## Host integration: `close_after_success`

The manifest declares `"close_after_success": false`. The host default is
`true`: a successful detached launch hides the preview window. This key keeps
the window open after a copy. An explicit per-entry choice in Seer's control
settings still wins over the manifest value. The key is a plain JSON boolean
and is only accepted for packages with the `control` capability.

The clipboard is a shared resource, so a temporarily busy clipboard is retried
for up to three seconds. The previous clipboard content is not cleared before
the replacement allocation is ready, so a failure to allocate cannot destroy it;
once the write succeeds the system owns the data. A clipboard API failure after
the clipboard has been emptied cannot be rolled back, and this package does not
claim otherwise.

Cancellation: there is nothing for the host to cancel. It does not track the
helper after the launch, so the helper owns its whole lifetime. It holds no
child process and writes only to the clipboard and the current clipboard
owner's memory. The listing is bounded by `--max-entries` and `--max-bytes`,
the latter counting the separator bytes along with the names;
`timeout_ms` is not enforced by the host. The listing is read-only: no file
content is read, only names and attributes.

## Known limitations

- A folder with an extremely large single directory must be enumerated and sorted
  before the entry limit can apply. That is slower than the rendered output
  suggests, and is the reason for the two-minute manifest budget.
- Only `*` and `?` are supported in `--exclude`.

## Dependencies

No third-party dependency. The package contains only `plugin.json` and
`copy_folder_listing.exe`; the VC++ runtime is linked statically, so `dumpbin
-dependents` on the shipped helper lists only `ole32`, `SHELL32`, `USER32` and
`KERNEL32`.

## Build and test

```powershell
cd plugins/copy-folder-listing
cmake --preset default            # Ninja, Release; binaryDir outside the working tree (machine-local CMakeUserPresets.json)
cmake --build --preset default
ctest --preset default
```

Without the machine-local preset file, configure with an explicit
out-of-repo `-B` directory instead.

`folderlisting_test` covers wildcard matching, every option including the rejected
values, ordering, the one-level-only scope, hidden and excluded entries, both
limits, links taken from real reparse points the operating system provides, a
failing clipboard writer that must preserve the generated result, the failure
paths (a missing folder, an empty folder that must not touch the clipboard), and
a real clipboard round trip. It also launches the real helper executable once to
pin the process contract: under `SEER_TEST_NO_UI=1` a usage refusal reaches
standard error with an empty standard output and the documented exit code. `folderlisting_manifest_test`
loads the staged package the way the host loader does.

The clipboard assertions replace the clipboard and restore the previous text when
the previous content was text; a run whose clipboard holds non-text content (an
image, a file list, rich text) is skipped instead, because that content cannot be
restored from text alone.

## Install and acceptance

```powershell
cmake --build --preset default --target folderlisting_package
```

The package target runs the manifest contract test first, so a field-level
contract violation never produces an archive that looks publishable, and then
writes `dist/copy-folder-listing-1.1.0.zip` with `plugin.json`, the helper and
`README.md` at the root. `cmake --install "<your-build-dir>" --prefix dist`
still produces the same tree when an installed layout is what you need.

The install step itself only proves the shipped files are present; the
field-level package contract lives in `folderlisting_manifest_test`, so the
`ctest` run above is a required part of publishing, not an optional check.

Verified in this repository:

- The listing is deterministic across runs, ordered folders-first and
  case-insensitively, and never contains entries below the selected folder.
- A real folder holding AppExecLink reparse points rendered every link with the
  "not followed" marker, and the marked lines match an independent enumeration.
- A failing clipboard writer returned a failure while preserving the generated
  text, and a real clipboard write was read back successfully after the write.
- An empty folder succeeded without calling the clipboard writer.
- A helper started without arguments wrote its refusal to standard error, kept
  standard output empty, and exited with the usage code.

Manual acceptance still required on a real Seer installation:

- Copy the listing of a medium project folder and paste it into an editor;
  confirm the clipboard holds plain text, one entry per line, first level only.
- Copy the listing of a folder containing a reparse point and confirm the link
  is listed with the marker.
- Run the action on an access-denied folder and confirm the helper shows its
  own error dialog and the clipboard is left unchanged.
- Set `--max-entries 10` on a large folder and confirm the truncation note
  appears in the pasted text.
- Hold the clipboard open from another application and confirm the helper retries
  and then reports a visible failure rather than silently doing nothing.


## TODO:
- options
  - inlcude abslute path?