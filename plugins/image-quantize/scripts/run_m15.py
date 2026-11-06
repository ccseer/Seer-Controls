#!/usr/bin/env python3
"""M15: gap between the engine and a per-pixel Lloyd reference.

The reference runs on unique colours with their pixel counts, so it never bins
the image. It is seeded from a fixed constant and its iteration count is
printed, which is what section 8.3 asks the report to record.

A gap above 10 percent is a finding, not a gate failure, because Phase 1 is Wu
only and the refinement that closes the gap is Phase 2 work.

Usage:
    py -3 scripts/run_m15.py --bench <exe> --reference <exe> --metrics <exe>
        --out <dir> [--fixtures a.png b.png ...]
"""

import argparse
import json
import os
import subprocess
import sys

DEFAULT_FIXTURES = (
    "test/data/generated/synthetic_noise_128x128.png",
    "test/data/generated/collision_300_one_cell_20x15.png",
    "test/data/generated/alpha_soft_shadow_64x64.png",
    "test/data/real/ui_01_light.png",
    "test/data/real/photo_03_sky_gradient.png",
)

NUMERIC = ("de00_mean", "de00_p95", "psnr_db", "ssim")


def run(command):
    return subprocess.run(command, capture_output=True, text=True)


def bench(bench_exe, path, output):
    completed = run([
        bench_exe, "--input", path, "--iterations", "1", "--threads", "8",
        "--colors", "256", "--dither", "none", "--space", "srgb",
        "--output", output,
    ])
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip())
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    header = lines[0].split(",")
    return dict(zip(header, lines[-1].split(",")))


def measure(metrics_exe, source, output, report):
    completed = run([
        metrics_exe, "--input", source, "--output", output, "--out", report,
    ])
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip())
    with open(report, "r", encoding="utf-8") as handle:
        for line in handle:
            if line.startswith('{"fixture"'):
                row = json.loads(line)
                for key in NUMERIC:
                    value = row.get(key)
                    row[key] = "n/a" if isinstance(value, str) else float(value)
                return row
    raise RuntimeError("no JSON row for %s" % source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--fixtures", nargs="*", default=None)
    args = parser.parse_args()

    fixtures = args.fixtures or list(DEFAULT_FIXTURES)
    os.makedirs(args.out, exist_ok=True)
    rows = []
    for path in fixtures:
        name = os.path.basename(path)
        stem = os.path.splitext(name)[0]
        ours_png = os.path.join(args.out, stem + ".ours.png")
        ref_png = os.path.join(args.out, stem + ".ref.png")
        stats = os.path.join(args.out, stem + ".ref.json")
        bench_row = bench(args.bench, path, ours_png)
        completed = run([
            args.reference, "--input", path, "--output", ref_png,
            "--colors", "256", "--stats", stats,
        ])
        if completed.returncode != 0:
            print("%-40s SKIP %s" % (name, completed.stderr.strip()))
            continue
        reference = {}
        if os.path.isfile(stats):
            with open(stats, "r", encoding="utf-8") as handle:
                reference = json.load(handle)
        ours = measure(args.metrics, path, ours_png,
                       os.path.join(args.out, stem + ".ours.md"))
        base = measure(args.metrics, path, ref_png,
                       os.path.join(args.out, stem + ".ref.md"))
        ours_de00 = ours["de00_mean"]
        ref_de00 = base["de00_mean"]
        gap = 0.0 if ref_de00 <= 0 else (ours_de00 - ref_de00) / ref_de00 * 100.0
        rows.append({
            "fixture": name,
            "path": bench_row.get("path", ""),
            "ours_de00": ours_de00,
            "ref_de00": ref_de00,
            "gap_percent": gap,
            "ours_psnr": ours["psnr_db"],
            "ref_psnr": base["psnr_db"],
            "iterations": reference.get("iterations", ""),
            "seed": reference.get("seed", ""),
            "unique": reference.get("unique", ""),
            "sse": reference.get("sse", ""),
        })
        print("%-40s dE00 %.3f vs ref %.3f gap %+6.2f%% (path %s, ref iters %s)"
              % (name, ours_de00, ref_de00, gap, bench_row.get("path", "?"),
                 reference.get("iterations", "?")))

    summary = os.path.join(args.out, "m15.md")
    with open(summary, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# M15 histogram versus per-pixel reference\n\n")
        handle.write("| fixture | path | our dE00 | reference dE00 | gap | "
                     "ref iterations | ref seed | reference unique colours |\n")
        handle.write("| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for row in rows:
            handle.write("| %s | %s | %.3f | %.3f | %+.2f%% | %s | %s | %s |\n"
                         % (row["fixture"], row["path"], row["ours_de00"],
                            row["ref_de00"], row["gap_percent"],
                            row["iterations"], row["seed"], row["unique"]))
        worst = max((row["gap_percent"] for row in rows), default=0.0)
        handle.write("\nWorst gap: %+.2f%% (a gap above 10 percent is a finding).\n"
                     % worst)
    print("")
    print("wrote %s" % summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
