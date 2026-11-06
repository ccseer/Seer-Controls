# Explorer Properties Control

Opens the native Windows system Properties dialog for the currently previewed file.

Success means the Properties window appeared. The helper watches for it and
reports a failure when there is none after three seconds, so a Shell call that
is accepted but shows nothing is never mistaken for a result.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <path>` | an existing regular file | required | The path to the file whose Properties dialog should be opened. Populated automatically by Seer via `${input_file}`. |
