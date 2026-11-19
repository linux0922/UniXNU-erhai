#!/usr/bin/env python3
"""Recreate the Erhai DXE input directory from local vendor firmware and Pagani wrappers."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile


STOCK_SHA256 = "ca72c899177c54a6fa2876d65c9881ba1cbb4ef3dd3afcd584e33ec1194e2dac"
DRIVER_MAP = Path(__file__).with_name("erhai-uefi-drivers.json")


def firmware_volume(data):
    if not data.startswith(b"\x7fELF\x02\x01"):
        raise ValueError("Expected the stock ELF64 uefi image")
    header_offset, = struct.unpack_from("<Q", data, 32)
    entry_size, entries = struct.unpack_from("<HH", data, 54)
    for index in range(entries):
        offset = header_offset + index * entry_size
        kind, _, file_offset, address, _, size = struct.unpack_from("<IIQQQQ", data, offset)
        if (kind, address, size) == (1, 0xa7000000, 0x2a8000):
            volume = data[file_offset:file_offset + size]
            if len(volume) == size and volume[40:44] == b"_FVH":
                return volume
    raise ValueError("Expected the stock firmware volume at 0xa7000000")


def children(obj):
    return getattr(obj, "objects", None) or ()


def filesystems(obj):
    for child in children(obj):
        if type(child).__name__ == "FirmwareFileSystem":
            yield child
        yield from filesystems(child)


def extract_pinned_drivers(data, required_hashes):
    from uefi_firmware import AutoParser

    found = {}
    root = AutoParser(firmware_volume(data)).parse()
    if root is None:
        raise ValueError("Could not parse the stock firmware volume")
    for filesystem in filesystems(root):
        for file in children(filesystem):
            for section in children(file):
                for item in (section, *children(section)):
                    if getattr(item, "type", None) != 0x10 or not item.data:
                        continue
                    digest = hashlib.sha256(item.data).hexdigest()
                    if digest in required_hashes:
                        found[digest] = item.data
    return found


def prepare(stock, pagani, output):
    data = stock.read_bytes()
    if hashlib.sha256(data).hexdigest() != STOCK_SHA256:
        raise ValueError("Stock uefi image differs from the verified input")
    mapping = json.loads(DRIVER_MAP.read_text(encoding="utf-8"))
    images = extract_pinned_drivers(data, {entry["sha256"] for entry in mapping.values()})
    for relative, entry in mapping.items():
        if not (pagani / relative).is_file():
            raise ValueError(f"Missing Pagani template path: {relative}")
        if entry["sha256"] not in images:
            raise ValueError(f"Missing verified stock module: {entry['name']}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".erhai-dxe-", dir=output.parent) as folder:
        stage = Path(folder) / "drivers"
        shutil.copytree(pagani, stage)
        for relative, entry in mapping.items():
            (stage / relative).write_bytes(images[entry["sha256"]])
        if output.exists():
            raise FileExistsError(output)
        stage.rename(output)
    print(f"{output}: prepared {len(mapping)} stock DXE modules")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-uefi", type=Path, required=True)
    parser.add_argument("--pagani-binaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new Binaries/erhai directory")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    try:
        prepare(args.stock_uefi, args.pagani_binaries, args.output)
    except (ImportError, OSError, ValueError, struct.error) as error:
        parser.exit(1, f"prepare-erhai-uefi-drivers: {error}\n")


if __name__ == "__main__":
    main()
