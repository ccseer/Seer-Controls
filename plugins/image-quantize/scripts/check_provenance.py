#!/usr/bin/env python3
"""Provenance checker for the seer_image_quantize package (SPEC-003 2.4).

Scans a package tree for:

* forbidden third-party quantizer symbols/strings: ``pngquant``,
  ``libimagequant``, ``imagequant`` and ``magick``;
* non-English (non-ASCII) characters in C/C++ sources;
* UTF-8 BOM and CRLF line endings in text source files.

Behaviour required by the provenance contract:

* the vendored ``tools/thirdparty`` directory (indeed any directory named
  ``thirdparty``) is excluded from every check;
* the forbidden-symbol scan is restricted to C/C++ source files, so
  documentation that legitimately names the ``pngquant`` baseline tool is NOT
  flagged;
* an explicit allowlist file (``--allowlist``) can exempt named source files
  or named ``file :: term`` pairs, for example the WIC proof of concept, which
  prints the word ``pngquant`` in a blocked-baseline status line yet links no
  quantizer library.

Output: a grep proof with a ``file:line`` for every hit, allowlisted matches
printed for transparency, then a final ``CLEAN`` or ``FINDINGS: n`` line.
Exit status: 0 when clean, 1 when there are findings, 2 on a configuration
error. Uses only the Python 3.13 standard library.

Usage examples::

    python scripts/check_provenance.py
    python scripts/check_provenance.py --root path/to/image-quantize
    python scripts/check_provenance.py --allowlist scripts/provenance-allowlist.txt

Allowlist file format (one entry per line, ``#`` comments and blank lines
ignored)::

    # exempt a whole source file
    tools/wicpoc/wic_poc.cpp
    # or exempt only one forbidden term in that file
    tools/wicpoc/wic_poc.cpp :: pngquant

Paths are relative to ``--root`` (forward slashes) and may use ``*`` globs.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# Longest alternative first: "libimagequant" must be reported as a single
# token rather than also matching the substring "imagequant".
FORBIDDEN_TERMS = ("pngquant", "libimagequant", "imagequant", "magick")
FORBIDDEN_RE = re.compile(r"libimagequant|imagequant|pngquant|magick", re.IGNORECASE)

# The forbidden-symbol scan is deliberately limited to C/C++ source files so
# that Markdown/PowerShell/Python documentation discussing the pngquant
# baseline is not treated as a third-party symbol occurrence.
C_CPP_EXTS = {".c", ".cpp", ".h", ".hpp", ".cc", ".cxx", ".c++", ".hh", ".hxx"}

# BOM/CRLF is checked across the text source files of the package.
TEXT_EXTS = C_CPP_EXTS | {
    ".py", ".ps1", ".md", ".txt", ".cmake", ".json", ".rc", ".in",
}
TEXT_FILENAMES = {"cmakelists.txt", ".gitattributes", ".gitignore", ".editorconfig"}

# Human-readable list of the required vendored exclusions.
VENDORED_LABEL = "tools/thirdparty (any directory named 'thirdparty')"

UTF8_BOM = b"\xef\xbb\xbf"


@dataclass(frozen=True)
class Finding:
    category: str
    rel: str
    line: int
    detail: str

    def render(self) -> str:
        return f"{self.category} {self.rel}:{self.line}: {self.detail}"


@dataclass(frozen=True)
class Allowed:
    rel: str
    line: int
    term: str

    def render(self) -> str:
        return f"ALLOW {self.rel}:{self.line}: {self.term} (allowlisted)"


def is_excluded(rel_path: Path) -> bool:
    """True for anything under a vendored 'thirdparty' directory."""
    parts = [p.lower() for p in rel_path.parts]
    # parts[:-1] are parent directories; the last part is the file name.
    if any(part == "thirdparty" for part in parts[:-1]):
        return True
    # Also exclude hidden VCS/build directories to keep the grep proof focused.
    if any(part == ".git" for part in parts[:-1]):
        return True
    return False


def is_c_cpp_source(rel_path: Path) -> bool:
    return rel_path.suffix.lower() in C_CPP_EXTS


def is_text_file(rel_path: Path) -> bool:
    return rel_path.suffix.lower() in TEXT_EXTS or rel_path.name.lower() in TEXT_FILENAMES


class Allowlist:
    """Parses and applies an explicit allowlist file."""

    def __init__(self) -> None:
        self.entries: list[tuple[str, str | None]] = []
        self.source: Path | None = None

    def load(self, path: Path) -> None:
        self.source = path
        for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "::" in line:
                pattern, term = line.split("::", 1)
                term = term.strip().lower() or None
                self.entries.append((pattern.strip().replace("\\", "/"), term))
            else:
                self.entries.append((line.replace("\\", "/"), None))

    @staticmethod
    def _matches(rel: str, pattern: str) -> bool:
        pattern = pattern[2:] if pattern.startswith("./") else pattern
        if rel == pattern or fnmatch.fnmatchcase(rel, pattern):
            return True
        # Convenience: a bare file name matches that name anywhere in the tree.
        return "/" not in pattern and rel.endswith("/" + pattern)

    def allows(self, rel: str, term: str) -> bool:
        term_lower = term.lower()
        for pattern, allowed_term in self.entries:
            if not self._matches(rel, pattern):
                continue
            if allowed_term is None or allowed_term == term_lower:
                return True
        return False

    def describe(self) -> str:
        if self.source is None:
            return "none"
        return f"{self.source} ({len(self.entries)} entr{'y' if len(self.entries) == 1 else 'ies'})"


def scan(root: Path, allowlist: Allowlist,
         skip_paths: frozenset[Path]) -> tuple[list[Finding], list[Allowed], int]:
    findings: list[Finding] = []
    allowed: list[Allowed] = []
    scanned = 0

    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if path.resolve() in skip_paths:
            # The config/allowlist file itself is not part of the package
            # sources being proven; scanning it would only produce a
            # self-referential BOM/CRLF hit on the operator's own file.
            continue
        rel_path = path.relative_to(root)
        if is_excluded(rel_path):
            continue
        rel = rel_path.as_posix()
        scanned += 1

        try:
            data = path.read_bytes()
        except OSError as exc:  # unreadable file: report as a finding, do not crash
            findings.append(Finding("IO", rel, 0, f"cannot read file: {exc}"))
            continue
        if b"\x00" in data:
            # Binary file: no meaningful line-based checks apply.
            continue

        # --- BOM / CRLF (text source files) --------------------------------
        if is_text_file(rel_path):
            if data.startswith(UTF8_BOM):
                findings.append(Finding("BOM", rel, 1, "UTF-8 BOM (EF BB BF) present"))
            crlf_lines = [
                i for i, line in enumerate(data.split(b"\n"), start=1)
                if line.endswith(b"\r")
            ]
            if crlf_lines:
                findings.append(Finding(
                    "CRLF", rel, crlf_lines[0],
                    f"CRLF line endings on {len(crlf_lines)} line(s); "
                    f"first at line {crlf_lines[0]}",
                ))

        # --- Forbidden symbols + non-English (C/C++ sources only) ----------
        if not is_c_cpp_source(rel_path):
            continue
        text = data.decode("utf-8-sig", errors="replace")
        for line_no, line in enumerate(text.splitlines(), start=1):
            if any(ord(ch) > 127 for ch in line):
                offenders = "".join(sorted({ch for ch in line if ord(ch) > 127}))
                findings.append(Finding(
                    "NON_ENGLISH", rel, line_no,
                    "non-ASCII character(s) " + repr(offenders) +
                    " in: " + line.strip()[:120],
                ))
            for match in FORBIDDEN_RE.finditer(line):
                term = match.group(0)
                if allowlist.allows(rel, term):
                    allowed.append(Allowed(rel, line_no, term))
                else:
                    findings.append(Finding(
                        "SYMBOL", rel, line_no,
                        f"forbidden term '{term}' in: {line.strip()[:120]}",
                    ))

    return findings, allowed, scanned


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="check_provenance.py",
        description=(
            "Scan the image-quantize package for forbidden third-party "
            "quantizer symbols, non-English C/C++ source characters, and "
            "BOM/CRLF, excluding vendored thirdparty directories."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Forbidden terms (C/C++ sources only): " +
            ", ".join(FORBIDDEN_TERMS) + ".\n"
            "Exit codes: 0 = CLEAN, 1 = FINDINGS, 2 = configuration error.\n"
            "An explicit --allowlist file exempts named files or "
            "'file :: term' pairs (see module docstring for the format)."
        ),
    )
    parser.add_argument(
        "--root", type=Path, default=None,
        help="package subtree to scan (default: the parent of this script's directory)",
    )
    parser.add_argument(
        "--allowlist", type=Path, default=None,
        help="explicit allowlist file of paths / 'path :: term' pairs to exempt",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = (args.root if args.root is not None
            else Path(__file__).resolve().parent.parent).resolve()
    if not root.is_dir():
        print(f"ERROR: --root is not a directory: {root}", file=sys.stderr)
        return 2

    allowlist = Allowlist()
    if args.allowlist is not None:
        if not args.allowlist.is_file():
            print(f"ERROR: --allowlist file not found: {args.allowlist}",
                  file=sys.stderr)
            return 2
        allowlist.load(args.allowlist)

    skip_paths = frozenset()
    if allowlist.source is not None:
        skip_paths = frozenset({allowlist.source.resolve()})
    findings, allowed, scanned = scan(root, allowlist, skip_paths)

    print("PROVENANCE CHECK (SPEC-003 2.4)")
    print(f"ROOT: {root}")
    print(f"EXCLUDED: {VENDORED_LABEL}")
    print(f"SCANNED_FILES: {scanned}")
    print("SYMBOL_SCAN: C/C++ sources only (" +
          ", ".join(FORBIDDEN_TERMS) + ")")
    print(f"ALLOWLIST: {allowlist.describe()}")
    print("--- grep proof ---")
    if not findings and not allowed:
        print("(no hits)")
    for item in allowed:
        print(item.render())
    for item in findings:
        print(item.render())
    print("--- end grep proof ---")

    if findings:
        print(f"FINDINGS: {len(findings)}")
        return 1
    print("CLEAN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
