#!/usr/bin/env python3
"""Extract the tested OPD2413 DTB from the public v0.6.0 Linux setup image."""

import argparse
import hashlib
from pathlib import Path
import struct


BOOT_SHA256 = "78bd6c5596e8d2a6740d6593830457233c35355ffc14965760ffea5dc6056a45"
DTB_SHA256 = "9201ea6c07983a8c9fc74d3414bf7fcea46aa200f32b20f942bb4fc64a99133c"
RAMOOPS = (b"", b"reserved-memory", b"ramoops@bfff00000")


def extract(boot):
    if hashlib.sha256(boot).hexdigest() != BOOT_SHA256:
        raise ValueError("Expected the pinned v0.6.0 Linux setup boot image")
    if boot[:8] != b"ANDROID!" or struct.unpack_from("<I", boot, 40)[0] != 2:
        raise ValueError("Expected an Android v2 boot header")
    page, = struct.unpack_from("<I", boot, 36)
    sizes = [struct.unpack_from("<I", boot, offset)[0] for offset in (8, 16, 24, 1632, 1648)]
    start = page + sum((size + page - 1) // page * page for size in sizes[:-1])
    dtb = bytearray(boot[start:start + sizes[-1]])
    magic, size, cursor, _, _, _, _, _, _, length = struct.unpack_from(">10I", dtb)
    if magic != 0xd00dfeed or size != len(dtb) or not 40 <= cursor < cursor + length <= size:
        raise ValueError("Invalid setup DTB")

    # Use FDT_NOP so the original offsets and all other platform data remain intact.
    structure_end = cursor + length
    stack = []
    node_start = None
    matches = 0
    while cursor + 4 <= structure_end:
        token_start = cursor
        token, = struct.unpack_from(">I", dtb, cursor)
        cursor += 4
        if token == 1:
            end = dtb.index(0, cursor, structure_end)
            stack.append(bytes(dtb[cursor:end]))
            cursor = (end + 4) & ~3
            if tuple(stack) == RAMOOPS:
                node_start = token_start
        elif token == 2:
            if tuple(stack) == RAMOOPS:
                dtb[node_start:cursor] = struct.pack(">I", 4) * ((cursor - node_start) // 4)
                matches += 1
            stack.pop()
        elif token == 3:
            prop_len, = struct.unpack_from(">I", dtb, cursor)
            cursor += 8 + ((prop_len + 3) & ~3)
        elif token == 9:
            break
        elif token != 4:
            raise ValueError(f"Unexpected FDT token {token}")
    if matches != 1 or hashlib.sha256(dtb).hexdigest() != DTB_SHA256:
        raise ValueError("The extracted DTB differs from the USB desktop reference")
    return dtb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup-boot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        dtb = extract(args.setup_boot.read_bytes())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("xb") as output:
            output.write(dtb)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"prepare-erhai-dtb: {error}\n")
    print(f"{args.output} SHA-256 {DTB_SHA256}")


if __name__ == "__main__":
    main()
