#!/usr/bin/env python3
"""Build the Phase 4 comparison and AC-25 / AC-26 binding tables.

Inputs are three run_quality.py CSV files measured in this phase with the same
fixture set, max_colors=256 and the same pinned baseline. Runtime is supplied
from a separate serial measurement so concurrent evidence runs do not distort
it.

Usage:
    py -3 scripts/run_phase4_compare.py
        --fs-oklab <quality-none.csv> --none-oklab <quality-none.csv>
        --fs-srgb <quality-none.csv> --out <phase4-comparison.md>
        [--p3-ms 2000.0] [--p5-ms 7000.0]
"""

import argparse
import csv
import io
import math

CATEGORIES = (
    "photographs", "gradients", "icons-screenshots", "alpha", "large",
    "synthetic", "collision", "orientation", "profile",
)

# AC-2 .. AC-5 factors from section 12, applied to a same-dither-mode baseline.
FACTORS = {
    "photographs": ("AC-2", 1.10),
    "gradients": ("AC-3", 1.10),
    "icons-screenshots": ("AC-4", 1.15),
    "alpha": ("AC-5", 1.25),
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


def category_rows(rows, category):
    return [r for r in rows if r.get("category") == category]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fs-oklab", required=True)
    parser.add_argument("--none-oklab", required=True)
    parser.add_argument("--fs-srgb", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--strength", type=float, default=1.0)
    parser.add_argument("--p3-ms", type=float, default=None)
    parser.add_argument("--p5-ms", type=float, default=None)
    args = parser.parse_args()

    fs = load(args.fs_oklab)
    no = load(args.none_oklab)
    srgb = load(args.fs_srgb)
    out = io.StringIO()

    out.write("# Phase 4 comparison and binding (11.8)\n\n")
    out.write("One fixture set, max_colors=256, Oklab Approach A unless noted, "
              "Floyd-Steinberg strength %.2f unless noted. Baseline re-measured "
              "with the pinned pngquant 2.17.0 in the same dither mode.\n\n"
              % args.strength)

    out.write("## Dither on/off and error-space rows\n\n")
    out.write("| row | pipeline | n | mean dE00 | p95 dE00 | PSNR dB | SSIM | "
              "mean runtime ms |\n")
    out.write("| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for label, rows in (("Oklab A, no dither", no),
                         ("Oklab A, FS %.2f (shipping default)" % args.strength, fs),
                         ("sRGB, FS %.2f" % args.strength, srgb)):
        out.write("| %s | %s | %d | %s | %s | %s | %s | %s |\n" % (
            label.split(",")[0], label, len(rows),
            fmt(mean(num(r.get("de00")) for r in rows)),
            fmt(mean(num(r.get("de00_p95")) for r in rows)),
            fmt(mean(num(r.get("psnr")) for r in rows)),
            fmt(mean(num(r.get("ssim")) for r in rows), 4),
            fmt(mean(num(r.get("engine_ms")) for r in rows), 1)))
    out.write("| Oklab A, alpha-scaled error | n/a | n/a | n/a | n/a | n/a | "
              "n/a | n/a |\n\n")
    out.write("The alpha-scaled variant was not implemented: 5.7 adopts it only "
              "on a measured win, and no engine switch exists without an ABI "
              "change. This row is recorded as absent, not as a pass.\n\n")

    out.write("## Per-category dE00, FS %.2f\n\n" % args.strength)
    out.write("| category | n | Oklab no dither | Oklab FS | sRGB FS | "
              "FS baseline | Oklab FS ratio |\n")
    out.write("| :--- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
    for category in CATEGORIES:
        a, b, c = (category_rows(no, category), category_rows(fs, category),
                   category_rows(srgb, category))
        if not b:
            continue
        da = mean(num(r.get("de00")) for r in a)
        db = mean(num(r.get("de00")) for r in b)
        dc = mean(num(r.get("de00")) for r in c)
        base = mean(num(r.get("base_de00")) for r in b)
        ratio = db / base if db is not None and base else None
        out.write("| %s | %d | %s | %s | %s | %s | %s |\n" % (
            category, len(b), fmt(da), fmt(db), fmt(dc), fmt(base),
            fmt(ratio)))

    out.write("\n## AC-25 binding on the shipping defaults\n\n")
    out.write("| criterion | category | factor x FS baseline | threshold | "
              "measured | paired wins / n | verdict |\n")
    out.write("| :--- | :--- | ---: | ---: | ---: | ---: | :--- |\n")
    for category, (name, factor) in FACTORS.items():
        rows = category_rows(fs, category)
        if not rows:
            continue
        base = mean(num(r.get("base_de00")) for r in rows)
        measured = mean(num(r.get("de00")) for r in rows)
        limit = factor * base if base is not None else None
        wins = sum(1 for r in rows
                   if num(r.get("de00")) is not None
                   and num(r.get("base_de00")) is not None
                   and num(r.get("de00")) < num(r.get("base_de00")))
        if category == "alpha":
            verdict = "BLOCKED (zero baseline)"
        elif limit is None:
            verdict = "n/a"
        else:
            verdict = ("PASS" if measured is not None and measured <= limit
                        else "FAIL")
        out.write("| %s | %s | %.2f x %s | %s | %s | %d/%d | %s |\n" % (
            name, category, factor, fmt(base), fmt(limit, 4),
            fmt(measured, 4), wins, len(rows), verdict))
    alpha_rows = category_rows(fs, "alpha")
    out.write("\nAlpha set composited dE00 at the shipping defaults: white %s, "
              "black %s, checker %s (baseline %s / %s / %s).\n" % (
                  fmt(mean(num(r.get("comp_white")) for r in alpha_rows)),
                  fmt(mean(num(r.get("comp_black")) for r in alpha_rows)),
                  fmt(mean(num(r.get("comp_checker")) for r in alpha_rows)),
                  fmt(mean(num(r.get("base_comp_white")) for r in alpha_rows)),
                  fmt(mean(num(r.get("base_comp_black")) for r in alpha_rows)),
                  fmt(mean(num(r.get("base_comp_checker")) for r in alpha_rows))))

    out.write("\n## AC-26 banding (6.7, gradient fixtures only)\n\n")
    out.write("| fixture | no-dither step | FS step | pinned baseline FS | "
              "1.10 x baseline | below no-dither | within 1.10x |\n")
    out.write("| :--- | ---: | ---: | ---: | ---: | :--- | :--- |\n")
    gradient_fixtures = (
        "synthetic_color_gradient_1024x1.png",
        "synthetic_gray_ramp_1024x1.png",
        "synthetic_vertical_ramp_64x256.png",
    )
    fs_by_name = {r["fixture"]: r for r in fs}
    for name in gradient_fixtures:
        row = fs_by_name.get(name)
        if row is None:
            continue
        step = num(row.get("banding"))
        if step is None:
            continue
        match = next((r for r in no if r["fixture"] == row["fixture"]), None)
        plain = num(match.get("banding")) if match else None
        base = num(row.get("base_banding"))
        upper = base * 1.10 if base is not None else None
        out.write("| %s | %s | %s | %s | %s | %s | %s |\n" % (
            row["fixture"], fmt(plain), fmt(step), fmt(base), fmt(upper),
            "PASS" if plain is not None and step < plain else "FAIL",
            "PASS" if upper is not None and step <= upper else "FAIL"))

    out.write("\n## Performance\n\n")
    out.write("| scenario | target | measured | verdict |\n")
    out.write("| :--- | ---: | ---: | :--- |\n")
    if args.p3_ms is not None:
        out.write("| P3, 4K FS | <= 2500 ms | %.1f ms | %s |\n" % (
            args.p3_ms, "PASS" if args.p3_ms <= 2500.0 else "FAIL"))
    if args.p5_ms is not None:
        out.write("| P5, 8K FS | proposed in report | %.1f ms | measured |\n" %
                  args.p5_ms)

    text = out.getvalue()
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote %s (%d/%d/%d measured rows)" %
          (args.out, len(fs), len(no), len(srgb)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
