#!/usr/bin/env python3
"""Build the 5.6.4 comparison matrix from two run_quality.py CSV files.

Both rows use the same fixture set, max_colors=256 and no dithering. Row 3
(Oklab B) is absent in v1.0.0; the script states that explicitly. Runtime can
be supplied from a clean, serial measurement so concurrent evidence runs do
not distort the number that the phase report quotes.

Usage:
    py -3 scripts/run_phase3_compare.py --srgb <quality-none.csv>
        --oklab <quality-none.csv> --out <phase3-comparison.md>
        [--runtime-srgb-ms 394.0] [--runtime-oklab-ms 410.0]
"""

import argparse
import csv
import io
import math
import os

CATEGORIES = (
    "photographs", "gradients", "icons-screenshots", "alpha", "large",
    "synthetic", "collision", "orientation", "profile", "multi-frame",
)

# Frozen at the Phase 1 gate (r10), plus the SSIM / PSNR rules of section 12.
AC = {
    "photographs": ("AC-2", 1.72, 0.9760, 39.009),
    "gradients": ("AC-3", 0.01, 1.0000, None),
    "icons-screenshots": ("AC-4", 0.00, 1.0000, None),
    "alpha": ("AC-5", None, 1.0000, None),
}


def num(value):
    if value in (None, "", "n/a"):
        return None
    if value == "inf":
        return math.inf
    try:
        return float(value)
    except ValueError:
        return None


def mean(values):
    flat = [v for v in values if v is not None]
    if not flat:
        return None
    if any(v == math.inf for v in flat):
        return math.inf
    return sum(flat) / len(flat)


def fmt(value, digits=3):
    if value is None:
        return "n/a"
    if value == math.inf:
        return "inf"
    return ("%." + str(digits) + "f") % value


def load(path):
    with open(path, encoding="utf-8", newline="") as handle:
        return [row for row in csv.DictReader(handle)
                if row.get("status") == "ok"]


def stats(rows):
    return {
        "de00": mean(num(r.get("de00")) for r in rows),
        "de00_p95": mean(num(r.get("de00_p95")) for r in rows),
        "psnr": mean(num(r.get("psnr")) for r in rows),
        "ssim": mean(num(r.get("ssim")) for r in rows),
        "engine_ms": mean(num(r.get("engine_ms")) for r in rows),
        "workspace": mean(num(r.get("workspace_bytes")) for r in rows),
        "base_de00": mean(num(r.get("base_de00")) for r in rows),
        "base_ssim": mean(num(r.get("base_ssim")) for r in rows),
        "base_psnr": mean(num(r.get("base_psnr")) for r in rows),
        "count": len(rows),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--srgb", required=True)
    parser.add_argument("--oklab", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--runtime-srgb-ms", type=float, default=None)
    parser.add_argument("--runtime-oklab-ms", type=float, default=None)
    args = parser.parse_args()

    srgb = load(args.srgb)
    oklab = load(args.oklab)
    a = stats(srgb)
    b = stats(oklab)
    runtime_a = (args.runtime_srgb_ms if args.runtime_srgb_ms is not None
                 else a["engine_ms"])
    runtime_b = (args.runtime_oklab_ms if args.runtime_oklab_ms is not None
                 else b["engine_ms"])

    out = io.StringIO()
    out.write("# Phase 3 comparison matrix (5.6.4)\n\n")
    out.write("Same fixture set, max_colors=256, no dither, sRGB Wu split on "
              "both rows; the working space for refinement and nearest search "
              "differs. Baseline re-measured with the pinned pngquant 2.17.0 "
              "(--nofs) for this phase.\n\n")
    out.write("| row | pipeline | n | mean dE00 | p95 dE00 | PSNR dB | SSIM | "
              "runtime ms | core bytes |\n")
    out.write("| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for label, s, runtime in (("1", a, runtime_a), ("2", b, runtime_b)):
        pipeline = "sRGB (Phase 2)" if label == "1" else "Oklab A"
        out.write("| %s | %s | %d | %s | %s | %s | %s | %s | %s |\n" % (
            label, pipeline, s["count"], fmt(s["de00"]),
            fmt(s["de00_p95"]), fmt(s["psnr"]), fmt(s["ssim"], 4),
            fmt(runtime, 1), fmt(s["workspace"], 0)))
    out.write("| 3 | Oklab B | n/a | n/a | n/a | n/a | n/a | n/a | n/a |\n\n")
    out.write("Row 3 is absent: Approach B is an optional experiment, not a "
              "v1.0.0 deliverable, and was not implemented. The "
              "`SQ_PIPELINE_OKLAB_B` enum remains valid at the ABI and resolves "
              "to the A path; no Oklab-grid statistics were added.\n\n")

    out.write("## Per-category breakdown\n\n")
    out.write("| category | n | sRGB dE00 | Oklab A dE00 | delta | "
              "Oklab wins | sRGB wins |\n")
    out.write("| :--- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for category in CATEGORIES:
        ra = [r for r in srgb if r.get("category") == category]
        rb = [r for r in oklab if r.get("category") == category]
        if not ra or not rb:
            continue
        da = mean(num(r.get("de00")) for r in ra)
        db = mean(num(r.get("de00")) for r in rb)
        wins = losses = 0
        for x in ra:
            y = next((r for r in rb if r["fixture"] == x["fixture"]), None)
            if y is None:
                continue
            vx, vy = num(x.get("de00")), num(y.get("de00"))
            if vx is None or vy is None:
                continue
            if vy < vx:
                wins += 1
            elif vy > vx:
                losses += 1
        delta = db - da if da is not None and db is not None else None
        out.write("| %s | %d | %s | %s | %+s | %d | %d |\n" % (
            category, len(ra), fmt(da), fmt(db), fmt(delta), wins, losses))

    out.write("\n## AC-2 .. AC-6 binding on the recommended pipeline (Oklab A)\n\n")
    out.write("| criterion | category | threshold | measured | baseline | "
              "paired ratio | wins / n | verdict |\n")
    out.write("| :--- | :--- | ---: | ---: | ---: | ---: | ---: | :--- |\n")
    for category, (name, de_limit, ssim_base, psnr_base) in AC.items():
        rows = [r for r in oklab if r.get("category") == category]
        if not rows:
            continue
        de = mean(num(r.get("de00")) for r in rows)
        base = mean(num(r.get("base_de00")) for r in rows)
        ratio = de / base if de and base else None
        wins = sum(1 for r in rows
                   if num(r.get("de00")) is not None
                   and num(r.get("base_de00")) is not None
                   and num(r.get("de00")) < num(r.get("base_de00")))
        if de_limit is None:
            verdict = "BLOCKED (baseline 0.000)"
        else:
            verdict = "PASS" if de is not None and de <= de_limit else "FAIL"
        out.write("| %s | %s | %s | %s | %s | %s | %d/%d | %s |\n" % (
            name, category, fmt(de_limit, 2), fmt(de), fmt(base),
            fmt(ratio), wins, len(rows), verdict))
    oklab_ssim = stats([r for r in oklab
                        if r.get("category") == "photographs"])["ssim"]
    oklab_psnr = stats([r for r in oklab
                        if r.get("category") == "photographs"])["psnr"]
    out.write("| AC-6 | photographs | >= %.4f | %s | n/a | n/a | n/a | %s |\n" % (
        AC["photographs"][2] - 0.005, fmt(oklab_ssim, 4),
        "PASS" if oklab_ssim is not None
        and oklab_ssim >= AC["photographs"][2] - 0.005 else "FAIL"))
    out.write("| AC-7 (reference) | photographs | >= %.3f | %s | n/a | n/a | n/a | %s |\n" % (
        AC["photographs"][3] - 1.0, fmt(oklab_psnr),
        "PASS" if oklab_psnr is not None
        and oklab_psnr >= AC["photographs"][3] - 1.0 else "FAIL"))

    out.write("\n## Per-image paired ratios (Oklab A vs sRGB)\n\n")
    out.write("| fixture | category | sRGB dE00 | Oklab A dE00 | delta |\n")
    out.write("| :--- | :--- | ---: | ---: | ---: |\n")
    for x in sorted(srgb, key=lambda r: r["fixture"]):
        y = next((r for r in oklab if r["fixture"] == x["fixture"]), None)
        if y is None:
            continue
        vx, vy = num(x.get("de00")), num(y.get("de00"))
        delta = vy - vx if vx is not None and vy is not None else None
        out.write("| %s | %s | %s | %s | %+s |\n" % (
            x["fixture"], x.get("category", ""), fmt(vx, 4), fmt(vy, 4),
            fmt(delta, 4)))

    text = out.getvalue()
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote %s (%d sRGB rows, %d Oklab rows)" %
          (args.out, len(srgb), len(oklab)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
