#!/usr/bin/env python3
"""Generate a Boot V2 State image matching one App binary."""

import argparse
import pathlib
import struct
import zlib


PAGE_SIZE = 0x800
STATE_MAGIC = 0x42535632
STATE_VERSION = 3
STATE_CONFIRMED = 4
APP_START = 0x08008000
APP_END = 0x0803DFFF
APP_MAX_SIZE = APP_END - APP_START + 1
RECORD = struct.Struct("<IHHIIBBBBII32sII32sIBB2sI")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    app = args.app.read_bytes()
    if not 8 <= len(app) <= APP_MAX_SIZE:
        raise SystemExit("App size is outside the configured App region")
    initial_sp, reset = struct.unpack_from("<II", app)
    if (initial_sp & 0x2FFE0000) != 0x20000000:
        raise SystemExit(f"Invalid App initial SP: 0x{initial_sp:08X}")
    if not APP_START <= reset <= APP_END:
        raise SystemExit(f"Invalid App reset vector: 0x{reset:08X}")

    version = args.version.encode("ascii")
    if not version or len(version) >= 32:
        raise SystemExit("Version must fit the 32-byte Boot State field")
    if b"TOP_FLYING_WING_FW_VERSION=" + version + b"\0" not in app:
        raise SystemExit("Boot State version does not match the App firmware marker")
    image_crc = zlib.crc32(app) & 0xFFFFFFFF
    fields = (
        STATE_MAGIC,
        STATE_VERSION,
        RECORD.size,
        1,
        STATE_CONFIRMED,
        0,
        0,
        0,
        0,
        len(app),
        image_crc,
        version,
        len(app),
        image_crc,
        version,
        0,
        0,
        0,
        b"\0\0",
        0,
    )
    record = RECORD.pack(*fields)
    record = record[:-4] + struct.pack("<I", zlib.crc32(record[:-4]) & 0xFFFFFFFF)
    if RECORD.unpack(record)[-1] != zlib.crc32(record[:-4]) & 0xFFFFFFFF:
        raise SystemExit("Boot State CRC verification failed")

    image = record + b"\xFF" * (PAGE_SIZE - len(record))
    image += b"\xFF" * PAGE_SIZE
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"App:        {args.app.resolve()}")
    print(f"Size:       {len(app)}")
    print(f"CRC32:      0x{image_crc:08X}")
    print(f"Version:    {args.version}")
    print(f"BootState:  {args.output.resolve()}")
    print(f"Load:       0x0803E800")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
