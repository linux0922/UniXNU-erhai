#!/usr/bin/env python3
"""Stage and build the high-RAM Erhai UEFI from pinned source checkouts."""

import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile


PATCHES = Path(__file__).resolve().parents[1] / "patches"
UEFI_REV = "66421cb7bb5694843aec8de1ca3d1de4c20cca38"
UNIXNU_REV = "c1ecc5b74c9dc4ced13bcfbd81a2c3998025a5e5"
DARWINBOOT_REV = "85332b317ee80898fc78939ea64e6ca52d3837a3"
FD = Path("Build/erhaiPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd")


def check_revision(root, expected):
    actual = subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()
    if actual != expected:
        raise ValueError(f"{root}: expected {expected}, found {actual}")


def check_memory(iomem):
    base, end = 0x8c0000000, 0x9c0000000
    ranges = [(int(a, 16), int(b, 16) + 1, name.strip()) for a, b, name in
              re.findall(r"^\s*([a-f0-9]+)-([a-f0-9]+) : ([^\r\n]+)",
                         iomem.read_text(), flags=re.M | re.I)]
    overlap = [(a, b, name) for a, b, name in ranges if a < end and b > base]
    if len(overlap) != 1 or overlap[0][2] != "System RAM" or not overlap[0][0] <= base < end <= overlap[0][1]:
        raise ValueError("The 0x8c0000000–0x9c0000000 window must be unreserved System RAM in the device iomem dump")


def apply_patch(root, filename):
    command = ["git", "apply", "--whitespace=nowarn"]
    patch = PATCHES / filename
    done = subprocess.run([*command, "--reverse", "--check", patch], cwd=root, capture_output=True)
    if done.returncode:
        subprocess.run([*command, "--check", patch], cwd=root, check=True)
        subprocess.run([*command, patch], cwd=root, check=True)


def make_overlay(repo, subtree, revision, destination, patch):
    destination.mkdir(parents=True)
    data = subprocess.check_output(["git", "-C", repo, "archive", revision, subtree])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(destination, filter="data")
    apply_patch(destination, patch)


def build(args):
    check_revision(args.uefi, UEFI_REV)
    check_revision(args.unixnu, UNIXNU_REV)
    check_revision(args.darwinboot, DARWINBOOT_REV)
    check_memory(args.iomem)
    if hashlib.sha256(args.dtb.read_bytes()).hexdigest() != (
            "9201ea6c07983a8c9fc74d3414bf7fcea46aa200f32b20f942bb4fc64a99133c"):
        raise ValueError("Expected the DTB produced by prepare-erhai-dtb.py")
    if not (args.uefi / "Binaries/erhai").is_dir():
        raise ValueError("Prepare Binaries/erhai with prepare-erhai-uefi-drivers.py")
    args.work.mkdir(parents=True)
    package = args.work / "unixnu"
    darwinboot = args.work / "darwinboot"
    make_overlay(args.unixnu, "package", UNIXNU_REV, package, "uefi-unixnu-package.patch")
    make_overlay(args.darwinboot, "DarwinBootPkg", DARWINBOOT_REV,
                 darwinboot / "drivers/DarwinBoot", "uefi-darwinboot.patch")
    apply_patch(args.uefi, "uefi-erhai.patch")
    dtb = args.uefi / "Resources/DTBs/erhai.dtb"
    if dtb.exists() and dtb.read_bytes() != args.dtb.read_bytes():
        raise ValueError(f"Existing DTB differs: {dtb}")
    if not dtb.exists():
        shutil.copyfile(args.dtb, dtb)

    if args.stage_only:
        print(f"Prepared board sources and overlays in {args.work}")
        return
    env = dict(os.environ, UNIXNU_ROOT=str(package), UNIXNU_DARWINBOOT_ROOT=str(darwinboot))
    env.setdefault("CLANGPDB_AARCH64_PREFIX", "aarch64-linux-gnu-")
    subprocess.run([sys.executable, "build_uefi.py", "-d", "erhai", "-m", "0", "-r", "DEBUG"],
                   cwd=args.uefi, env=env, check=True)
    image = (args.uefi / FD).read_bytes()
    if len(image) != 0x2a8000 or image[40:44] != b"_FVH":
        raise ValueError("Unexpected Erhai firmware volume")
    output = args.work / "recovery-high-ram.fd"
    with output.open("xb") as stream:
        stream.write(image)
    print(f"{output} SHA-256 {hashlib.sha256(image).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("uefi", "unixnu", "darwinboot", "dtb", "iomem", "work"):
        parser.add_argument(f"--{name}", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--stage-only", action="store_true", help="apply source patches without compiling")
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"build-erhai-uefi: {error}\n")


if __name__ == "__main__":
    main()
