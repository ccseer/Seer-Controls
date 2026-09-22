# Explorer Properties Control

**Explorer Properties Control** is an official Canonical v1 Control plugin for [Seer](https://1218.io/), the file preview tool for Windows. It opens the native Windows system Properties dialog for the currently previewed file.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <path>` | an existing regular file | required | The path to the file whose Properties dialog should be opened. Populated automatically by Seer via `${input_file}`. |
