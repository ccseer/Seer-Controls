# Who Is Locking This File? Control

**Who Is Locking This File? Control** is an official Canonical v1 Control plugin for [Seer](https://1218.io/), the file preview tool for Windows. It inspects and displays which applications, background services, or system processes are currently holding or locking the selected file.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <file>` | an existing regular file | required | The file to inspect. Populated automatically by Seer via `${input_file}`. |
| `--admin` | flag | absent | Query with administrator rights via Windows UAC consent prompt to inspect privileged system processes. |
