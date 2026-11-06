#!/usr/bin/env python3
"""Measure the Phase 1 quality table against the pinned pngquant baseline.

For every decodable fixture the harness runs the engine through
quantize_bench, then scores three things with quantize_metrics:

1. our indexed PNG against the source,
2. the pngquant baseline PNG (produced by quantize_metrics itself) against the
   source, so the paired ratio uses the same metric implementation,
3. output and baseline byte sizes.

The CSV is the evidence. The Markdown summary carries the per-category means,
the per-image paired ratios and the win/loss counts that section 12 asks for,
plus the alpha set composited over white, black and checkerboard.

Usage:
    py -3 scripts/run_quality.py --bench <quantize_bench.exe>
        --metrics <quantize_metrics.exe> --baseline <pngquant.exe>
        --out <dir> [--dither none|fs]
"""

import argparse
import csv
import json
import math
import os
import subprocess
import sys

DECODABLE = (".png", ".gif", ".jpg", ".jpeg", ".bmp")

def mean(values):
    """Undefined metrics are None and infinite PSNR is inf; skip neither."""
    flat = [value for value in values if value is not None]
    if not flat:
        return None
    if any(value == math.inf for value in flat):
        return math.inf
    return sum(flat) / len(flat)


def fmt(value, scale=1.0, digits=3):
    if value is None:
        return "n/a"
    if value == math.inf:
        return "inf"
    return ("%." + str(digits) + "f") % (value * scale)


NUMERIC = (
    "de00_mean", "de00_p95", "de00_p99", "cie76_mean", "psnr_db", "ssim",
    "alpha_mae", "alpha_p95", "alpha_max", "composite_white",
    "composite_black", "composite_checker", "banding_step",
)


def num(value):
    """The row encodes undefined metrics as "n/a" and infinite ones as "inf"."""
    if isinstance(value, (int, float)):
        return float(value)
    if value == "inf":
        return math.inf
    return None


def category_of(name):
    stem = name.lower()
    if stem.startswith("photo_"):
        return "photographs"
    if stem.startswith("large_"):
        return "large"
    if stem.startswith("ui_"):
        return "icons-screenshots"
    if stem.startswith("alpha_"):
        return "alpha"
    if stem.startswith("collision_"):
        return "collision"
    if stem.startswith("orientation_"):
        return "orientation"
    if stem.startswith("profile_"):
        return "profile"
    if stem.startswith("multiframe_"):
        return "multi-frame"
    if stem.startswith("synthetic_color_gradient") or stem.startswith(
        "synthetic_gray_ramp"
    ) or stem.startswith("synthetic_vertical_ramp"):
        return "gradients"
    return "synthetic"


def find_fixtures(roots):
    found = []
    skipped = []
    for root in roots:
        if os.path.isfile(root):
            found.append(root)
            continue
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if not os.path.isfile(path):
                continue
            if name.lower().endswith(DECODABLE):
                found.append(path)
            else:
                skipped.append((name, "container needs the Phase 5 WIC helper"))
    return found, skipped


def run(command):
    return subprocess.run(command, capture_output=True, text=True)


def bench_row(bench, path, out_png, threads, space, dither, strength):
    command = [
        bench,
        "--input", path,
        "--iterations", "1",
        "--threads", str(threads),
        "--colors", "256",
        "--dither", dither,
        "--space", space,
        "--output", out_png,
    ]
    if dither == "fs":
        command += ["--dither-strength", "%.4f" % strength]
    completed = run(command)
    if completed.returncode != 0:
        raise RuntimeError("bench failed for %s: %s" % (path, completed.stderr.strip()))
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    header = lines[0].split(",")
    values = lines[-1].split(",")
    return dict(zip(header, values))


def metrics_json(metrics, source, output, report, baseline, dither):
    command = [
        metrics,
        "--input", source,
        "--output", output,
        "--out", report,
        "--colors", "256",
        "--dither", dither,
    ]
    if baseline:
        command += ["--baseline", baseline]
    completed = run(command)
    if completed.returncode != 0:
        raise RuntimeError(
            "metrics failed for %s: %s" % (source, completed.stderr.strip())
        )
    with open(report, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line.startswith('{"fixture"'):
                row = json.loads(line)
                for key in NUMERIC:
                    if key in row:
                        row[key] = num(row[key])
                return row
    raise RuntimeError("metrics produced no JSON row for %s" % source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", required=True)
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--fixtures", nargs="*", default=None)
    parser.add_argument("--dither", default="none", choices=["none", "fs"])
    parser.add_argument("--dither-strength", type=float, default=1.0)
    parser.add_argument("--space", default="srgb", choices=["srgb", "oklab"])
    parser.add_argument("--threads", type=int, default=8)
    args = parser.parse_args()

    roots = args.fixtures or ["test/data/real", "test/data/generated"]
    fixtures, skipped = find_fixtures(roots)
    if not fixtures:
        print("no fixtures found")
        return 2

    image_dir = os.path.join(args.out, "images")
    report_dir = os.path.join(args.out, "reports")
    os.makedirs(image_dir, exist_ok=True)
    os.makedirs(report_dir, exist_ok=True)

    rows = []
    for path in fixtures:
        name = os.path.basename(path)
        category = category_of(name)
        stem = os.path.splitext(name)[0]
        our_png = os.path.join(image_dir, stem + ".png")
        our_report = os.path.join(report_dir, stem + ".md")
        base_report = os.path.join(report_dir, stem + ".baseline.md")
        try:
            bench = bench_row(args.bench, path, our_png, args.threads,
                              args.space, args.dither, args.dither_strength)
            ours = metrics_json(
                args.metrics, path, our_png, our_report, args.baseline, args.dither
            )
            baseline_png = our_png + ".baseline.png"
            if not os.path.isfile(baseline_png):
                raise RuntimeError("baseline PNG missing for %s" % name)
            base = metrics_json(
                args.metrics, path, baseline_png, base_report, None, args.dither
            )
        except RuntimeError as error:
            print("%-44s SKIP %s" % (name, error))
            rows.append({"fixture": name, "category": category, "status": "skip"})
            continue

        our_bytes = os.path.getsize(our_png)
        base_bytes = os.path.getsize(baseline_png)
        de00_ratio = (
            ours["de00_mean"] / base["de00_mean"]
            if ours["de00_mean"] and base["de00_mean"] else 0.0
        )
        rows.append(
            {
                "fixture": name,
                "category": category,
                "status": "ok",
                "path": bench.get("path", ""),
                "engine_ms": bench.get("engine_total_ms", ""),
                "workspace_bytes": bench.get("workspace_bytes", ""),
                "de00": ours["de00_mean"],
                "de00_p95": ours["de00_p95"],
                "psnr": ours["psnr_db"],
                "ssim": ours["ssim"],
                "alpha_mae": ours["alpha_mae"],
                "alpha_p95": ours["alpha_p95"],
                "comp_white": ours["composite_white"],
                "comp_black": ours["composite_black"],
                "comp_checker": ours["composite_checker"],
                "bytes": our_bytes,
                "base_de00": base["de00_mean"],
                "base_psnr": base["psnr_db"],
                "base_ssim": base["ssim"],
                "base_comp_white": base["composite_white"],
                "base_comp_black": base["composite_black"],
                "base_comp_checker": base["composite_checker"],
                "base_bytes": base_bytes,
                "size_ratio": our_bytes / base_bytes if base_bytes else 0.0,
                "de00_ratio": de00_ratio,
                "win": 1 if ours["de00_mean"] < base["de00_mean"] else 0,
                "banding": ours["banding_step"],
                "base_banding": base["banding_step"],
            }
        )
        print(
            "%-44s %-18s dE00 %s vs %s (%sx) size %.3fx"
            % (name, category, fmt(ours["de00_mean"]), fmt(base["de00_mean"]),
               fmt(de00_ratio), rows[-1]["size_ratio"])
        )
        sys.stdout.flush()

    csv_path = os.path.join(args.out, "quality-%s.csv" % args.dither)
    fields = [
        "fixture", "category", "status", "path", "engine_ms",
        "workspace_bytes", "de00", "de00_p95",
        "psnr", "ssim", "alpha_mae", "alpha_p95", "comp_white", "comp_black",
        "comp_checker", "bytes", "base_de00", "base_psnr", "base_ssim",
        "base_comp_white", "base_comp_black", "base_comp_checker", "base_bytes",
        "size_ratio", "de00_ratio", "win", "banding", "base_banding",
    ]
    with open(csv_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in fields})

    good = [row for row in rows if row.get("status") == "ok"]
    for row in good:
        row["win"] = 1 if (row["de00"] is not None and row["base_de00"] is not None
                           and row["de00"] < row["base_de00"]) else 0
    order = ["photographs", "gradients", "icons-screenshots", "alpha", "large",
             "synthetic", "collision", "orientation", "profile", "multi-frame"]
    summary_path = os.path.join(args.out, "quality-%s.md" % args.dither)
    with open(summary_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# Quality table (%s baseline)\n\n" % args.dither)
        handle.write(
            "Our pipeline is Wu plus weighted k-means refinement in %s with %s; "
            "the baseline is pinned pngquant 2.17.0 with %s.\n\n"
            % ("Oklab (Approach A)" if args.space == "oklab" else "sRGB",
               ("dithering off" if args.dither == "none"
                else "Floyd-Steinberg at strength %.2f" % args.dither_strength),
               "--nofs" if args.dither == "none" else "default Floyd-Steinberg")
        )
        handle.write(
            "| category | n | dE00 | dE00 base | ratio | wins | PSNR | PSNR base | "
            "SSIM | SSIM base | size ratio |\n"
        )
        handle.write(
            "| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n"
        )
        for category in order:
            subset = [row for row in good if row["category"] == category]
            if not subset:
                continue
            count = len(subset)
            de00 = mean(row["de00"] for row in subset)
            base_de00 = mean(row["base_de00"] for row in subset)
            ratio = de00 / base_de00 if de00 and base_de00 else 0.0
            wins = sum(row["win"] for row in subset)
            handle.write(
                "| %s | %d | %s | %s | %s | %d/%d | %s | %s | %s | %s | %s |\n"
                % (category, count, fmt(de00), fmt(base_de00), fmt(ratio, 1.0, 3),
                   wins, count, fmt(mean(row["psnr"] for row in subset)),
                   fmt(mean(row["base_psnr"] for row in subset)),
                   fmt(mean(row["ssim"] for row in subset), 1.0, 4),
                   fmt(mean(row["base_ssim"] for row in subset), 1.0, 4),
                   fmt(mean(row["size_ratio"] for row in subset), 1.0, 3))
            )

        alpha = [row for row in good if row["category"] == "alpha"]
        if alpha:
            handle.write("\n## Alpha set, composited\n\n")
            handle.write("| background | dE00 | dE00 base | ratio |\n")
            handle.write("| :--- | ---: | ---: | ---: |\n")
            for label, key, base_key in (
                ("white", "comp_white", "base_comp_white"),
                ("black", "comp_black", "base_comp_black"),
                ("checker", "comp_checker", "base_comp_checker"),
            ):
                ours = mean(row[key] for row in alpha)
                base = mean(row[base_key] for row in alpha)
                handle.write("| %s | %s | %s | %s |\n"
                             % (label, fmt(ours), fmt(base),
                                fmt(ours / base if ours and base else 0.0, 1.0, 3)))
            handle.write(
                "\nAlpha MAE mean %s, p95 %s.\n"
                % (fmt(mean(row["alpha_mae"] for row in alpha)),
                   fmt(mean(row["alpha_p95"] for row in alpha)))
            )

        gradients = [row for row in good if row["category"] == "gradients"]
        if gradients:
            handle.write("\n## Gradient banding step (6.7)\n\n")
            handle.write("| fixture | ours | baseline |\n| :--- | ---: | ---: |\n")
            for row in gradients:
                handle.write("| %s | %s | %s |\n"
                             % (row["fixture"], fmt(row["banding"]),
                                fmt(row["base_banding"])))

        if skipped:
            handle.write("\n## Skipped fixtures\n\n")
            for name, reason in skipped:
                handle.write("- %s: %s\n" % (name, reason))
        handle.write("\nRows: %d measured, %d skipped.\n"
                     % (len(good), len(rows) - len(good)))

    print("")
    print("wrote %s and %s" % (csv_path, summary_path))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
