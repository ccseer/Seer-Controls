# Copy Folder Listing Control

Copies a flat listing of the immediate contents of the currently previewed folder to the clipboard.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <directory>` | an existing folder | required | The folder to list. Populated automatically by Seer via `${input_file}`. |
| `--include-hidden` | flag | absent | Include entries with the hidden attribute in the listing. |
| `--exclude <pattern>` | `*` and `?` wildcards | absent | Skip matching entry names. May be specified multiple times. |
| `--max-entries <n>` | integer > 0 | `5000` | Maximum number of entries to render before truncating. |
| `--max-bytes <n>` | integer > 0 | `1048576` | Maximum rendered text size (in UTF-8 bytes) before truncating. |
| `--separator <value>` | named form, escape sequence or literal text | `crlf` | Written after every entry, including the last. Named forms: `crlf`, `lf`, `cr`, `tab`, `space`, `comma`, `semicolon`, `pipe`, `none`. `\n`, `\r`, `\t` and `\\` are expanded; the result is limited to 16 characters. |
