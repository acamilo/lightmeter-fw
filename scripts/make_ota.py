#!/usr/bin/env python3
# Wrap an ESP-IDF application binary in a Zigbee OTA Upgrade File so it can
# be served by ZHA, Zigbee2MQTT, or any other ZCL OTA server.
#
# Reference: Zigbee Cluster Library spec §11 (OTA Upgrade file format).
# Header is little-endian; the single "Upgrade Image" sub-element (tag 0x0000)
# carries the raw ESP-IDF .bin.
#
# The three identity values (manufacturer, image_type, stack_version) must
# match what the device firmware advertises on its OTA cluster — see
# LIGHTMETER_MANUFACTURER / LIGHTMETER_IMAGE_TYPE in main/main.c.

import argparse
import struct
import sys
from pathlib import Path

OTA_UPGRADE_FILE_IDENTIFIER = 0x0BEEF11E
OTA_HEADER_VERSION          = 0x0100
ZIGBEE_STACK_VERSION_PRO    = 0x0002
TAG_UPGRADE_IMAGE           = 0x0000


def build_ota(binary: bytes, manufacturer: int, image_type: int,
              file_version: int, header_string: str) -> bytes:
    # Header tag: 56 bytes (no optional fields).
    header_length    = 56
    field_control    = 0x0000
    hdr_string_bytes = header_string.encode("utf-8")[:32].ljust(32, b"\x00")

    # Upgrade Image sub-element: 2-byte tag id + 4-byte length + payload.
    subelement = struct.pack("<HI", TAG_UPGRADE_IMAGE, len(binary)) + binary
    total_image_size = header_length + len(subelement)

    header = struct.pack(
        "<IHHHHHIH32sI",
        OTA_UPGRADE_FILE_IDENTIFIER,
        OTA_HEADER_VERSION,
        header_length,
        field_control,
        manufacturer,
        image_type,
        file_version,
        ZIGBEE_STACK_VERSION_PRO,
        hdr_string_bytes,
        total_image_size,
    )
    return header + subelement


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input",  help="ESP-IDF application .bin")
    p.add_argument("output", help="Output .ota file")
    p.add_argument("--manufacturer", required=True,
                   help="Manufacturer code (hex, e.g. 0x1289)")
    p.add_argument("--image-type",   required=True,
                   help="Image type (hex, e.g. 0x0001)")
    p.add_argument("--version",      required=True,
                   help="File version (hex, e.g. 0x00000002 — must be > current)")
    p.add_argument("--header-string", default="lightmeter",
                   help="Human-readable tag embedded in the header (<=32 bytes)")
    args = p.parse_args()

    manufacturer = int(args.manufacturer, 16)
    image_type   = int(args.image_type,   16)
    file_version = int(args.version,      16)

    raw = Path(args.input).read_bytes()
    ota = build_ota(raw, manufacturer, image_type, file_version, args.header_string)
    Path(args.output).write_bytes(ota)

    print(f"wrote {args.output}: {len(ota)} bytes "
          f"(manuf=0x{manufacturer:04x} type=0x{image_type:04x} "
          f"version=0x{file_version:08x})")


if __name__ == "__main__":
    sys.exit(main() or 0)
