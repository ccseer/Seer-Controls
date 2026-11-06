#!/usr/bin/env python3
"""Run the section 8.6 determinism matrix over the fixture set.

Every sample is one process: the harness launches quantize_bench once per
(thread count, repetition, process) triple and records the SHA-256 of
palette || indices that the bench prints. Section 8.6 asks for four thread
counts, three repetitions and two separate processes, and every hash has to be
identical for a fixture, including the selected path.

The CSV written by this script is the evidence; the Markdown summary is what
the phase report quotes.

Usage:
    py -3 scripts/run_determinism.py --bench <quantize_bench.exe> --out <dir>
"""

import argparse
import csv
import json
import os
import subprocess
import sys

THREAD_COUNTS = (1, 2, 4, 8)
REPETITIONS = 3
PROCESSES = 2

# stb_image decodes PNG, GIF, JPEG, BMP, TGA and PSD. TIFF and WebP need the
# shipping helper's WIC path, which is Phase 5 work, so they are recorded as
# skipped rather than silently dropped.
DECODABLE = (".png", ".gif", ".jpg", ".jpeg", ".bmp")


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
                skipped.append((path, "container needs the Phase 5 WIC helper"))
    return found, skipped


def run_bench(bench, path, threads, space):
    command = [
        bench,
        "--input", path,
        "--iterations", "1",
        "--threads", str(threads),
        "--colors", "256",
        "--dither", "none",
        "--space", space,
    ]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            "bench failed for %s at %d threads: %s"
            % (path, threads, completed.stderr.strip())
        )
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError("bench produced no data row for %s" % path)
    header = lines[0].split(",")
    values = lines[-1].split(",")
    row = dict(zip(header, values))
    row["sha256"] = row.get("output_sha256", "")
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--fixtures", nargs="*", default=None)
    parser.add_argument("--threads", default="1,2,4,8")
    parser.add_argument("--repetitions", type=int, default=REPETITIONS)
    parser.add_argument("--processes", type=int, default=PROCESSES)
    parser.add_argument("--label", default="")
    parser.add_argument("--space", default="srgb", choices=["srgb", "oklab"])
    args = parser.parse_args()

    roots = args.fixtures or ["test/data/real", "test/data/generated"]
    fixtures, skipped = find_fixtures(roots)
    if not fixtures:
        print("no fixtures found under %s" % ", ".join(roots))
        return 2

    threads = [int(value) for value in args.threads.split(",") if value]
    os.makedirs(args.out, exist_ok=True)
    samples_path = os.path.join(args.out, "determinism-samples.csv")
    summary_path = os.path.join(args.out, "determinism.md")

    rows = []
    failures = []
    for path in fixtures:
        name = os.path.basename(path)
        hashes = set()
        paths = set()
        for repetition in range(1, args.repetitions + 1):
            for process in range(1, args.processes + 1):
                for thread_count in threads:
                    row = run_bench(args.bench, path, thread_count,
                                   args.space)
                    rows.append(
                        {
                            "fixture": name,
                            "threads": thread_count,
                            "repetition": repetition,
                            "process": process,
                            "path": row.get("path", ""),
                            "engine_ms": row.get("engine_total_ms", ""),
                            "sha256": row["sha256"],
                        }
                    )
                    hashes.add(row["sha256"])
                    paths.add(row.get("path", ""))
        status = "ok" if len(hashes) == 1 and len(paths) == 1 else "MISMATCH"
        if status != "ok":
            failures.append((name, sorted(hashes), sorted(paths)))
        print("%-44s %-3s hashes=%d %s" % (name, status, len(hashes),
                                           sorted(hashes)[0][:16]))
        sys.stdout.flush()

    with open(samples_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "fixture",
                "threads",
                "repetition",
                "process",
                "path",
                "engine_ms",
                "sha256",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    with open(summary_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# Determinism matrix (SPEC-003 section 8.6)\n\n")
        handle.write("%s\n\n" % args.label)
        handle.write(
            "Thread counts %s; %d repetitions; %d independent processes; "
            "every sample is one bench process.\n\n"
            % (threads, args.repetitions, args.processes)
        )
        handle.write("| fixture | samples | distinct hashes | path | status |\n")
        handle.write("| :--- | ---: | ---: | :--- | :--- |\n")
        for path in fixtures:
            name = os.path.basename(path)
            subset = [row for row in rows if row["fixture"] == name]
            distinct = {row["sha256"] for row in subset}
            selected = {row["path"] for row in subset}
            status = "PASS" if len(distinct) == 1 and len(selected) == 1 else "FAIL"
            handle.write(
                "| %s | %d | %d | %s | %s |\n"
                % (name, len(subset), len(distinct), ",".join(sorted(selected)), status)
            )
        if skipped:
            handle.write("\nSkipped fixtures:\n\n")
            for path, reason in skipped:
                handle.write("- %s: %s\n" % (os.path.basename(path), reason))
        handle.write("\nRESULT: %s\n" % ("PASS" if not failures else "FAIL"))

    print("")
    print("RESULT: %s (%d fixtures, %d samples)" % (
        "PASS" if not failures else "FAIL", len(fixtures), len(rows)))
    for name, hashes, paths in failures:
        print("  MISMATCH %s hashes=%s paths=%s" % (name, hashes, paths))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
