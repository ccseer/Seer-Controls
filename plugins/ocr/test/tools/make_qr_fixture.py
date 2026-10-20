#!/usr/bin/env python3
"""Regenerates test/fixtures/qr_seer.png for ocr_test.

The fixture is a QR Code encoding "https://github.com/ccseer/Seer"
(version 3, error correction M, scale 4, quiet-zone border 4 -> 148x148 px).
Requires: pip install segno
"""

import os

import segno

PAYLOAD = "https://github.com/ccseer/Seer"


def main() -> None:
    fixtures_dir = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures"))
    os.makedirs(fixtures_dir, exist_ok=True)
    out_path = os.path.join(fixtures_dir, "qr_seer.png")

    qr = segno.make(PAYLOAD, error="m")
    qr.save(out_path, scale=4, border=4, kind="png")
    print("wrote", out_path)


if __name__ == "__main__":
    main()
