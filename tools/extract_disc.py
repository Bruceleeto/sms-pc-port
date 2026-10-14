#!/usr/bin/env python3
"""Extract a GMSE01 disc image into a folder the port can run from.

usage: tools/extract_disc.py --disc rom/GAME.iso --out game

Writes OUT/sys (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin) and
OUT/files (every file on the disc, by its FST path), the layout Dolphin's
"Extract Entire Disc" produces and platform/dvd reads with
SMS_DISC_ROOT=OUT/files.
"""
import argparse
import os
import shutil
import sys

from bundle_disc import MAGIC, Disc, be32


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def extract(disc, out, expect_id):
    boot = disc.read(0, 0x440)
    if be32(boot, 0x1C) != MAGIC:
        sys.exit("not a GameCube disc image (no 0xC2339F3D at 0x1C)")
    game_id = boot[:6].decode("ascii", "replace")
    if expect_id and game_id != expect_id:
        sys.exit("disc is %s, expected %s (Super Mario Sunshine, North America)" % (game_id, expect_id))

    tmp = out + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    sysdir, files = os.path.join(tmp, "sys"), os.path.join(tmp, "files")
    os.makedirs(sysdir)
    os.makedirs(files)

    write(os.path.join(sysdir, "boot.bin"), boot)
    write(os.path.join(sysdir, "bi2.bin"), disc.read(0x440, 0x2000))
    apl_head = disc.read(0x2440, 0x20)
    write(os.path.join(sysdir, "apploader.img"),
          disc.read(0x2440, 0x20 + be32(apl_head, 0x14) + be32(apl_head, 0x18)))
    dol_off = be32(boot, 0x420)
    dol_head = disc.read(dol_off, 0x100)
    dol_size = max(be32(dol_head, i * 4) + be32(dol_head, 0x90 + i * 4) for i in range(18))
    write(os.path.join(sysdir, "main.dol"), disc.read(dol_off, dol_size))
    fst = disc.read(be32(boot, 0x424), be32(boot, 0x428))
    write(os.path.join(sysdir, "fst.bin"), fst)

    # FST: 12-byte entries, then the name table. A directory's size field is
    # the index one past its last child.
    count = be32(fst, 8)
    names = count * 12

    def name(i):
        start = names + (be32(fst, i * 12) & 0xFFFFFF)
        return fst[start:fst.index(b"\0", start)].decode("shift_jis")

    n_files = total = 0
    stack = [(count, files)]
    for i in range(1, count):
        while i >= stack[-1][0]:
            stack.pop()
        path = os.path.join(stack[-1][1], name(i))
        if fst[i * 12]:
            os.makedirs(path, exist_ok=True)
            stack.append((be32(fst, i * 12 + 8), path))
            continue
        off, size = be32(fst, i * 12 + 4), be32(fst, i * 12 + 8)
        with open(path, "wb") as f:
            for o in range(0, size, 8 << 20):
                f.write(disc.read(off + o, min(8 << 20, size - o)))
        n_files += 1
        total += size

    shutil.rmtree(out, ignore_errors=True)
    os.replace(tmp, out)
    print("extract_disc: %s, %d files, %.1f MiB -> %s/" % (game_id, n_files, total / 1048576.0, out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--disc", required=True, help="GMSE01 disc image (.iso/.gcm/.ciso)")
    ap.add_argument("--out", required=True, help="output folder (gets sys/ and files/)")
    ap.add_argument("--game-id", default="GMSE01", help="required game ID ('' accepts any)")
    a = ap.parse_args()
    extract(Disc(a.disc), a.out.rstrip("/"), a.game_id)


if __name__ == "__main__":
    main()
