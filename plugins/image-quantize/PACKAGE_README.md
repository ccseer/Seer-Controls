# Quantize Image

Reduce an image to a palette of up to 256 colours and write the result as an
indexed PNG next to the source. The encoder picks the smallest of 1/2/4/8 bits
per pixel and writes `PLTE` plus a per-entry `tRNS` when the palette needs it.
Nothing else changes: the source is never modified, no file is uploaded, and no
runtime has to be installed.

## Options & Arguments

The host passes only the input path. Every other option has a fixed shipping
default and exists for diagnostics and for the test harness.

| Option | Values | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `--input <path>` | path | required | Image to quantize; the only value the host supplies |
| `--colors <2..256>` | integer | 256 | Total palette entries, including the transparent entry |
| `--dither <none\|fs>` | `none`, `fs` | `fs` | Floyd-Steinberg error diffusion on or off |
| `--dither-strength <0..1>` | number | 0.5 | Error diffusion scale |
| `--space <srgb\|oklab>` | `srgb`, `oklab` | `oklab` | Refinement and nearest-search space |
| `--pipeline <auto\|srgb\|a\|b>` | keyword | `auto` | Explicit pipeline override for comparisons |
| `--kmeans-iter <0..1000>` | integer | 20 | Refinement budget; 0 disables refinement |
| `--threads <0..256>` | integer | 0 | 0 = auto; never changes the result |
| `--deflate <fast\|balanced\|max>` | keyword | `max` | Lossless compression effort; alias `--optimize 0\|1\|2` |
| `--seerviewer <path>` | path | off | Open SeerViewer in its waiting state on the output path before quantizing; the finished rename completes the wait |
| `--wait-timeout=<5000..600000>` | integer | 300000 | How long the waiting viewer polls before giving up |
| `--output <path>` | path | derived | Explicit destination |
| `--overwrite` | flag | off | Permit replacing `--output` |
| `--log <path>` | path | off | Append one JSON diagnostic line |

Default destination is `<source-dir>\<stem>-<colors>c.png`, for example
`photo-256c.png`. When that file already exists the helper advances to
`photo-256c-2.png`, `-3`, and so on, up to 1000 attempts.

## Behaviour

- Animated and multi-page sources produce one PNG from the first frame; for
  ICO the largest frame is used.
- EXIF orientation is normalised before quantization, so the output matches
  what the viewer showed.
- An embedded colour profile on an 8-bit image is converted to sRGB; a
  profile that cannot be converted, or any 16-bit image with an embedded
  profile, is refused with an explicit error instead of guessing sRGB or
  truncating.
- 16-bit input is reduced to 8 bits with the 257 round, not by truncation.
- The output is written atomically: a temporary file in the target directory,
  then a rename on the same volume. A partially written PNG is never visible.
- Alpha is preserved per palette entry through `tRNS`; an opaque source stays
  opaque, with no `tRNS` chunk and no translucent entry.
- `--deflate` trades time for size, never pixels: all three tiers decode to the
  same image, `balanced` also reorders the palette by luminance, and `max` adds
  an optimal-parse Deflate pass. The default is `max`: the control exists to
  shrink the file, so the button spends the extra seconds.
- With `--seerviewer` the helper opens the viewer in its waiting state on the
  final output path immediately, then quantizes; the atomic rename completes
  the wait and the image appears. A viewer that cannot launch only costs the
  loading state: the output is still written headless. Without `--seerviewer`
  the run stays silent and headless.

## Exit codes

| Code | Meaning |
| ---: | :--- |
| 0 | Output written; nothing is printed |
| 1 | Engine, encode or write failure |
| 2 | Missing or invalid argument |
| 3 | Decoder or encoder unavailable |
| 5 | Input format not decodable |
| 6 | Input missing or not a regular file |
| 8 | Destination locked, or auto-naming exhausted |

Every failure writes exactly one line to stderr, prefixed
`image-quantize: `. stdout stays empty.
