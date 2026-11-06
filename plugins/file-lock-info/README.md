# Who Is Locking This File?

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


This independently downloadable Seer Canonical v1 Control package shows which
applications and services are currently using the selected file. It is a
read-only inspection: nothing is shut down, restarted, or modified. The report
is shown in a system-owned modal dialog; the plugin draws no window of its own.

The package interface in `plugin.json` is fixed:

- backend: `process`
- capability: `control`
- file matcher: `${type_file}`
- command: package-relative `file_lock_info.exe`
- arguments: `--input ${input_file}`
- `timeout_ms`: 120000
- `success_exit_codes`: `[0]`

## Options

| Option | Values | Default | Meaning |
|---|---|---|---|
| `--input <file>` | an existing regular file | required | The file to inspect. |
| `--admin` | flag | absent | Query with administrator rights, after an explicit consent prompt. |

There is no `admin` property in `plugin.json`. `--admin` is an argument a user
adds in Seer's Arguments editor, and it is the only way to query elevated: the
report dialog has a single OK button, so elevation happens only through this
argument. Elevation is always explicit and consent-driven, and nothing is ever
elevated automatically.

## What the report shows

The dialog shows plain text: the file, up to three holders in full (to keep the
modal message box within standard display bounds, with an omitted count note when
more holders exist), and the standing limitation note. For every user the Restart
Manager reports:

- the kind of holder (application, service, Windows Explorer, console, or
  critical system process) and its registered name,
- the process identifier,
- the process start time when it can be read,
- the executable path when it is readable at the current privilege level.

Nothing is written to the inspected file, and no process is touched. The
dialog has nothing to refresh or configure: run the control again for a fresh
query.

## How the result is presented

The report is built in memory in full and then shown with one system call
(`MessageBoxW`, `MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND`), so the
plugin draws no window of its own and copies nothing to the clipboard. The
dialog is modal: the helper exits only once the user dismisses it. The host
applies no `timeout_ms` to a detached Control action, so the report can stay
open as long as the user needs. A query that failed is not a report: it opens
the helper's own error message box and also writes the reason to stderr for
tests, so an error can never read like a result.

## Wording of the result

An empty result reads:

```
No application reported.
A driver, a protected process or another user session may still be using this file.
```

The second line appears only in that case. A populated report lists the
applications and adds nothing else, because the caveat is only useful to
somebody looking at an empty result.

`Executable: not accessible` means the process could not be opened at the current
privilege level. A report with at least one such line ends with `Some details need
administrator rights.`; when the query already ran elevated it says those details
are not readable even with administrator rights, instead of suggesting another
elevation.

## Process identity

The query reports each holder's identifier together with the process start time
it observed. Before showing a name or path, the helper reads the process's own
start time and compares it. When they differ, the identifier was recycled, so the
details are withheld and the entry says the process identifier changed while the
list was read; run the control again to query again. This prevents attributes of
an unrelated process from being presented as the holder.

## Bounded query

`RmGetList` is called with a growing buffer: it starts at 16 entries and grows to
the reported requirement, capped at 4096 entries and six attempts. The reported
entry count is validated against the allocated capacity before it is used, and a
count larger than the buffer is clamped rather than trusted. If the list keeps
growing past the cap, the first 4096 entries are reported together with a note
saying more were available; if the list changes without the buffer ever being
large enough, the query fails with an instruction to run the control again. The
session is ended on every exit path, including the failure paths.

## Success and failure

| Exit code | Meaning |
|---|---|
| `0` | the report was shown in the dialog |
| `1` | the query failed or elevation failed |
| `2` | invalid arguments |
| `4` | the consent prompt was cancelled |
| `5` | the Restart Manager is not available on this system |
| `6` | the selection is not an existing regular file |
| `7` | the elevation request did not complete in time |

The helper runs the query, shows the modal report dialog and exits once the
user dismisses it. Every failure is shown by the helper in its own system
message box and also written to stderr for tests; Seer shows no failure toast
because it stopped observing the helper at launch. The exit codes above are the
helper's own contract; the host does not read them.

Cancellation: there is nothing for the host to cancel. It neither waits for nor
terminates the helper's process tree. The inspection holds no child process and
never modifies the file.

## What this package deliberately does not do

It never calls `RmShutdown` or `RmRestart` in any form, never kills or suspends a
process, never closes a handle owned by another process, never injects into a
process, and never installs a driver. It reads the file's attributes and opens
process handles only for `PROCESS_QUERY_LIMITED_INFORMATION`. It draws no UI of
its own: the report goes to one system message box, and every failure message
comes from the host.

## Dependencies

No third-party dependency. The one non-default system import is `Rstrtmgr.dll`,
the Windows Restart Manager. The package contains only `plugin.json` and
`file_lock_info.exe`, and the VC++ runtime is linked statically, so `dumpbin
-dependents` on the shipped helper lists only `ole32`, `SHELL32`, `USER32`,
`ADVAPI32`, `KERNEL32` and `Rstrtmgr.dll`.

## Build and test

```powershell
cd plugins/file-lock-info
cmake --preset default            # Ninja, Release; binaryDir outside the working tree (machine-local CMakeUserPresets.json)
cmake --build --preset default
ctest --preset default
```

Without the machine-local preset file, configure with an explicit
out-of-repo `-B` directory instead.

`filelock_test` covers argument parsing and every rejected value, the report
wording for the empty, populated, truncated, identity-changed, elevated and
failed cases, the real Restart Manager backend against a file nobody holds and
against a file held exclusively by a controlled process, and the failed-query
contract of the presentation: a query that failed opens no result dialog, exits
with the query's own code and explains itself in the helper's error dialog. The
dialog itself is modal, so the success path is covered by the manual
acceptance list below instead of an automated check.

## Install and acceptance

```powershell
cmake --build --preset default --target filelock_package
```

The package target runs the manifest contract test first and then writes
`dist/file-lock-info-1.1.0.zip`, whose root holds `plugin.json`, the helper and
`README.md`. `cmake --install "<your-build-dir>" --prefix dist` still produces
the same tree when an installed layout is what you need.

Verified in this repository:

- The real Restart Manager reported the controlled process that opened the test
  file exclusively, with its identifier, in the generated report.
- A file nobody holds produced the exact documented empty wording, and a failed
  query was never presented as a result: it exits with the query's own code and
  its reason goes to the failure channel.

Manual acceptance still required on a real Seer installation:

- Enable the package, open a document in another application, and confirm the
  report dialog names that application and its executable path.
- Query a file that no one holds and confirm the exact "No application reported."
  wording and the single limitation line below it appear in the dialog.
- Run the control again after opening the file in a second application and
  confirm the new holder appears in the dialog.
- Confirm every holder is listed with its name, identifier, executable path and
  start time, and that an unreadable path adds nothing beyond the single
  administrator line at the end.
- Add `--admin` in Seer's Arguments editor and confirm the UAC prompt appears,
  that cancelling it leaves Seer open and produces the helper's own error dialog
  explaining that nothing was queried, and
  that accepting it shows a report resolved at the elevated privilege level.
- Confirm the helper exits as soon as the dialog is dismissed, and that the
  report can stay open as long as the user needs: the host applies no timeout.
- Confirm disabling the package removes the control and uninstalling removes the
  files.
