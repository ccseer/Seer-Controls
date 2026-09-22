# Explorer Share Control

**Explorer Share Control** is an official Canonical v1 Control plugin for [Seer](https://1218.io/), the file preview tool for Windows. It opens the modern Windows system Share UI panel for the currently previewed file using the Windows Runtime (WinRT) `DataTransferManager` API.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <path>` | an existing regular file | required | The path to the file to share via Windows Share UI. Populated automatically by Seer via `${input_file}`. |
