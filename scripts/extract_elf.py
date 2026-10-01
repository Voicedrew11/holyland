#!/usr/bin/env python3
"""Extract the boot ELF from a PS2 ISO (stdlib only).

Reads SYSTEM.CNF from the ISO9660 filesystem, follows its BOOT2 entry
(e.g. ``BOOT2 = cdrom0:\\SLUS_203.18;1``) and writes that file out.

Usage:
    extract_elf.py [--all] <iso-path> <output-dir>

Writes <output-dir>/<BOOT_ELF_NAME> and prints the game id (the ELF name).
Byte-identical to the on-disc file extent.

With --all, unpacks the whole disc tree instead (the runtime reads loose files
from its working directory). Files named DUMMY*.OUT are skipped: they are disc
padding.
"""

import os
import struct
import sys

SECTOR = 2048


def parse_dir_record(buf, off):
    rec_len = buf[off]
    if rec_len == 0:
        return None, off + 1
    if off + rec_len > len(buf):
        return None, len(buf)
    rec = buf[off:off + rec_len]
    extent = struct.unpack("<I", rec[2:6])[0]
    size = struct.unpack("<I", rec[10:14])[0]
    flags = rec[25]
    name_len = rec[32]
    name = bytes(rec[33:33 + name_len])
    return {
        "extent": extent,
        "size": size,
        "is_dir": bool(flags & 0x02),
        "name": name,
    }, off + rec_len


def read_dir(f, extent, size):
    f.seek(extent * SECTOR)
    data = f.read(size)
    entries = []
    off = 0
    while off < len(data):
        if data[off] == 0:
            # pad to next sector
            off = ((off // SECTOR) + 1) * SECTOR
            continue
        rec, off = parse_dir_record(data, off)
        if rec is not None:
            entries.append(rec)
    return entries


def norm(name: bytes) -> str:
    s = name.decode("utf-8", "replace")
    if ";" in s:
        s = s.split(";")[0]
    return s.upper()


def root_entries(f):
    # PVD root record lives at sector 16, offset 156
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise SystemExit("not an ISO9660 image (bad PVD)")
    root, _ = parse_dir_record(pvd, 156)
    return read_dir(f, root["extent"], root["size"])


def extract_tree(f, entries, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for e in entries:
        n = norm(e["name"])
        if n in ("\x00", "\x01"):
            continue
        path = os.path.join(out_dir, n)
        if e["is_dir"]:
            extract_tree(f, read_dir(f, e["extent"], e["size"]), path)
        elif n.startswith("DUMMY") and n.endswith(".OUT"):
            continue
        else:
            f.seek(e["extent"] * SECTOR)
            remaining = e["size"]
            with open(path, "wb") as out:
                while remaining:
                    chunk = f.read(min(remaining, 1 << 24))
                    if not chunk:
                        raise SystemExit(f"short read extracting {n}")
                    out.write(chunk)
                    remaining -= len(chunk)
            print(f"wrote {path} ({e['size']} bytes)")


def find_file(f, path_parts):
    entries = root_entries(f)
    for i, part in enumerate(path_parts):
        want = part.upper()
        match = None
        for e in entries:
            n = norm(e["name"])
            if n in ("\x00", "\x01"):
                continue
            if n == want:
                match = e
                break
        if match is None:
            raise SystemExit(f"path component not found on ISO: {part}")
        if i == len(path_parts) - 1:
            if match["is_dir"]:
                raise SystemExit(f"BOOT path is a directory: {part}")
            return match
        if not match["is_dir"]:
            raise SystemExit(f"not a directory on ISO: {part}")
        entries = read_dir(f, match["extent"], match["size"])
    raise SystemExit("empty path")


def main():
    args = sys.argv[1:]
    extract_all = "--all" in args
    args = [a for a in args if a != "--all"]
    if len(args) != 2:
        print(f"usage: {sys.argv[0]} [--all] <iso-path> <output-dir>", file=sys.stderr)
        return 2
    iso_path, out_dir = args
    os.makedirs(out_dir, exist_ok=True)

    with open(iso_path, "rb") as f:
        cnf_rec = find_file(f, ["SYSTEM.CNF"])
        f.seek(cnf_rec["extent"] * SECTOR)
        cnf = f.read(cnf_rec["size"]).decode("utf-8", "replace")
        print(cnf.strip())

        boot = None
        for line in cnf.splitlines():
            line = line.strip()
            if line.upper().startswith("BOOT2"):
                _, _, val = line.partition("=")
                boot = val.strip()
                break
        if not boot:
            raise SystemExit("no BOOT2 entry in SYSTEM.CNF")
        # cdrom0:\SLUS_203.18;1 -> [SLUS_203.18]
        p = boot.replace("cdrom0:", "").replace("\\", "/").strip()
        parts = [c for c in p.split("/") if c]
        parts = [c.split(";")[0] for c in parts]

        rec = find_file(f, parts)
        f.seek(rec["extent"] * SECTOR)
        data = f.read(rec["size"])
        if extract_all:
            extract_tree(f, root_entries(f), out_dir)

    name = parts[-1].upper()
    out_path = os.path.join(out_dir, name)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"wrote {out_path} ({len(data)} bytes)")
    print(f"GAME_ID={name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
