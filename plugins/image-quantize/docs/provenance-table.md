# Provenance table (SPEC-003 2.4, Phase C artifact)

- **Reporting date:** 2026-11-04
- **Status:** Phase 0 process setup only. This table is the reviewer artifact
  described in 2.3 Phase C and 2.4; it is **not** the final
  `quantization-license-audit.md`, which is a Phase 5 deliverable.
- **Rule (2.3 Phase C):** one row per nontrivial block, naming the equation it
  implements, its source, the date derived and the date implemented. **A block
  whose row cannot name an equation must be deleted and re-derived.**

All derivation notes for the rows below are in `provenance-notes.md` (dated
2026-11-04). Dates are recorded as YYYY-MM-DD.

| Block | Equation / invariant | Source | Date derived | Date implemented | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| C ABI validation order | size check (`struct_size >= sizeof`) before any field read; then version; then range, reserved0, non-finite and overflow checks; NaN range test `!(x >= lo && x <= hi)` | SPEC-003 4.3 (4.1, 4.2, 8.4) | 2026-11-04 | 2026-11-04 | Order is the invariant; smaller size rejected before reading reserved0 |
| 10.2 workspace-byte formula | `core_bytes = 2299968 + 2097152*scan_workers + 48*width + 1310720 + 262144 + 8*table_entries + 65536*scan_workers + 16384`; `sq_workspace_bytes()` equals it exactly | SPEC-003 10.2 (5.7, 5.10.1, 4.6) | 2026-11-04 | 2026-11-04 | Formula not constant; peak equals formula; no MB/KB mixing |
| sRGB transfer + 256-entry linear table | `linear = c/12.92` if `c <= 0.04045` else `((c+0.055)/1.055)^2.4`; inverse with `12.92*L` or `1.055*L^(1/2.4)-0.055`; 256-entry table so hot path has no `pow`; clamp linear RGB to [0,1] | SPEC-003 5.6.1, 4.8; IEC 61966-2-1 (standard constant) | 2026-11-04 | 2026-11-04 | Single shared table reused by CIELAB and Oklab; no second copy |
| sRGB -> XYZ -> CIELAB, D65 0..1 white point | sRGB-to-XYZ matrix; `f(t)=t^(1/3)` if `t>216/24389` else `(841/108)t+4/29`; `L*=116f(Y/Yn)-16`, `a*=500(f(X/Xn)-f(Y/Yn))`, `b*=200(f(Y/Yn)-f(Z/Zn))`; `Xn=0.95047, Yn=1.00000, Zn=1.08883` | SPEC-003 6.1; standard sRGB-to-XYZ and CIELAB constants | 2026-11-04 | 2026-11-04 | 0..1 white point matches XYZ scale; 0..100 values would rescale every ratio by 1/100 |
| CIEDE2000 (Sharma / Wu / Dalal 2005) | Delta E00 per Sharma, Wu, Dalal (2005) with `kL=kC=kH=1`; formula not transcribed, validated against 34 published pairs to 1e-4; alpha-weighted mean (weight `a/255`) plus unweighted opaque-only mean | Sharma et al., Color Res. Appl. 30(1), 2005; SPEC-003 6.1, 2.2, 8.1 U3 | 2026-11-04 | 2026-11-04 | Primary gate metric; U3 is the blocking unit test; test-data URL and access date in provenance-notes.md section 2.1 |
| Weighted percentile aggregation | sort ascending, accumulate weights, take first value whose cumulative weight reaches `p x total_weight` (`p` = 0.95 or 0.99 fraction); ties resolve to larger value | SPEC-003 6.1 | 2026-11-04 | 2026-11-04 | Fixed so two reports cannot disagree; p95 and p99 reported |
| PSNR | `MSE = sum((c_src-c_out)^2)/(3N)`; `PSNR = 10*log10(255^2/MSE)`; `MSE==0` reports `inf` | SPEC-003 6.2 | 2026-11-04 | 2026-11-04 | RGB channels only, pixels with `a>0`, gamma-encoded 8-bit |
| SSIM 11x11 Gaussian sigma 1.5 | `Y = 0.2126R+0.7152G+0.0722B`; 11x11 Gaussian window `sigma=1.5`, `K1=0.01`, `K2=0.03`, `L=255`; mask `a<16`, skip windows with excluded center, mean over rest; images <11px report `n/a` | SPEC-003 6.3; Wang et al., IEEE TIP 13(4), 2004 | 2026-11-04 | 2026-11-04 | Fully opaque images use every interior window |
| Alpha MAE / p95 / max and three-background compositing | MAE = mean of `abs(a_src-a_out)` where `a_src>0` or `a_out>0`; p95 and max of same error; composited `C = round(src_rgb*(a/255) + bg*(1-a/255))` then Delta E00; backgrounds white, black, 8x8 checkerboard at origin | SPEC-003 6.5 (5.2) | 2026-11-04 | 2026-11-04 | Gamma-unaware blend; both images composited with own alpha; max reported not gated |
| 6.7 banding step | mean luma `Y=0.2126R+0.7152G+0.0722B` per column on >=1024px gradient; 16-sample box filter; adjacent smoothed samples to `L*` via neutral `(Y,Y,Y)` through 6.1; report max absolute `delta L*`; also report source floor | SPEC-003 6.7, 9.1 | 2026-11-04 | 2026-11-04 | Gradient fixtures only; one number per image; reported as `delta L*`, not Delta E00 |

## Phase 1-5 blocks

> This section was intentionally empty in Phase 0. Before each later phase
> report, the Phase C step of 2.3 adds one row per nontrivial block
> implemented in that phase, naming its equation, source, date derived and
> date implemented. Do not add rows here speculatively, and do not back-fill
> earlier phases. The final `quantization-license-audit.md` (2.4 nine-row
> audit table, grep proof and reviewer structural-similarity conclusion) is
> a Phase 5 deliverable and is not part of this table.
>
> Derivation notes for the rows below:
> `C:/Users/corey/Dev/build_output/Seer/WIP/quantize/phase2/derivation-notes.md`
> (dated 2026-11-05).

| Block | Equation / invariant | Source | Date derived | Date implemented | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Wu box splitter and 3-axis variance reduction | `sse = (m2_r - m1_r^2/w) + (m2_g - m1_g^2/w) + (m2_b - m1_b^2/w)`; 3D cumulative table inclusion-exclusion; candidate cuts across all 3 axes pick maximal variance reduction `gain = sse(parent) - sse(low) - sse(high)`; box count <= max_colors; progress cancellation checked every 256 candidate cuts | SPEC-003 5.4, 4.6, Appendix A/B; X. Wu, "Efficient Statistical Computations for Optimal Color Quantization", 1991 | 2026-11-04 | 2026-11-05 | Evaluates variance reduction across all 3 axes rather than only the largest-variance axis; transparent slot reserved if present |
| Weighted Lloyd refinement | `J = sum_i w_i d^2(p_i, c_{a(i)})` with `d^2 = (dr^2 + dg^2) + db^2` in double; alternate nearest-centre assignment with weighted-mean centre update; strict `<` keeps the lowest cluster index | SPEC-003 5.5, 4.8 item 4, 5.6.2; Lloyd, IEEE IT 28(2), 1982 | 2026-11-05 | 2026-11-05 | Objective trace is exposed to the unit suite; query points are float cell means, not 8-bit-quantised coordinates; rounding happens once at emission |
| Fixed logical shard reduction | `S[s][c] += p_i w_i` for `s = i/1024` and `i` ascending inside the shard; merge `s` ascending then `c` ascending; `J = sum_s J_s` in shard order; `centre[c] = T[c]/T_w[c]` | SPEC-003 4.8 item 3, 5.5, 10.2 | 2026-11-05 | 2026-11-05 | Thread count only selects which shards run concurrently; a byte-wise fingerprint over the merged sums is compared across 1/2/4/8 threads (U23) |
| Refinement termination | stop on `iterations == kmeans_max_iter`, `(J_prev - J_cur)/max(J_prev, 1e-12) < kmeans_tolerance`, or no assignment change; empty cluster retains its previous centre | SPEC-003 5.5, 8.1 U9, 11.6 | 2026-11-05 | 2026-11-05 | `kmeans_max_iter == 0` keeps the initial palette; the objective is non-increasing up to floating rounding |
| Integer palette alpha | `W_c = sum_{a(i)=c} w_i` and `N_c = sum_{a(i)=c} n_i` in uint64, `a_c = floor(W_c/N_c + 0.5)` clamped to 8-bit | SPEC-003 5.5, 5.2, Appendix A | 2026-11-05 | 2026-11-05 | Integer addition is associative, so this pass needs no shard rule; `w_i` is an exact integer below 2^53 stored as double |
| RGBA-exact deduplication | entries with equal packed RGBA merge into the lowest index; assignments are remapped through the same table | SPEC-003 5.5, 8.1 U19 | 2026-11-05 | 2026-11-05 | Entries sharing RGB but differing in alpha are never merged |
| Farthest-first seeding | first seed maximises weight, then repeatedly the point maximising its minimum squared distance to the chosen seeds; ties resolve to the smallest packed RGBA key | SPEC-003 5.10, 5.3, 4.8 item 3 | 2026-11-05 | 2026-11-05 | The exact colour table is sorted by packed key, so ascending point index is the same tie-break; the transient minimum array reuses the pre-allocated shard block |
| Refinement progress and cancellation | `percent = floor(43 + 22*(iteration*n + completed)/(kmeans_max_iter*n))`; workers read the cancel flag every 1024 assignments; the calling thread publishes progress, sets the flag and joins | SPEC-003 4.5, 4.6, 4.6.1, 10.4 | 2026-11-05 | 2026-11-05 | No worker allocates and no worker invokes the callback |
| V3D / V4D recorded experiment | 4D distance `d^2_3 + (alpha'*w_a - alpha'_centre)^2` over an exact RGBA table; alpha MAE, p95, max and composited dE00 over white, black and checkerboard | SPEC-003 5.9, 6.5, 11.6 | 2026-11-05 | 2026-11-05 | Independent data source only; the fourth axis is never merged into the engine and would change the 10.2 formula and the golden hashes |
| Oklab forward and inverse matrices | `l_=cbrt(0.4122214708r+0.5363325363g+0.0514459929b)` (likewise m, s); `(L,a,b)` from the published second matrix; inverse cubes `l_`, `m_`, `s_` and applies the inverse matrix; linear RGB clamped to [0,1] before the transfer | SPEC-003 5.6.1; B. Ottosson, "A perceptual color space for image processing", 2020 (https://bottosson.github.io/posts/oklab/, accessed 2026-11-04) | 2026-11-05 | 2026-11-05 | Reuses the single Phase 0 256-entry sRGB-to-linear table for 8-bit input; fractional cell means use the transfer formula; clamp events live in a private counter and are never exported |
| Oklab round-trip invariants | `linear -> Oklab -> linear` within 2e-6 for 10^5 random triples (U1); all 256 gray levels exact (U2); all 16.7M RGB triples within 1 LSB per channel | SPEC-003 5.6.1, 8.1 U1/U2 | 2026-11-05 | 2026-11-05 | Full enumeration runs in a separate manual tool, not in CTest; command, elapsed time and worst delta recorded in the Phase 3 report |
| Approach A pipeline | Wu split stays in the sRGB 32^3 histogram; points and centres are `(L,a,b)`; `J = sum w_i d^2` in Oklab; centroid converts to sRGB8 once at emission; index search targets the post-dedup 8-bit palette converted back to Oklab | SPEC-003 5.6.2, 5.5, 5.7, 4.8 | 2026-11-05 | 2026-11-05 | AUTO derives the working space from `color_space`; the B pipeline enum resolves to A and row 3 of 5.6.4 is absent; no new heap allocation, so AC-10 is unchanged |
| Phase 3 comparison matrix | two measured rows (sRGB, Oklab A) on one fixture set at `max_colors=256` without dithering, with mean/p95 dE00, PSNR, SSIM, runtime, core bytes, per-category and per-image paired ratios; AC-2 .. AC-6 bound on the recommended row | SPEC-003 5.6.4, 12, 11.7 | 2026-11-05 | 2026-11-05 | Baseline re-measured with the pinned binary, not copied from Phase 2; the written recommendation is open decision 1 |
| Floyd-Steinberg feedback and integer conservation | `C = original + incoming`, `C' = coerce(C)`, `e = C' - q`, `k = round(s*1024)`, `e_scaled = (e*k)/1024`; `p7=(e_scaled*7)/16`, `p3`, `p5`, `p1 = e_scaled - p7 - p3 - p5` so the four shares sum to `e_scaled` exactly | SPEC-003 5.7, 8.1 U12/U22 | 2026-11-05 | 2026-11-05 | Division replaces a right shift for negative errors; U12 asserts the exact equality over signed errors |
| Working-space error diffusion | sRGB uses int32 in the pre-allocated double slots; Oklab uses double shares `e*s*7/16`, `3/16`, `5/16`, `1/16`; both search the post-dedup 8-bit palette converted back into the working space | SPEC-003 5.7, 5.6.2, 10.2, 8.1 U24 | 2026-11-05 | 2026-11-05 | The `48 x width` error rows were reserved at entry since Phase 1; this phase only starts reading them, so AC-10 stays an equality |
| Serpentine raster and cancellation | even rows scan left to right, odd rows right to left; `a == 0` pixels emit the transparent index and discard error aimed at them; strictly sequential; progress band 67-99 with checkpoints every 1024 pixels and every 32 rows | SPEC-003 5.7, 4.6, 4.6.1, 4.8 item 5 | 2026-11-05 | 2026-11-05 | Strength 0 and `SQ_DITHER_NONE` bypass the loop, so U11 is byte-identical by construction |
| Phase 4 comparison and binding | dither on/off, sRGB-error versus Oklab-error rows, 6.7 banding, AC-25 against a same-dither-mode baseline, AC-26 against the no-dither row and the pinned baseline, P3/P5 timing | SPEC-003 5.7, 6.7, 11.8, 12 | 2026-11-05 | 2026-11-05 | The alpha-scaled variant row is absent; AC-3 is carried forward as an open BLOCKED finding and AC-26's strict "below no-dither" test fails on all three gradient fixtures |

| Lossless Deflate encoder (`max` tier) | original optimal-parse encoder: 16-bit hash chains over the 32 KB window, nearest distance cached per length-code bucket, shortest-path parse driven by an iterated Huffman cost model (6 passes per 256 KB block), dynamic Huffman with length-limited codes and a Kraft-checked fallback, stored-block guard | SPEC-003 13.5 (r14) | 2026-11-05 | 2026-11-05 | Written from RFC 1951 only; no zlib/Zopfli/libdeflate source read, ported or linked. `deflate_test` round-trips every stream through lodepng's inflate and asserts byte determinism |
| Palette luminance reordering (`balanced`, `max`) | stable sort of palette entries by `2126R + 7152G + 722B` with an RGBA tie-break, index plane remapped through the permutation | SPEC-003 13.5 (r14) | 2026-11-05 | 2026-11-05 | Deterministic; every tier decodes to identical RGBA on the four measured fixtures |

## Rule reminder

A block without a named equation must be deleted and re-derived (2.3 Phase
C). The reviewer's structural-similarity comparison against the diff and this
table is performed outside the implementation loop (2.3, 2.4).
