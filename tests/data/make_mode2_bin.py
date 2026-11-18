#!/usr/bin/env python3
"""An ISO image as a CD's raw Mode 2 Form 1 track (2352-byte frames, the ISO's sectors at byte 24), for the CD CHD
of tests/iso9660_test.c. EDC and ECC are left zero: nothing here checks them.

    python3 tests/data/make_mode2_bin.py disc.iso disc_mode2.bin
"""
import sys


def bcd(n):
    return (n // 10) << 4 | n % 10


def frame(lba, data):
    at = lba + 150  # the 2-second lead-in before sector 0
    minute, second, block = at // (75 * 60), at // 75 % 60, at % 75
    sync = b"\x00" + b"\xff" * 10 + b"\x00"
    header = bytes([bcd(minute), bcd(second), bcd(block), 2])
    subheader = bytes([0, 0, 0x08, 0]) * 2  # Form 1, data
    return sync + header + subheader + data + bytes(280)


def main():
    source, out = sys.argv[1], sys.argv[2]
    with open(source, "rb") as f:
        iso = f.read()
    with open(out, "wb") as f:
        for lba in range(len(iso) // 2048):
            f.write(frame(lba, iso[lba * 2048:(lba + 1) * 2048]))


if __name__ == "__main__":
    main()
