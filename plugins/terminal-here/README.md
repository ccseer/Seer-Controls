# Terminal Here

This independently downloadable Seer Canonical v1 Control package opens a
terminal window in the current folder. It is a whole-matcher Control: it appears
for folders, not for files.

The package interface in `plugin.json` is fixed:

- backend: `process`
- capability: `control`
- folder matcher: `${type_folder}`
- command: package-relative `terminal_here.exe`
- arguments: `--input ${input_file}`
- `timeout_ms`: 150000
- `success_exit_codes`: `[0]`

`${type_folder}` is used because `${type_folder}` is the folder token the host
matcher understands. No `${output_file}`, `${output_dir}`, or `${no_cache}`
argument appears anywhere: those placeholders are rejected for the Control
capability by the host manifest validator.

## Options

| Option | Values | Default | Meaning |
|---|---|---|---|
| `--input <directory>` | an existing directory | required | The folder the terminal must start in. |
| `--shell <name>` | `auto`, `wt`, `pwsh`, `powershell`, `cmd` | `auto` | Which shell to open. |
| `--admin` | flag | absent | Ask for an elevated terminal through the Windows consent prompt. |

There is no `admin` field in `plugin.json` on purpose. `--admin` is an argument
that a user adds in Seer's Arguments editor, exactly like any other option.

To change the default behaviour of the control, edit the argument list of the
installed control in Seer:

```
["--input", "${input_file}", "--shell", "pwsh"]
["--input", "${input_file}", "--admin"]
```

## Shell selection

`auto` prefers, in order:

1. Windows Terminal (`wt.exe`) from the documented AppX execution alias at
   `%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe`, an App Paths registration, or
   `PATH`.
2. PowerShell 7 (`%ProgramFiles%\PowerShell\7\pwsh.exe`, App Paths, `PATH`).
3. Windows PowerShell (`%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`).
4. Command Prompt (`%SystemRoot%\System32\cmd.exe`).

An explicitly requested shell is never substituted. If `--shell wt` is given and
Windows Terminal is not installed, the action fails with a message naming the
requested shell, because silently opening a different shell would contradict the
request. The search never looks inside the selected folder, so a `wt.exe` or
`cmd.exe` dropped into the folder cannot hijack the launch.

## How the starting directory is applied

Each shell needs a different mechanism, and each mechanism has its own parsing
boundary. CRT argument quoting is only correct for the first one.

| Shell | Mechanism | Parsing boundary that sees the path |
|---|---|---|
| Windows Terminal | `wt -w new new-tab -d <directory>`; the process working directory is ignored by `wt.exe`, which is a redirector | Windows Terminal's own command-line parser |
| PowerShell 7 / Windows PowerShell | no path argument at all: the directory is the process working directory | none |
| Command Prompt | no path argument at all: the directory is the process working directory | none |

The path never becomes part of a command string. It is never interpolated into
`cmd /c`, `powershell -Command`, or a generated script, so no shell parser ever
evaluates it.

### Windows Terminal and the semicolon

Windows Terminal uses `;` to separate commands and re-parses its arguments
itself, so a directory containing `;` is not fixed by CRT quoting. `wt.exe`
compares whole argument tokens, so `wt -w new new-tab -d "C:\a;b"` is parsed as
two commands and the starting directory is lost.

Measured on Windows Terminal 1.24.11911.0 with the shared probe child, using
`wt -w new new-tab -d <directory> <probe>` and reading back the child's actual
working directory:

| Directory | `-d` value | Result |
|---|---|---|
| `...\plain dir` | unchanged | correct directory |
| `...\amp&dir` | unchanged | correct directory |
| `...\pct%dir` | unchanged | correct directory |
| `...\bang!dir` | unchanged | correct directory |
| `...\caret^dir` | unchanged | correct directory |
| `...\paren(1)dir` | unchanged | correct directory |
| `...\brack[1]dir` | unchanged | correct directory |
| `...\sp;ace dir` | unchanged | **no window started in the folder** |
| `...\sp;ace dir` | `...\sp\;ace dir` | correct directory |
| `...\semi;only` | `...\semi\;only` | correct directory |
| `...\;b` | `...\\;b` | correct directory |
| `...\deep;;two` | `...\deep\;\;two` | correct directory |

So the helper escapes every `;` in the `-d` value as `\;`, which is also correct
when the `;` directly follows a path separator. `terminalhere_live_test`
re-runs the semicolon, punctuation and plain cases on the local machine instead
of asserting them from this table.

`wt.exe` exits with code 0 in every one of those cases, including the failures.
Process exit is therefore not evidence of readiness, and the launcher treats a
non-zero exit as the only failure signal the documented interface provides.

## Elevation

`--admin` uses the documented `runas` verb through `ShellExecuteExW`, elevating
the known launcher or shell with a validated argument array. UAC is never
bypassed, no policy is changed, and no scheduled task is created.

- Cancelling the consent prompt returns a non-zero exit code, so Seer does not
  apply its close-after-success behaviour. It is cancellation, not a successful
  launch and not a silent non-admin fallback.
- The elevation hop is bounded to 90 s, because the prompt belongs to the user.
- `ShellExecuteExW` returns only after the elevated process exists. That is the
  strongest signal available across an elevation boundary; the elevated starting
  directory and the elevated state are listed as manual checks in the acceptance
  section.
- An elevated Command Prompt still cannot start in a UNC directory, so that
  combination is rejected before elevation is attempted.

## Success, failure and cancellation

Exit code `0` means the requested terminal is up:

- Windows Terminal: `wt.exe` accepted the command and confirmed the handoff by
  exiting with code `0` within 8 s. Its own error text is captured and shown when
  it exits non-zero.
- PowerShell and Command Prompt: the console window belongs to the new process
  and the system creates it before `CreateProcessW` returns, so the launcher
  returns success once the shell is confirmed to still be running after 400 ms. A
  shell that exits inside that window is reported as a failure with its exit
  code, because it never gave the user a prompt.

Failures report through standard error: every failure writes its reason there,
and Seer shows it in a toast over the preview window. The launcher returns
non-zero:

| Exit code | Meaning |
|---|---|
| `0` | success |
| `1` | the shell could not be started, or Windows Terminal rejected the request |
| `2` | invalid arguments |
| `3` | the requested shell is not installed |
| `4` | the consent prompt was cancelled |
| `5` | the shell cannot start in the selected directory (Command Prompt with a UNC path) |
| `6` | the selection is not an existing directory |
| `7` | Windows Terminal did not confirm the handoff in time |

Cancellation: Seer cancels a Control by terminating the helper's process tree.
The launcher runs for at most about 8 s (Windows Terminal) or 400 ms (console
shells) and owns no child after that, so nothing is left modifying anything. The
terminal window is started outside the helper's containment job and keeps running
after the helper exits, which is the intended lifetime contract for a UI child.

## Known limitations

- Success is a **mechanism-level** signal, not a proof that a window is on screen.
  For Windows Terminal the helper waits for `wt.exe` to exit 0; for a console
  shell it requires the shell to still be running after 400 ms. Neither proves a
  visible terminal in the requested directory, and `wt.exe` exits 0 even when it
  rejects the starting directory. The live test covers the directory outcome, and
  the visible-window check remains a manual acceptance item.
- If Seer owns a job that forbids breaking away, a terminal that is launched
  anyway may inherit that job and therefore close when Seer closes. The launch
  still succeeds and the downgrade is reported nowhere: a successful control
  has no output channel the host would render, so the user is not told that
  this happened. This is the one case where the terminal window may not
  outlive Seer.

## Dependencies

No third-party dependency. The helper resolves the installed shells at run time
and never downloads or installs anything. The VC++ runtime is linked statically,
so the package contains only `plugin.json` and `terminal_here.exe`; `dumpbin
-dependents` on the shipped helper lists only `ole32`, `SHELL32`, `USER32`,
`ADVAPI32`, `GDI32` and `KERNEL32`.

## Build and test

```powershell
cd plugins/terminal-here
cmake --preset default            # Ninja, Release; binaryDir outside the working tree (machine-local CMakeUserPresets.json)
cmake --build --preset default
ctest --preset default
```

Without the machine-local preset file, configure with an explicit
out-of-repo `-B` directory instead.

Tests:

- `terminalhere_test`: Windows Terminal escaping, argument parsing, shell
  selection and the launch plan, real shell discovery, the console lifetime
  contract against a real child process, and the shared
  `plugins/common/test` assertions (CRT argv boundary, containment job, owned
  scratch, clipboard, UI readiness handoff).
- `terminalhere_live_test`: launches a real Windows Terminal window with a
  `;` directory, a punctuation directory and a plain directory, and reads back
  the working directory the child actually received. It returns 77 (ctest
  "skipped") when Windows Terminal is not installed.
- `terminalhere_manifest_test`: loads the staged package the way the host
  loader does and checks the fixed package contract.

The test suite briefly opens one console window and up to three Windows Terminal
windows; they close on their own. The clipboard assertions replace the clipboard
and restore the previous text when the previous content was text.

## Install and acceptance

```powershell
cmake --install "<your-build-dir>" --prefix dist
Compress-Archive -Path dist/* -DestinationPath terminal-here-1.0.0.zip
```

The install step validates that the manifest and helper are present under the
same package root and that the fixed contract is intact. Create the archive only
from the validated `dist` directory.

Verified in this repository:

- Real Windows Terminal 1.24.11911.0 launched in a folder containing `;`,
  `&`, `%`, `!`, `^`, `(`, `)`, `[`, `]`, spaces and Chinese characters, and the
  child's actual working directory matched the selection in every case.
- Invalid arguments, unknown shells, a missing `--input`, and a file instead of a
  folder are rejected with a visible window and a non-zero exit code.
- Command Prompt with a UNC path is rejected before anything is started.

Manual acceptance still required on a real Seer installation:

- Enable the package and confirm the control appears for a folder and produces
  exactly one control action.
- Select a folder with a trailing separator, a Chinese name, and a name
  containing `;`, and confirm the terminal opens in that folder.
- Press the control twice quickly and confirm Seer reports one invocation per
  press and no terminal is started twice for the same press.
- With `--admin`, confirm the UAC prompt appears, that cancelling it leaves Seer
  open (no close-after-success) and shows the cancellation window, and that
  accepting it opens an **elevated** terminal already in the selected folder.
- Confirm the terminal window survives Seer being closed.
- Confirm disabling the package removes the control and uninstalling it removes
  the files; reinstalling restores the control.
- On a machine without Windows Terminal, confirm `auto` falls back to PowerShell
  and that `--shell wt` reports the missing dependency instead of substituting.
- Confirm the Control close-after-success preference hides Seer only after the
  terminal is visible.
