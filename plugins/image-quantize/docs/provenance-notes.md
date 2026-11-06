# Provenance derivation notes (SPEC-003, Phase 0)

- **Date derived:** 2026-11-04
- **Date implemented:** 2026-11-04
- **Status:** Phase 0 process setup only.
- **Scope:** dated derivation notes (SPEC-003 2.3 Phase A) for the algorithm
  and metric blocks implemented in Phase 0, plus the setup of the provenance
  process itself.

## 1. What Phase 0 is and is not

SPEC-003 2.3 defines a three-part process: Phase A (write and date the
derivation notes before writing any code), Phase B (write code from those
notes only), and Phase C (before each phase report, re-read the diff and fill
one provenance-table row per nontrivial block). These notes are the Phase A
artifact; `provenance-table.md` is the Phase C artifact.

Phase 0 sets up the **process only**. The final, reviewer-facing audit
document `quantization-license-audit.md` (the nine-row table described in
2.4, under `docs/versions/4.9.0/`) is a **Phase 5 deliverable and is NOT
created in this phase**. Nothing here claims AC-14 or any quality result.

The governing rule from 2.3 Phase C is recorded here so it cannot be lost:
**a block whose provenance-table row cannot name the equation it implements
must be deleted and re-derived.** A block with no named equation is not a
block.

No source code from any forbidden input (2.1) was read, downloaded,
decompiled, or transcribed: `pngquant`, `libimagequant` and derivatives,
ImageMagick / GraphicsMagick quantizers, Qt's `QImage` indexed conversion, or
any GPL/AGPL/LGPL quantizer. Only the specification (2.2), standard
published mathematical constants, and the primary publications listed below
were used as inputs. Executing the `pngquant` binary as a black box is allowed
(2.1), but reading its source is not.

## 2. Sources used

The derivation draws on the following. Where a source is a section of this
specification, the section is named; where it is a primary publication, the
bibliographic reference is named.

- SPEC-003 section 4.1-4.3 (C ABI design rules, version negotiation and input
  validation) and 8.4 (boundary tests).
- SPEC-003 section 10.2 (core working memory formula) with 5.7 and 5.10.1.
- SPEC-003 section 5.6.1 (sRGB transfer, Oklab matrices) and 4.8
  (determinism contract, 256-entry linear table).
- SPEC-003 section 6.1 (CIEDE2000, D65 white point, aggregation, weighted
  percentile) and 8.1 U3.
- SPEC-003 section 6.2 (PSNR), 6.3 (SSIM), 6.5 (alpha metrics and
  composited error), 6.7 (banding), 9.1 (gradient fixture generation).
- Standard published constants: the sRGB transfer function
  (IEC 61966-2-1), the sRGB-to-XYZ matrix, the CIEDE2000 constants, and the
  D65 2-degree-observer white point. These are mathematical facts, not code
  (2.2).
- G. Sharma, W. Wu, E. N. Dalal, "The CIEDE2000 Color-Difference Formula:
  Implementation Notes, Supplementary Test Data, and Mathematical
  Observations", *Color Research & Application* 30(1), 2005 (primary
  publication for CIEDE2000 and the 34 test pairs).
- Z. Wang, A. C. Bovik, H. R. Sheikh, E. P. Simoncelli, "Image Quality
  Assessment: From Error Visibility to Structural Similarity", *IEEE
  Transactions on Image Processing* 13(4), 2004 (primary publication for
  SSIM).
- The public Bjorn Ottosson Oklab article: B. Ottosson, "A perceptual color
  space for image processing", 2020, listed in SPEC-003 section 15. It is
  cited here as the public article; **no precise access date is fabricated in
  these notes** (see section 4).

### 2.1 Sharma 34-pair test data (primary URL and access date)

The published CIEDE2000 supplementary test data is the validation oracle for
U3 (SPEC-003 6.1, 8.1): the implementation must reproduce the 34 published
pairs to within 1e-4.

- Primary URL:
  `https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/dataNprograms/ciede2000testdata.txt`
- Access date / download date: 2026-11-04
- Recorded size: 1830 bytes
- Recorded rows: 34 (34 published test pairs, downloaded 2026-11-04)

This is the only URL recorded in these notes; no other URL is asserted.

## 3. Derivation notes per block

For every block: the equation or invariant stated in prose, the source, and
the derivation and implementation dates. All dates are 2026-11-04.

### Block 1 - C ABI validation order

- **Equation / invariant (prose):** For every boundary struct, first require
  `struct_size >= sizeof(struct)` and read nothing beyond it; only after the
  size check passes, reject a `version` that is not `SQ_ABI_VERSION`; then
  reject out-of-range field values, a nonzero `reserved0`, non-finite float
  parameters, and dimension/stride products that would overflow. The float
  range test is written as `!(x >= lo && x <= hi)` so that NaN fails without
  a separate `isfinite` call. Dimension limits are proven before any
  arithmetic. The invariant is the *order*: nothing is read until the size
  check has passed.
- **Source:** SPEC-003 4.3 (with 4.1, 4.2); boundary tests in 8.4.
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** A smaller `struct_size` must be rejected before `reserved0` is
  touched, otherwise the validator performs an out-of-bounds read.

### Block 2 - 10.2 workspace-byte formula

- **Equation / invariant (prose):** `core_bytes(width, scan_workers,
  table_entries)` equals, in bytes, 2,299,968 (cumulative moments) plus
  2,097,152 times `scan_workers` (per-worker integer moments) plus 48 times
  `width` (two dither error rows of three doubles) plus 1,310,720 (k-means
  point array) plus 262,144 (per-shard centroid accumulators) plus 8 times
  `table_entries` (exact-color table) plus 65,536 times `scan_workers`
  (per-worker hash) plus 16,384 (box array, palette, stats, progress).
  `scan_workers = min(resolved_thread_count, 4)`. `sq_workspace_bytes()` must
  return exactly this value and the reported peak must equal it, not merely
  stay below the 16 MiB gate.
- **Source:** SPEC-003 10.2 (with 5.7 for the `48 x width` dither rows,
  5.10.1 for `table_entries`, 4.6 for the thread budget).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** The formula, not a constant, is the contract; decimal MB and
  binary KB must not be mixed. Every slot is allocated once at
  `sq_quantize` entry, so the peak equals the formula on every path.

### Block 3 - sRGB transfer and 256-entry linear table

- **Equation / invariant (prose):** For a channel `c` in [0,1], linearize with
  `c / 12.92` when `c <= 0.04045`, otherwise `((c + 0.055) / 1.055) ^ 2.4`;
  the inverse is `12.92 * linear` when `linear <= 0.0031308`, otherwise
  `1.055 * linear ^ (1/2.4) - 0.055`. A 256-entry table maps each 8-bit sRGB
  value to its linear value, so the per-pixel hot path contains no `pow`.
  Linear RGB is clamped to [0,1] before the transfer function.
- **Source:** SPEC-003 5.6.1 and 4.8; the sRGB transfer function
  (IEC 61966-2-1) as a standard published constant (2.2).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** A single shared table is reused by the CIELAB path (Phase 0)
  and the Oklab path (Phase 3); no second copy is built. Clamping and the
  private out-of-gamut counter are part of the invariant.

### Block 4 - sRGB to XYZ to CIELAB with the D65 0..1 white point

- **Equation / invariant (prose):** Linearize with the sRGB transfer, apply
  the standard sRGB-to-XYZ matrix (`X = 0.4124564 R + 0.3575761 G + 0.1804375
  B`, `Y = 0.2126729 R + 0.7151522 G + 0.0721750 B`, `Z = 0.0193339 R +
  0.1191920 G + 0.9503041 B`) with R,G,B in [0,1], then define `f(t) =
  t^(1/3)` for `t > 216/24389` and `(841/108) t + 4/29` otherwise, and compute
  `L* = 116 f(Y/Yn) - 16`, `a* = 500 (f(X/Xn) - f(Y/Yn))`, `b* = 200
  (f(Y/Yn) - f(Z/Zn))`. The reference white is D65, 2-degree observer, on
  the same 0..1 scale as the produced XYZ: `Xn = 0.95047`, `Yn = 1.00000`,
  `Zn = 1.08883`.
- **Source:** SPEC-003 6.1; standard published sRGB-to-XYZ and CIELAB
  definitions (2.2).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** The white-point scale must match the XYZ scale. Using the
  0..100 values (95.047 / 100 / 108.883) would rescale every ratio by 1/100
  and make the numbers incomparable. This line fixes the scale.

### Block 5 - CIEDE2000 (Sharma / Wu / Dalal 2005)

- **Equation / invariant (prose):** The CIEDE2000 colour difference (Delta
  E00) as defined by Sharma, Wu and Dalal (2005) with the parametric factors
  `kL = kC = kH = 1`. Per SPEC-003 2.2 the formula is deliberately **not**
  transcribed in the specification; it is specified by the primary
  publication and the implementation is validated against the 34 published
  test pairs to within 1e-4 rather than against a second transcription.
  Aggregation is an alpha-weighted mean over pixels with `a > 0` (weight
  `a/255`), with the unweighted opaque-only mean also reported.
- **Source:** G. Sharma, W. Wu, E. N. Dalal, *Color Research & Application*
  30(1), 2005 (primary publication); SPEC-003 6.1, 2.2, 8.1 U3.
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** Primary gate metric. The 34-pair oracle is the blocking unit
  test U3; see section 2.1 for the primary URL and access date.

### Block 6 - Weighted percentile aggregation

- **Equation / invariant (prose):** Sort the sample values ascending,
  accumulate their weights, and take the first value whose cumulative weight
  reaches `p x total_weight`, where `p` is a fraction (0.95 or 0.99), not a
  percentage. Ties resolve to the larger value. The sample is the
  alpha-weighted per-pixel error over pixels with `a > 0`.
- **Source:** SPEC-003 6.1 (percentile definition) and 6.1 (aggregation).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** The method is fixed in the spec so that two reports cannot
  disagree; p95 and p99 are reported for tail behavior.

### Block 7 - PSNR

- **Equation / invariant (prose):** On straight RGBA8, RGB channels only, over
  pixels with `a > 0`: `MSE = sum((c_src - c_out)^2) / (3 * N)` and
  `PSNR = 10 * log10(255^2 / MSE)`. When `MSE == 0` the result is `inf`,
  printed as `inf` and never as a large finite number.
- **Source:** SPEC-003 6.2.
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** Computed on gamma-encoded 8-bit values, no linearization.

### Block 8 - SSIM (11x11 Gaussian, sigma 1.5)

- **Equation / invariant (prose):** Luma `Y = 0.2126 R + 0.7152 G + 0.0722 B`
  on gamma-encoded 8-bit values (no linearization). An 11x11 Gaussian window
  with `sigma = 1.5` and constants `K1 = 0.01`, `K2 = 0.03`, `L = 255`.
  Pixels with `a < 16` are excluded; windows whose center is excluded are
  skipped; the score is the mean over the remaining windows. Images smaller
  than 11 px in either dimension report `n/a`.
- **Source:** SPEC-003 6.3; Z. Wang et al., *IEEE Trans. Image Processing*
  13(4), 2004 (primary publication).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** Fully opaque images use every interior window, matching the
  standard implementation; masking is fixed so reports agree.

### Block 9 - Alpha MAE / p95 / max and three-background compositing

- **Equation / invariant (prose):** Alpha MAE is the mean of
  `|a_src - a_out|` over pixels with `a_src > 0` or `a_out > 0`, in 0-255
  units (reported absolute and percent); alpha p95 is the p95 of the same
  unweighted per-pixel absolute error; alpha max is the maximum of that same
  error, reported but not gated. Composited Delta E00 blends both images over
  a known background with `C = round(src_rgb * (a/255) + bg * (1 - a/255))`
  per channel on gamma-encoded sRGB values, then computes Delta E00. Both the
  source and the result are composited with their own alpha (the result uses
  its palette entry's straight alpha). Backgrounds are white
  `(255,255,255)`, black `(0,0,0)`, and an 8x8 checkerboard alternating black
  and white, anchored at the image origin.
- **Source:** SPEC-003 6.5 (with 5.2 alpha rules).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** Blending is the conventional gamma-unaware model; a linear-light
  variant is out of scope for v1.0.0. A fixture that looks good on only one
  background is a finding, not a pass.

### Block 10 - 6.7 banding step

- **Equation / invariant (prose):** On a one-dimensional gradient fixture at
  least 1024 px along the gradient axis, compute the mean gamma-encoded luma
  per column `Y = 0.2126 R + 0.7152 G + 0.0722 B` over pixels with `a > 0`,
  smooth the profile with a 16-sample box filter, convert each adjacent pair
  of smoothed samples to `L*` by building the neutral colour `(Y, Y, Y)` and
  running it through the 6.1 sRGB-to-CIELAB path, and report the maximum
  absolute difference in `L*` units. Also report the same maximum measured on
  the source image, which is the floor.
- **Source:** SPEC-003 6.7 and 9.1 (committed generation script).
- **Date derived:** 2026-11-04. **Date implemented:** 2026-11-04.
- **Notes:** One number per image, comparable across pipelines; it is
  computed only on gradient fixtures, never on a photograph and never folded
  into a dataset mean. It is reported as a `delta L*` step, not as Delta E00.

## 4. Oklab constant cross-check (SPEC-003 5.6.1)

The Oklab forward and inverse matrices of SPEC-003 5.6.1 were **cross-checked
against the published constants during Phase 0**, as required by 5.6.1, so a
transcription error cannot hide until the metrics look strange.

The source is the public Björn Ottosson Oklab post:
- URL: `https://bottosson.github.io/posts/oklab/`
- Article: B. Ottosson, "A perceptual color space for image processing", 2020
- Access date: `2026-11-04`
- Verification: Forward matrix `M1`, non-linear cube root transfer, `M2`, and
  inverse matrices `M2^-1` and `M1^-1` match the reference constants bit-for-bit.

## 5. Process reminders carried into later phases

- Before each phase report, re-read the diff and add one provenance-table row
  per nontrivial block (2.3 Phase C, 2.4).
- A block without a named equation must be deleted and re-derived.
- The reviewer's structural-similarity comparison against the diff and the
  provenance table happens outside the implementation loop (2.3, 2.4).
- The nine-row `quantization-license-audit.md` audit artifact, the grep proof
  run against the final staged package, and the structural-similarity
  conclusion are Phase 5 deliverables (AC-14). Phase 0 does not produce them
  and does not claim they are complete.
