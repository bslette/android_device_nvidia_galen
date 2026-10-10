#!/usr/bin/env python3
"""Make NVIDIA's libnvrm_chip.so report 7 instead of 8 for Tegra194 capability 514.

The Shield-TV (Android 11) libnvhwcomposer.so rejects any value above 7 in hwc_device_open
(cmp w8,#7 ; b.hi -> "Can't get display information"), and the chip library's Tegra194 spec
says 8 (Tegra210: 6, Tegra186: 7). Reporting 7 lets the composer start on Xavier. Measured on
a Xavier NX 2026-10-09; the T194 init stores this 16-byte rodata block at spec+12:
    01000000 00000000 08000000 00000000     (spec+20 == capability 514)

Usage: patch_chip_caps.py FILE...    (idempotent; keeps FILE.orig)
Exit status is non-zero if a file has neither the unpatched nor the patched block exactly once.
"""
import os
import re
import shutil
import sys

UNPATCHED = bytes.fromhex("01000000" "00000000" "08000000" "00000000")
PATCHED = bytes.fromhex("01000000" "00000000" "07000000" "00000000")


def patch(path):
    data = open(path, "rb").read()
    unp = [m.start() for m in re.finditer(re.escape(UNPATCHED), data)]
    pat = [m.start() for m in re.finditer(re.escape(PATCHED), data)]
    if len(unp) == 1 and not pat:
        orig = path + ".orig"
        if not os.path.exists(orig):
            shutil.copy2(path, orig)
        out = bytearray(data)
        out[unp[0]:unp[0] + len(UNPATCHED)] = PATCHED
        with open(path, "wb") as f:
            f.write(out)
        print("%s: patched capability 514 8 -> 7 at offset %#x" % (path, unp[0] + 8))
        return 0
    if len(pat) == 1 and not unp:
        print("%s: already patched (offset %#x)" % (path, pat[0] + 8))
        return 0
    print("%s: refusing to patch: unpatched hits=%d patched hits=%d (expected exactly one)"
          % (path, len(unp), len(pat)), file=sys.stderr)
    return 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    rc = 0
    for p in sys.argv[1:]:
        rc |= patch(p)
    sys.exit(rc)
