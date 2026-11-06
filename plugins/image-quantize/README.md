# Quantize Image

Canonical v1 Control package that reduces the current image to at most 256
colours and writes an indexed PNG next to the source. The host passes only
`--input "${input_file}"`; every other switch exists for diagnostics and for
the harness.

This file is the developer record. The shipped, user-facing readme is
`PACKAGE_README.md`, staged as `README.md` inside the ZIP.

## Layout

| Path | Purpose |
| :--- | :--- |
| `src/engine/` | `quantize_core`: Wu splitter, weighted k-means, Oklab, nearest search, Floyd-Steinberg. C++17, no Qt, no third-party code, plain C ABI (`include/seer_quantize.h`). |
| `src/helper/` | `image_quantize.exe`: `wWinMain`, CLI, WIC decode, lodepng encode, output policy. |
| `test/` | Engine suites, `runner_test` (helper contract), manifest test. |
| `tools/` | Bench, metrics, reference implementations, WIC PoC. Test/tool only. |
| `scripts/` | Dataset generation, quality/determinism/golden harnesses, provenance check, `ci.ps1`. |
| `docs/` | Dataset manifest, golden hashes, provenance tables. |

## Build

Presets `default` (Release) and `asan`. MSVC 19.51, `/MT`, `/W4 /WX`. The build
needs only the Windows SDK: WIC, ole32 and shell32 are system components.

```
cmake --preset default
cmake --build --preset default
ctest --preset default
```

`imagequantize_package` stages `plugin.json`, `image_quantize.exe` and
`README.md`, runs the manifest test, then packs `dist/image-quantize-1.0.0.zip`
through a POST_BUILD `7z` step (spec 3.3/13.8; a `package.ps1` is deliberately
not used).

## Behaviour contract

- Exit 0 means the PNG was written; nothing is printed. Every failure writes
  exactly one `image-quantize: ` line to stderr, UTF-8, no truncation notice.
- Cancellation does not exist in v1.0.0: codes 4 and 7 are reserved and never
  produced. The host enforces `timeout_ms` by killing the process.
- The source is opened read-only and never modified. Output goes to the source
  directory (or `--output`), through a temp file in the target directory and a
  rename, so a partial PNG is never observable and the rename stays on one
  volume.
- Caps: 256 palette entries including the transparent entry, 65535 px per side,
  2^28 pixels.
- Viewer mode: with `--seerviewer <exe>` the helper resolves the final output
  name first, opens SeerViewer with `--wait-file --wait-timeout=<ms>` on it
  (loading UI within milliseconds of the click), quantizes, and completes the
  wait with the atomic rename. The name is resolved up front, so a rename
  collision is a failure rather than a name change; a viewer that cannot launch
  degrades to the headless run. `plugin.json` passes `${seerviewer_exe}` and
  `--wait-timeout=300000` alongside `--input`.

## Algorithm provenance

The engine is an independent implementation written from the published
equations. It contains no third-party quantizer code: no source was read,
ported or linked, and `scripts/check_provenance.py` is the grep proof.

- Initial palette: X. Wu, "Efficient Statistical Computations for Optimal Color
  Quantization", Graphics Gems II, Academic Press, 1991, pp. 126-133.
- Refinement: S. P. Lloyd, "Least Squares Quantization in PCM", IEEE
  Transactions on Information Theory 28(2), 1982 (weighted Lloyd refinement over
  the colour histogram).
- Working space: B. Ottosson, "A perceptual color space for image processing",
  2020, https://bottosson.github.io/posts/oklab/
- Dithering: R. W. Floyd, L. Steinberg, "An Adaptive Algorithm for Spatial Grey
  Scale", Proceedings of the Society for Information Display 17(2), 1976.
- Transfer function: IEC 61966-2-1 (sRGB).
- Deflate (the `max` tier only): RFC 1951, an original encoder written for this
  package; no zlib, Zopfli or other third-party compressor is linked.
- Quality measurement in the test harness only: G. Sharma, W. Wu, E. N. Dalal,
  "The CIEDE2000 Color-Difference Formula: Implementation Notes, Supplementary
  Test Data, and Mathematical Observations", Color Research & Application 30(1),
  2005, https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/

CIEDE2000 is never used by the product path; it scores fixtures in
`quantize_metrics`.

## Dependency policy

The shipped helper links `quantize_core` plus WIC (decode), the vendored
`lodepng` (PNG-8 encode; open decision 2 approved), ole32 and shell32. No VC++
redistributable, no third-party DLL. `stb_image` lives under `tools/thirdparty`
and is compiled only into test/tool targets with `/W0`. `pngquant` 2.17.0 is
executed as a black-box baseline and never linked or shipped.

## Tests

- `quantize_test`: engine units and properties (418,107 checks).
- `deflate_test`: the `max` tier encoder round-trips through lodepng's inflate
  on synthetic buffers up to 76 KB, is byte-deterministic, and emits a valid
  stored block for empty input.
- `quantize_abi_test`: C99 ABI.
- `runner_test` (1,358 checks): helper contract — exit codes, single-line stderr, collision
  advance, 1,000-attempt exhaustion, `--overwrite`, concurrent helpers,
  thread-count determinism, `MAX_PATH`, `\\?\`, UNC and non-ASCII paths, a
  read-only source directory, indexed output for a grey palette, an opaque
  source with no translucent palette entry, `--deflate` tier validation, tier
  output equality and ordering, the tRNS truncation regression, viewer-mode
  argument validation and headless degradation, and AC-20 (PNG decoded with
  lodepng and compared pixel-for-pixel against the engine's own palette and
  indices).
- `image_quantize_manifest_test`: manifest contract and staged tree.
- `quantize_metrics`, `quantize_bench`, `quantize_wic_poc`, `quantize_wic_depth`
  self-checks.

## Manual acceptance checklist

1. Install the ZIP in a clean portable profile; the control appears for image
   files only.
2. Click it on a JPEG and on a PNG with transparency; a `*-256c.png` appears
   beside each source, the host shows no toast, and the source is unchanged.
3. Click twice on the same file; the second run produces `*-256c-2.png`.
4. Point it at an image in a read-only folder; the host shows one line naming
   the directory.
5. Open the result in Seer: orientation matches the original, transparency is
   intact, and the image renders cleanly (gradient banding remains tracked under
   open item AC-3).

## Deflate tiers

`--deflate fast|balanced|max` (alias `--optimize 0|1|2`) selects how much work
the encoder spends on the same pixels. The engine output never changes.

| Tier | What runs | Candidates |
| :--- | :--- | :--- |
| `fast` | lodepng deflate, window 32768 | filter 0 and minimum-sum, smallest kept |
| `balanced` | lodepng deflate | the two above plus entropy filtering, each on the original and on a luminance-sorted palette |
| `max` (default) | `sq_deflate` optimal parse (`src/helper/sq_deflate.cpp`), 6 cost-model passes per 256 KB block | screens the balanced set with lodepng, then re-encodes the best variant and filter 0 with the optimal parser |

All tiers decode to identical RGBA (checked across fixtures), and `max` is
byte-deterministic across runs and thread counts. Time is deliberately not a gate
(implementation plan item 9): the control is a one-shot conversion, not a
preview path. Since r15 the shipping default is `max`, so `plugin.json` keeps
passing only `--input` while the helper default does the work; `timeout_ms` is
300000 to keep a ~3x margin over the measured 101.3 s 8K run.

Across the benchmark dataset, `max` saves an additional 6% to 18% over `fast`
losslessly on the exact same palette and pixels. The full per-fixture breakdown
across `fast`, `balanced`, and `max` is consolidated into the unified benchmark
table below under [Real-world dataset benchmark evaluation](#real-world-dataset-benchmark-evaluation).

## Comparison with TinyPNG and pngquant

TinyPNG (tinypng.com) is a hosted cloud service: the image is uploaded, optimised
by an unpublished server-side pipeline, and downloaded again. This package does
the same class of work locally, in one process, with no network access, no account,
and no upload file-size restrictions. The table mixes measured numbers with facts;
a cell this repository did not measure says so instead of repeating a claim.
Measured comparisons and vendor claims stay in this developer record;
`PACKAGE_README.md` describes behaviour only.

| Dimension | This package (`image-quantize`) | TinyPNG (tinypng.com) | pngquant 2.17 (local baseline) |
| :--- | :--- | :--- | :--- |
| Where it runs | local native Win32 process, no install | hosted cloud service, upload and download | local CLI |
| Network & Privacy | 100% offline, zero upload, no file size limit | requires internet, uploads data, 5 MB free web limit | 100% offline, local CLI |
| Colour reduction | up to 256 entries: 5-bit histogram split, then weighted k-means | up to 256 entries (PNG-8, algorithm unpublished) | up to 256, its own quantizer |
| Working space | Oklab for refinement and nearest search | not published | its own internal space |
| Dither | Floyd-Steinberg, strength 0.5 | not published | Floyd-Steinberg, default strength 1.0 |
| Alpha | straight per-entry alpha, `tRNS` | straight per-entry alpha, `tRNS` | straight per-entry alpha |
| Deterministic output | yes, identical bytes at any thread count | no guarantee published | yes for the same input and options |
| Measured time (Ultra 9 285K) | 0.04 s–34 s at <= 4K, 101 s at 8K (wall time) | not measured locally (network round-trip + queue) | 0.01 s–2.7 s at <= 4K, 8.4 s at 8K |
| Output reduction vs source (real suite) | -49.2% to -75.5% (-26.2% on soft shadow) | -24.5% to -73.8% (>5 MB fails on free tier) | -27.4% to -67.9% (+12.0% on soft shadow) |
| Visual error (measured samples) | dE00 1.86 photo, 0.00 UI and alpha | not measured | dE00 1.65 photo, 0.00 UI and alpha |

### Real-world dataset benchmark evaluation

Measured on Intel(R) Core(TM) Ultra 9 285K (24 cores / 24 threads, Windows 11 x64, MSVC 19.51 Release) across the 12 real fixtures under `test/data/real/` plus the synthetic alpha fixture under `test/data/generated/`:
- `image-quantize` was tested across all three Deflate tiers (`fast`, `balanced`, and shipping default `max`), 256 colours, Oklab, Floyd-Steinberg 0.5.
- `pngquant 2.17` was executed with `pngquant --force --quality=0-100 --speed 1 256 -- <input>` (default Floyd-Steinberg 1.0).
- `tinypng.com` was evaluated by processing identical inputs via the web service. Note: `large_01_4k.png` (7.27 MB) and `large_02_8k.png` (34.58 MB) exceeded TinyPNG's 5 MB free upload limit and failed to compress.
- **Summary vs TinyPNG**: In the 10 comparable fixtures, **image-quantize wins 4 and loses 6**:
  - *Wins (4)*: `photo_03_sky_gradient` (-1.7% of orig vs TinyPNG), `ui_01_light` (-31.6% of orig), `ui_02_dark` (-36.9% of orig), and `ui_04_toolbar_1px` (-7.5% of orig).
  - *Losses (6)*: The other five photographs (`photo_01`, `photo_02`, `photo_04`, `photo_05`, `photo_06`) and `ui_03_file_icons`, where TinyPNG is smaller by 0.6% to 9.8% of the original file size.
- **Summary vs pngquant 2.17**: **image-quantize wins 13 of 13 fixtures (100% win rate)**, producing smaller outputs across every photographic, UI, large-scale (4K/8K), and alpha fixture (saving 24 B to 3.87 MB over pngquant).

| Fixture | Dimensions | Original Size | `fast` | `balanced` | `max` (default) | `pngquant 2.17` | TinyPNG (tinypng.com, 2026/11/6) | Analysis & Comparison |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| `photo_01_portrait_skin` | 1920x1280 | 3,219,759 B | 1,388,426 B<br>(1.2 s, -56.9%) | 1,356,005 B<br>(2.6 s, -57.9%) | 1,280,566 B<br>(7.0 s, -60.2%) | 1,400,097 B<br>(1.0 s, -56.5%) | **1,170,626 B**<br>(-63.6%) | TinyPNG smaller by 109,940 B (3.4% of orig); beats pngquant by 119,531 B (3.7% of orig) |
| `photo_02_foliage_texture` | 1920x1280 | 4,755,731 B | 1,954,949 B<br>(0.9 s, -58.9%) | 1,780,405 B<br>(1.5 s, -62.6%) | 1,780,405 B<br>(2.5 s, -62.6%) | 1,990,176 B<br>(1.3 s, -58.2%) | **1,751,534 B**<br>(-63.2%) | TinyPNG smaller by 28,871 B (0.6% of orig); beats pngquant by 209,771 B (4.4% of orig) |
| `photo_03_sky_gradient` | 1920x1280 | 2,048,066 B | 561,302 B<br>(1.4 s, -72.6%) | 561,224 B<br>(3.2 s, -72.6%) | **501,660 B**<br>(33.9 s, -75.5%) | 688,439 B<br>(1.1 s, -66.4%) | 536,990 B<br>(-73.8%) | **image-quantize beats TinyPNG by 35,330 B (1.7% of orig)** and pngquant by 186,779 B (9.1% of orig) |
| `photo_04_night_scene` | 1920x1280 | 3,203,871 B | 1,040,838 B<br>(2.1 s, -67.5%) | 1,040,838 B<br>(5.9 s, -67.5%) | 974,355 B<br>(27.7 s, -69.6%) | 1,136,610 B<br>(1.6 s, -64.5%) | **948,283 B**<br>(-70.4%) | TinyPNG smaller by 26,072 B (0.8% of orig); beats pngquant by 162,255 B (5.1% of orig) |
| `photo_05_fabric_fur` | 1920x1280 | 2,961,974 B | 1,159,725 B<br>(1.3 s, -60.8%) | 1,159,705 B<br>(3.0 s, -60.8%) | 1,087,909 B<br>(6.0 s, -63.3%) | 1,150,919 B<br>(0.8 s, -61.1%) | **952,092 B**<br>(-67.9%) | TinyPNG smaller by 135,817 B (4.6% of orig); beats pngquant by 63,010 B (2.1% of orig) |
| `photo_06_saturated_product` | 1920x1280 | 4,004,584 B | 1,234,696 B<br>(1.2 s, -69.2%) | 1,234,694 B<br>(2.4 s, -69.2%) | 1,132,951 B<br>(4.2 s, -71.7%) | 1,287,086 B<br>(1.3 s, -67.9%) | **1,064,372 B**<br>(-73.4%) | TinyPNG smaller by 68,579 B (1.7% of orig); beats pngquant by 154,135 B (3.8% of orig) |
| `ui_01_light` | 1280x800 | 6,460 B | 3,021 B<br>(0.1 s, -53.2%) | 2,946 B<br>(0.2 s, -54.4%) | **2,496 B**<br>(11.1 s, -61.4%) | 3,018 B<br>(0.07 s, -53.3%) | 4,536 B<br>(-29.8%) | **image-quantize beats TinyPNG by 2,040 B (31.6% of orig)** and pngquant by 522 B (8.1% of orig) |
| `ui_02_dark` | 1280x800 | 6,291 B | 2,972 B<br>(0.1 s, -52.8%) | 2,894 B<br>(0.2 s, -54.0%) | **2,429 B**<br>(8.3 s, -61.4%) | 3,012 B<br>(0.08 s, -52.1%) | 4,749 B<br>(-24.5%) | **image-quantize beats TinyPNG by 2,320 B (36.9% of orig)** and pngquant by 583 B (9.3% of orig) |
| `ui_03_file_icons` | 1024x1024 | 28,903 B | 16,371 B<br>(0.1 s, -43.4%) | 16,344 B<br>(0.3 s, -43.5%) | 14,672 B<br>(26.2 s, -49.2%) | 20,971 B<br>(0.08 s, -27.4%) | **11,828 B**<br>(-59.1%) | TinyPNG smaller by 2,844 B (9.8% of orig); beats pngquant by 6,299 B (21.8% of orig) |
| `ui_04_toolbar_1px` | 960x64 | 827 B | 432 B<br>(0.03 s, -47.8%) | 432 B<br>(0.04 s, -47.8%) | **378 B**<br>(0.4 s, -54.3%) | 402 B<br>(0.01 s, -51.4%) | 440 B<br>(-46.8%) | **image-quantize beats TinyPNG by 62 B (7.5% of orig)** and pngquant by 24 B (2.9% of orig) |
| `alpha_soft_shadow_64x64` | 64x64 | 1,704 B | 1,466 B<br>(0.03 s, -14.0%) | 1,320 B<br>(0.03 s, -22.5%) | **1,257 B**<br>(0.04 s, -26.2%) | 1,908 B<br>(0.01 s, +12.0%) | *not tested<br>(synthetic)* | **image-quantize beats pngquant by 651 B (38.2% of orig)** (pngquant bloated by +12.0%) |
| `large_01_4k` | 3840x2160 | 7,273,173 B | 2,612,038 B<br>(4.6 s, -64.1%) | 2,611,909 B<br>(10.4 s, -64.1%) | **2,322,927 B**<br>(33.5 s, -68.1%) | 2,922,105 B<br>(2.7 s, -59.8%) | *Failed<br>(>5 MB upload limit)* | **image-quantize beats pngquant by 599,178 B (8.2% of orig)**; TinyPNG fails |
| `large_02_8k` | 7680x4320 | 34,583,576 B | 14,381,336 B<br>(17.7 s, -58.4%) | 14,381,012 B<br>(37.0 s, -58.4%) | **12,950,702 B**<br>(101.3 s, -62.6%) | 16,818,345 B<br>(8.4 s, -51.4%) | *Failed<br>(>5 MB upload limit)* | **image-quantize beats pngquant by 3,867,643 B (11.2% of orig)**; TinyPNG fails |

### Measured characteristics and trade-offs

- **Measured size ratios**: 8-bit indexed quantization achieves significant file
  size reduction across photographic and UI fixtures (0.24x–0.51x on the real
  benchmark suite, saving ~50% to 75% of original bytes). On images dominated by
  smooth alpha transparency, reduction is more modest (0.74x on `alpha_soft_shadow_64x64`,
  a 26% decrease, whereas pngquant produces a 1.12x expansion).
- **Comparison across engines**:
  - **Against TinyPNG (4 wins, 6 losses on 10 comparable fixtures)**:
    - On low-color UI and vector assets (`ui_01_light`, `ui_02_dark`, `ui_04_toolbar_1px`), `image-quantize` achieves significantly smaller sizes (saving an additional 7.5% to 36.9% of original bytes over TinyPNG), as TinyPNG appears to retain 24-bit RGB or less aggressive reduction on small low-color UI sources.
    - On smooth photographic gradients (`photo_03_sky_gradient`), `image-quantize` beats TinyPNG by 35 KB (1.7% of orig).
    - On complex photographic textures (`photo_01`, `photo_02`, `photo_04`, `photo_05`, `photo_06`) and multi-icon sheets (`ui_03`), TinyPNG achieves smaller sizes (saving an additional 0.6% to 9.8% of original size), likely reflecting proprietary cloud-side heuristics such as lossy palette merging or adaptive pre-filtering.
    - On 4K / 8K assets, TinyPNG is unavailable on the free tier (>5 MB limit), whereas `image-quantize` operates locally with zero upload limits.
  - **Against pngquant (13 wins, 0 losses on all 13 fixtures)**:
    - `image-quantize` outperforms `pngquant` across every single test case, saving 0.1% to 38.2% more of the original file size (from 24 B on the 1 px toolbar up to 3.87 MB on 8K).
    - On transparent images (`alpha_soft_shadow_64x64`), `image-quantize` reduces size by 26.2%, whereas `pngquant` expands the file by +12.0%.
- **Post-quantization Deflate and filtering**: The shipping `max` tier screens
  filter and palette-order candidates with lodepng, then re-encodes the most
  promising ones with the package's own optimal-parse Deflate
  (`src/helper/sq_deflate.cpp`), which applies the lossless optimizations that
  standalone tools such as Zopflipng or Oxipng perform: luminance-ordered
  palette indexing, per-row filter selection, and multi-pass iterative Deflate.
  Across all test fixtures, `max` yields an additional 6% to 18% size reduction
  over `fast` without changing a single pixel. `--deflate fast|balanced|max`
  (alias `--optimize`) selects the effort; `fast` provides sub-second to low-second
  encoding when execution speed is prioritized.
- **Color space and visual difference**: Refinement and nearest-color search are
  computed in Oklab rather than sRGB. On the portrait skin sample fixture,
  measured dE00 is 1.86 for this package and 1.65 for pngquant (with default
  dither strength 1.0). Gradient step smoothness remains an open item tracked
  under AC-3 (0.0156 measured vs 0.0133 threshold). For sources with <= 256
  distinct colors, the engine takes an exact lossless path (dE00 = 0.00, no dither).

## Known open items

- AC-22 now passes on every measured fixture (worst 1.075x, the 1 px toolbar)
  after the encoder was taught to choose between filter 0 and minimum-sum per
  file. The earlier 1.473x / 1.234x rows were encoder configuration, not
  palette data.
- Phase 4 carries AC-3, AC-26 and the missing alpha-scaled row as recorded
  BLOCKED items. AC-3 (gradients 0.0156 against 0.0133) is a gradient-path
  quality question, not a size or encoder question.
