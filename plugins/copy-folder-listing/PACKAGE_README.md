# Copy Folder Listing Control

**Copy Folder Listing Control** is an official Canonical v1 Control plugin for [Seer](https://1218.io/), the file preview tool for Windows. It copies a flat listing of the immediate contents of the currently previewed folder to the clipboard.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <directory>` | an existing folder | required | The folder to list. Populated automatically by Seer via `${input_file}`. |
| `--include-hidden` | flag | absent | Include entries with the hidden attribute in the listing. |
| `--exclude <pattern>` | `*` and `?` wildcards | absent | Skip matching entry names. May be specified multiple times. |
| `--max-entries <n>` | integer > 0 | `5000` | Maximum number of entries to render before truncating. |
| `--max-bytes <n>` | integer > 0 | `1048576` | Maximum rendered text size (in UTF-8 bytes) before truncating. |
