# Hex View

Generates a classic 16-byte hex dump of the current file — offset column, hexadecimal byte columns and a decoded ASCII column — and shows it instantly in a separate SeerViewer window, leaving your main preview untouched. Works on any file regardless of extension, so you can inspect the raw bytes of images, media, archives or executables even when a dedicated viewer is showing them.

## Syntax highlighting

The package ships `syntax/hextxt.xml`, a `HexDump` definition that makes Seer's text viewer claim and color the `*.hextxt` dumps this control writes (offset, hex bytes, ASCII column). The definition follows Seer's syntax contribution contract: `version="2"`, `priority="100"`, `kateversion="5.0"`. Do not edit these attributes — `priority` decides who wins a shared extension, and other definitions may already rely on the published value; `kateversion` must stay at or below the syntax library Seer bundles, or the host refuses to install the package.

## Options & Arguments

| Parameter / Flag | Accepted Values | Default | Purpose |
| :--- | :--- | :--- | :--- |
| `<input_file>` | File path | (Required pos 1) | The file to dump, expanded by the host from `${input_file}`. |
| `<seerviewer>` | Path to executable | (Required pos 2) | Absolute path to `SeerViewer.exe`, expanded from `${seerviewer_exe}`, used to present the pending/result window. |
| `--max-kb` | 1–1048576 (KB) | `64` | Hard cap on how many bytes are dumped (1 GiB maximum). Larger values are clamped into the range; the dump costs about 5.3 bytes of text per source byte, so raise it only when you have the disk space. |
| `--case` | `upper`, `lower` | `upper` | Case of the hexadecimal digits in both the offset and byte columns. |
| `--uppercase` | Flag | Off | Shorthand for `--case=upper`; cannot be combined with `--case` or `--lowercase`. |
| `--lowercase` | Flag | Off | Shorthand for `--case=lower`; cannot be combined with `--case` or `--uppercase`. |
| `--wait-timeout` | 5000–600000 (ms) | `30000` | Timeout passed to `SeerViewer.exe --wait-file` before the window shows its own timeout notice. |

To change the default behaviour of the control, edit the argument list of the
installed control in Seer's Settings, e.g. raise the dump cap:

```json
["${input_file}", "${seerviewer_exe}", "--max-kb=256"]
```
