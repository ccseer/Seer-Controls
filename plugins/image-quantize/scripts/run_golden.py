#!/usr/bin/env python3
"""Write or verify the golden hash manifest of section 8.5.

The hash covers palette || transparent_index || indices, exactly what
quantize_bench prints in its output_sha256 column, under a fixed parameter
set. Any intended algorithm
change has to be re-baselined in the same working-tree change and explained in
the phase report.

Usage:
    py -3 scripts/run_golden.py --bench <exe> --manifest docs/golden-hashes.json --write
    py -3 scripts/run_golden.py --bench <exe> --manifest docs/golden-hashes.json --verify
"""

import argparse
import json
import os
import subprocess
import sys

# A small, representative set: both photographs and UI, the large fixture that
# exercises the cell grid, the exact and degenerate paths, the alpha set and one
# collision fixture. The full matrix lives in the determinism evidence.
GOLDEN_SET = (
    "test/data/real/photo_01_portrait_skin.png",
    "test/data/real/photo_03_sky_gradient.png",
    "test/data/real/ui_01_light.png",
    "test/data/real/ui_02_dark.png",
    "test/data/real/large_01_4k.png",
    "test/data/generated/synthetic_2color_16x16.png",
    "test/data/generated/synthetic_256color_16x16.png",
    "test/data/generated/synthetic_checkerboard_8x8.png",
    "test/data/generated/synthetic_color_gradient_1024x1.png",
    "test/data/generated/synthetic_gray_ramp_1024x1.png",
    "test/data/generated/synthetic_noise_128x128.png",
    "test/data/generated/alpha_soft_shadow_64x64.png",
    "test/data/generated/alpha_glass_64x64.png",
    "test/data/generated/collision_300_one_cell_20x15.png",
)

PARAMS = {
    "max_colors": 256,
    "dither": "none",
    "color_space": "srgb",
    "alpha_mode": "straight",
    "threads": 4,
}

LABEL = "SPEC-003 Phase 2, Wu splitter plus k-means refinement, sRGB nearest neighbour"


def bench_hash(bench, path):
    completed = subprocess.run(
        [
            bench,
            "--input", path,
            "--iterations", "1",
            "--threads", str(PARAMS["threads"]),
            "--colors", str(PARAMS["max_colors"]),
            "--dither", PARAMS["dither"],
            "--space", PARAMS["color_space"],
        ],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "bench failed for %s: %s" % (path, completed.stderr.strip())
        )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    header = lines[0].split(",")
    values = lines[-1].split(",")
    row = dict(zip(header, values))
    return row["output_sha256"], row.get("path", "")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", required=True)
    parser.add_argument("--manifest", default="docs/golden-hashes.json")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--label", default=LABEL)
    args = parser.parse_args()
    if not args.write and not args.verify:
        print("choose --write or --verify")
        return 2

    if args.write:
        entries = []
        for path in GOLDEN_SET:
            if not os.path.isfile(path):
                print("missing fixture %s" % path)
                return 1
            digest, selected = bench_hash(args.bench, path)
            entries.append({"fixture": path, "path": selected, "sha256": digest})
            print("%-58s %s path=%s" % (path, digest[:16], selected))
        document = {
            "label": args.label,
            "hash_covers": "palette || transparent_index || indices",
            "params": PARAMS,
            "compiler_flags": "/W4 /WX /utf-8 /EHsc /fp:precise, _MT",
            "fixtures": entries,
        }
        with open(args.manifest, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(document, handle, indent=2)
            handle.write("\n")
        print("wrote %s (%d fixtures)" % (args.manifest, len(entries)))
        return 0

    with open(args.manifest, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    failures = []
    for entry in document["fixtures"]:
        digest, selected = bench_hash(args.bench, entry["fixture"])
        ok = digest == entry["sha256"] and selected == entry["path"]
        print("%-58s %s" % (entry["fixture"], "ok" if ok else "MISMATCH"))
        if not ok:
            failures.append((entry["fixture"], entry["sha256"], digest,
                             entry["path"], selected))
    if failures:
        print("\nGOLDEN RESULT: FAIL")
        for fixture, expected, actual, expected_path, actual_path in failures:
            print("  %s expected %s (%s) got %s (%s)"
                  % (fixture, expected[:16], expected_path, actual[:16], actual_path))
        return 1
    print("\nGOLDEN RESULT: PASS (%d fixtures)" % len(document["fixtures"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
