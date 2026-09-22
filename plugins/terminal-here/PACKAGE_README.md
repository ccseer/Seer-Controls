# Terminal Here Control

**Terminal Here Control** is an official Canonical v1 Control plugin for [Seer](https://1218.io/), the file preview tool for Windows. It opens a command prompt or terminal window with its starting directory set to the currently previewed folder.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <directory>` | an existing directory | required | The folder the terminal must start in. Populated automatically by Seer via `${input_file}`. |
| `--shell <name>` | `auto`, `wt`, `pwsh`, `powershell`, `cmd` | `auto` | Which shell to open. `auto` prefers Windows Terminal -> PowerShell 7 -> Windows PowerShell -> Command Prompt. |
| `--admin` | flag | absent | Open an elevated terminal window via Windows UAC consent prompt. |
