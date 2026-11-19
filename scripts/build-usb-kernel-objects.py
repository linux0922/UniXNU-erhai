#!/usr/bin/env python3
"""Compile Erhai USB, touch and signed-media index objects against a prepared XNU tree."""

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import tempfile
import uuid


DEVICE = Path(__file__).resolve().parents[1]
COMPILE_HELPER_SHA = "141144670e0b7c7dcf67c571fd1cf065631762255be68cdb0f9a47cca26ccd1b"
CORE_REV = "c1ecc5b74c9dc4ced13bcfbd81a2c3998025a5e5"
FIRMWARE_SHA = "3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b"
KEYSTORE_SHA = "273b030afee897e48c30bc917c4df8a9c4d94a189d073ef80c0e0a15caef82ee"
VOLUMES = (
    ("OS", 6077580800, "04769D26-2CA0-4825-B443-E2E5F88300F2",
     "e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2"),
    ("SystemOS", 3303014400, "8846FE78-2910-4297-AF97-D59BB6F8FC16",
     "b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f"),
)


def assert_revision(repo, revision):
    actual = subprocess.check_output(["git", "-C", repo, "rev-parse", "HEAD"], text=True).strip()
    if actual != revision:
        raise ValueError(f"{repo}: expected {revision}, found {actual}")


def read_pinned(path, expected):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"SHA-256 does not match the reference input: {path}")
    return data


def build_policy(inputs, output, scratch, clang):
    lines = [".section __DATA_CONST,__const"]
    for label, size, identity, digest in VOLUMES:
        raw = (inputs / f"{label}.manifest.bin").read_bytes()
        blocks = (size + 4095) // 4096
        header_prefix = struct.pack("<8s4I2Q16s32s", b"VROOT01\0", 1, 160, 4096, 0,
                                    size, blocks, uuid.UUID(identity).bytes, bytes.fromhex(digest))
        table = memoryview(raw)[544:]
        if (not raw.startswith(header_prefix) or len(table) != blocks * 32
                or hashlib.sha256(table).digest() != raw[88:120]):
            raise ValueError(f"Invalid {label} manifest identity or block table")
        signature = scratch / "manifest-signature.bin"
        signature.write_bytes(raw[160:544])
        subprocess.run(["openssl", "dgst", "-sha256", "-verify", inputs / "owner-public.pem",
                        "-signature", signature], input=raw[:160], check=True)
        name = label.lower()
        index = scratch / f"{name}-policy-index.bin"
        page = 16 * 1024
        index.write_bytes(b"".join(hashlib.sha256(table[at:at + page]).digest()
                                   for at in range(0, len(table), page)))
        tag = f"_erhai_{name}"
        lines.extend((f".globl {tag}_policy_index", f"{tag}_policy_index:",
                      f'.incbin "{index}"', f".globl {tag}_manifest_sha384",
                      f"{tag}_manifest_sha384:",
                      ".byte " + ", ".join(map(str, hashlib.sha384(raw).digest()))))
    assembly = scratch / "erhai-policy-index.S"
    assembly.write_text("\n".join(lines) + "\n", encoding="utf-8")
    subprocess.run([clang, "-target", "arm64-apple-ios17.0", "-c", assembly,
                    "-o", output / "erhai-policy-index.o"], check=True)


def build(args, stage):
    assert_revision(args.unixnu, CORE_REV)
    assert_revision(args.unixnu / "drivers/QcomGeniSpi", "eac47baefe6a5d2368a70351e6ad1b6361bedbe2")
    assert_revision(args.unixnu / "kits/ArmSmmu4K", "83969cd874503e65997ff7639f7dcf3d0a92cd17")
    audit = json.loads(args.storage_audit.read_text(encoding="utf-8"))
    if (audit.get("original_executable_sha256") != "852b7846a9039c58fde08478416080af38ffcd5b47c08c889b28497f5f5b4061"
            or audit.get("original_metaclass") != "0xfffffff006add1d0" or audit.get("method_slots") != 190):
        raise ValueError("Storage ABI audit differs from the supported 21H461 reference")
    firmware = read_pinned(args.touch_firmware, FIRMWARE_SHA)
    keystore = args.keystore_source.read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(keystore).hexdigest() != KEYSTORE_SHA:
        raise ValueError("Expected the pinned NativeKeyStoreEvents source")
    source = stage / "sources"
    shutil.copytree(DEVICE / "drivers/ErhaiUsbTouch", source)
    (source / "NativeKeyStoreEvents.cpp").write_bytes(keystore)
    rows = (", ".join(f"0x{byte:02x}" for byte in firmware[i:i + 16]) for i in range(0, len(firmware), 16))
    (source / "ErhaiTouchFirmwareBytes.inc").write_text(",\n".join(rows) + "\n", encoding="utf-8")

    helper_path = stage / "build-pagani-battery.py"
    helper_path.write_bytes(read_pinned(args.compile_helper, COMPILE_HELPER_SHA))
    helper = runpy.run_path(str(helper_path))
    compile_one = helper["compile_one"]
    cxx = helper["load_record"](args.tree, helper["CXX_RECORD"])
    cc = helper["load_record"](args.tree, helper["C_RECORD"])
    objects = stage / "objects"
    objects.mkdir()
    geni = args.unixnu / "drivers/QcomGeniSpi"
    smmu = args.unixnu / "kits/ArmSmmu4K"
    verified = args.tree / "kits/VerifiedRoot"
    dirs = (args.tree / ".cache/xnu-work/osfmk", args.tree / ".cache/xnu-work/bsd",
            args.tree / "build/storage-abi/include", DEVICE / "drivers/NativeUfs",
            verified / "include", geni, smmu)
    for directory in dirs:
        if not directory.is_dir():
            raise ValueError(f"Missing XNU staging dependency: {directory}")
    switches = (
        "ERHAI_WITH_STORAGE=1", "ERHAI_ORIGINAL_STORAGE_META=0xfffffff006add1d0ull",
        "ERHAI_HOME_INPUT=1", "ERHAI_HID_CONFIDENCE=1", "ERHAI_HID_COLLECTION=1",
        "ERHAI_HID_IDENTITY=1", "ERHAI_HID_EARLY=1", "ERHAI_TOUCH_IRQ_GATE=1",
        "ERHAI_TOUCH_IRQ_LATCH=1", "ERHAI_TOUCH_AUTO=1", "ERHAI_TOUCH_PROBE=1",
        "ERHAI_TOUCH_DMA=1", "ERHAI_VERIFY_STORAGE=1", "ERHAI_READAHEAD=0",
        "ERHAI_VERIFIED_CACHE=1", "ERHAI_POLICY_CACHE_PAGES=64",
        "ERHAI_KEYSTORE_EVENTS=1", "ERHAI_USB_ROOT=1",
    )
    flags = ["-mgeneral-regs-only", *helper["WARNINGS"], "-Wno-missing-field-initializers"]
    compile_one(cxx, source / "ErhaiMuxProbe.cpp", objects / "ErhaiMuxProbe.o",
                flags + [f"-I{directory}" for directory in dirs] + [f"-D{flag}" for flag in switches])
    for driver in (geni / "GeniSpi.c", smmu / "ArmSmmu4K.c"):
        obj = objects / f"{driver.stem}.o"
        subprocess.run([args.clang, "-target", "arm64-apple-ios17.0", "-ffreestanding",
                        "-fno-stack-protector", "-mgeneral-regs-only", "-O2",
                        "-c", driver, "-o", obj], check=True)
        helper["audit"](obj)
    compile_one(cc, source / "ErhaiVolatileData.c", objects / "ErhaiVolatileData.o", [])
    build_policy(args.usb_inputs, objects, stage, args.clang)
    helper["audit"](objects / "erhai-policy-index.o")
    objects.rename(args.output)
    print(f"Built {args.output}. Remaining kernel stages must bind and link these objects.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("tree", "unixnu", "compile-helper", "storage-audit",
                 "keystore-source", "touch-firmware", "usb-inputs", "output"):
        parser.add_argument(f"--{name}", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--clang", required=True, help="LLVM clang for ARM64 iOS objects")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".erhai-kernel-objects-", dir=args.output.parent) as folder:
            build(args, Path(folder))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"build-usb-kernel-objects: {error}\n")


if __name__ == "__main__":
    main()
