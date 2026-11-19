#!/usr/bin/env python3
"""Assemble an Erhai temporary boot image from locally built USB profile inputs."""

import argparse
import hashlib
import json
import lzma
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib


TRUST_SHA256 = "7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7"
FAT_DIR = "::/EFI/UniXNU"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(args, stage):
    config = tomllib.loads(args.config.read_text(encoding="utf-8"))
    default = config["boot"]["default"]
    entry, = (item for item in config["entry"] if item["id"] == default)
    if (entry["type"] != "xnu" or entry["path"] != "Payloads/kernel.macho"
            or entry.get("initrd") != "Payloads/data.dbrdisk"
            or not {"erhai-usb=1", "erhai-block=1"} <= set(entry["args"].split())):
        raise ValueError("Default boot entry must use the Erhai USB kernel and volatile Data")
    if config.get("kits", {}).get("autoload") is not False:
        raise ValueError("The USB payload must disable automatic kit loading")
    if sha256(args.trust) != TRUST_SHA256:
        raise ValueError("Expected the pinned 21H461 original trust bundle")
    with args.data.open("rb") as stream:
        if stream.read(24) != struct.pack("<8sIIQ", b"DBRDISK1", 1, 4096, 1 << 30):
            raise ValueError("Expected the 1 GiB volatile Data image in DBRDISK1 format")
    with args.dtb.open("rb") as stream:
        if stream.read(4) != bytes.fromhex("d00dfeed"):
            raise ValueError("Expected a flattened device tree")

    kernel = args.kernel.read_bytes()
    if not kernel.startswith(bytes.fromhex("cffaedfe0c000001")):
        raise ValueError("Expected the locally linked ARM64 Mach-O kernel")
    compressed = lzma.compress(kernel, format=lzma.FORMAT_ALONE, preset=6)
    compressed = compressed[:5] + len(kernel).to_bytes(8, "little") + compressed[13:]
    if lzma.decompress(compressed) != kernel:
        raise ValueError("Kernel compression round trip failed")
    packed = stage / "kernel.compressed"
    packed.write_bytes(b"ERHLZMA1" + compressed)
    files = {
        "unixnu.toml": args.config,
        "Payloads/kernel.macho": packed,
        "Payloads/kernel.macho.adt": args.adt,
        "Payloads/kernel.macho.trustcache": args.trust,
        "Payloads/data.dbrdisk": args.data,
    }
    payload = stage / "payload.img"
    with payload.open("xb") as stream:
        stream.truncate(140 * 1024 * 1024)
    subprocess.run(["mkfs.vfat", "-n", "RAMREC2", str(payload)], check=True)
    subprocess.run(["mmd", "-i", str(payload), "::/EFI", FAT_DIR, f"{FAT_DIR}/Payloads"], check=True)
    for name, path in files.items():
        destination = f"{FAT_DIR}/{name}"
        subprocess.run(["mcopy", "-i", str(payload), str(path), destination], check=True)
        with subprocess.Popen(["mtype", "-i", str(payload), destination], stdout=subprocess.PIPE) as readback:
            digest = hashlib.file_digest(readback.stdout, "sha256").hexdigest()
            if readback.wait() or digest != sha256(path):
                raise ValueError(f"FAT readback differs for {name}")

    image = stage / "boot.img"
    subprocess.run([sys.executable, args.builder, "bootimg", "--uefi", args.uefi,
                    "--fd", args.fd, "--dtb", args.dtb, "--ramdisk", payload, "-o", image],
                   cwd=args.builder.parent, check=True)
    with image.open("rb") as stream:
        magic, kernel_size, ramdisk_size, version = struct.unpack("<8sII24xI", stream.read(44))
    report = json.loads((stage / "boot.json").read_text(encoding="utf-8"))
    if (magic != b"ANDROID!" or not kernel_size or ramdisk_size or version != 4
            or report["embedded_payload_bytes"] != payload.stat().st_size):
        raise ValueError("Expected the OPD2413 Android v4 temporary boot image")
    digest = sha256(image)
    # Publish only a fully validated output. Never replace an existing image.
    args.output.hardlink_to(image)
    print(f"{args.output} SHA-256 {digest}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name, help_text in (
        ("builder", "Erhai unixnu entry script"),
        ("uefi", "Mu-Silicium checkout used by the Erhai boot image builder"),
        ("fd", "locally built Erhai firmware volume with RAM payload support"),
        ("dtb", "OPD2413 device DTB"),
        ("kernel", "linked and prelinked ARM64 USB touch kernel"),
        ("adt", "final USB root ADT with the volatile Data entry"),
        ("trust", "original 21H461 trust-cache bundle"),
        ("data", "locally prepared DBRDISK1 Data image"),
        ("config", "final USB boot-menu TOML"),
        ("output", "new Android boot image path"),
    ):
        parser.add_argument(f"--{name}", type=lambda value: Path(value).resolve(),
                            required=True, help=help_text)
    args = parser.parse_args()
    for name in ("builder", "fd", "dtb", "kernel", "adt", "trust", "data", "config"):
        if not getattr(args, name).is_file():
            parser.error(f"--{name} requires an existing file")
    if not args.uefi.is_dir():
        parser.error("--uefi requires an existing checkout")
    if args.output.exists():
        parser.error("output already exists")
    for name in ("mkfs.vfat", "mmd", "mcopy", "mtype"):
        if not shutil.which(name):
            parser.error(f"missing host tool: {name}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".erhai-package-", dir=args.output.parent) as folder:
            package(args, Path(folder))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"package-usb-boot: {error}\n")


if __name__ == "__main__":
    main()
