#!/usr/bin/env python3
"""Prepare the four read-only USB inputs and their public trust anchor."""

import argparse
import hashlib
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
from types import SimpleNamespace
import uuid


BOARD = Path(__file__).resolve().parents[1] / "board.toml"
PAGANI_REV = "c888efd3ce1b817d13f28783a77cf689cb2255c9"
OS_HASH = "e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2"
SYSTEM_HASH = "b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f"
SOURCE_HASHES = {
    "044-76040-038.dmg": "3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d",
    "044-76034-038.dmg": SYSTEM_HASH,
}
VOLUMES = (
    ("OS", "OS.raw", 6077580800, OS_HASH, "04769D26-2CA0-4825-B443-E2E5F88300F2"),
    ("SystemOS", "SystemOS.dmg", 3303014400, SYSTEM_HASH, "8846FE78-2910-4297-AF97-D59BB6F8FC16"),
)
TRUST_BUNDLE_HASH = "7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(args, stage):
    revision = subprocess.check_output(
        ["git", "-C", str(args.pagani_drivers), "rev-parse", "HEAD"], text=True
    ).strip()
    if revision != PAGANI_REV:
        raise ValueError(f"Expected Pagani Drivers at {PAGANI_REV}, found {revision}")
    manifest_tool = args.pagani_drivers / "kits/PaganiVerifiedRoot/tools/manifest.py"
    make_manifest = runpy.run_path(str(manifest_tool))["make"]

    sys.path.insert(0, str(args.unixnu / "scripts"))
    from unixnu.ipsw import Ipsw, im4p_payload
    from unixnu.udif import Image

    board = tomllib.loads(BOARD.read_text(encoding="utf-8"))
    board["ipsw"]["sha256"] = SOURCE_HASHES
    source = Ipsw(SimpleNamespace(board=board, out_root=args.cache),
                  spec=args.ipsw or board["ipsw"]["url"])
    try:
        os_dmg = source.extract("OS")
        system_dmg = source.extract("Cryptex1,SystemOS")
        static = im4p_payload(source.read("StaticTrustCache"))
        system = im4p_payload(source.read("Cryptex1,SystemTrustCache"))
        trust = struct.pack("<3I", 2, 12, 12 + len(static)) + static + system
        if hashlib.sha256(trust).hexdigest() != TRUST_BUNDLE_HASH:
            raise ValueError("Original 21H461 trust-cache bundle hash mismatch")
        (stage / "original-static-system.bundle").write_bytes(trust)
    finally:
        source.zip.close()
        source.stream.close()
    for component, path in (("OS", os_dmg), ("SystemOS", system_dmg)):
        if sha256(path) != SOURCE_HASHES.get(path.name):
            raise ValueError(f"{component}: source hash mismatch for {path}")

    # Keep large outputs on one filesystem. A hard link can reuse a verified raw OS.
    if args.os_raw:
        (stage / "OS.raw").hardlink_to(args.os_raw.resolve(strict=True))
    else:
        with (stage / "OS.raw").open("xb") as stream:
            for chunk in Image(os_dmg).blocks():
                stream.write(chunk)
    (stage / "SystemOS.dmg").hardlink_to(system_dmg)

    private_key = args.private_key.resolve(strict=True)
    public_key = stage / "owner-public.pem"
    subprocess.run(["openssl", "pkey", "-in", str(private_key), "-pubout",
                    "-out", str(public_key)], check=True)
    for name, filename, size, digest, identity in VOLUMES:
        path = stage / filename
        if path.stat().st_size != size:
            raise ValueError(f"{name}: image size mismatch")
        policy = stage / name
        with path.open("rb") as stream:
            record = make_manifest(stream, size, private_key, public_key,
                                   uuid.UUID(identity), 1, policy)
        if record["image_sha256"] != digest:
            raise ValueError(f"{name}: image SHA-256 mismatch")
        (policy / "manifest.bin").rename(stage / f"{name}.manifest.bin")
        anchor = stage / "trusted-key.bin"
        candidate = (policy / "trusted-key.bin").read_bytes()
        if anchor.exists() and anchor.read_bytes() != candidate:
            raise ValueError("The two manifests have different anchors")
        anchor.write_bytes(candidate)
        shutil.rmtree(policy)
        print(f"Verified {name}: {digest}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu", type=Path, required=True)
    parser.add_argument("--pagani-drivers", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new USB input directory")
    parser.add_argument("--private-key", type=Path, required=True, help="local RSA-3072 PEM")
    parser.add_argument("--ipsw", help="local IPSW or URL, defaulting to the board's Apple CDN URL")
    parser.add_argument("--os-raw", type=Path, help="reuse an existing raw OS on the output filesystem")
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output already exists: {args.output}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".erhai-usb-", dir=args.output.parent) as temporary:
        stage = Path(temporary) / "usb"
        stage.mkdir()
        try:
            prepare(args, stage)
            stage.rename(args.output)
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            parser.exit(1, f"prepare-usb-inputs: {error}\n")


if __name__ == "__main__":
    main()
