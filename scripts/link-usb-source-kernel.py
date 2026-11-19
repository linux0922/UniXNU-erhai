#!/usr/bin/env python3
"""Link the Erhai USB objects with an existing source-kernel build and companion archive."""

import argparse
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


XNU_OBJECTS = Path("build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837")
ARCHIVE = "libErhaiUsbArmCpu.a"
REPLACEMENTS = ("GeniSpi.o", "ArmSmmu4K.o", "ErhaiVolatileData.o", "erhai-policy-index.o")
ROOT_WAIT = "waiting for verified USB root volumes"


def link(args, stage):
    objects = args.tree / XNU_OBJECTS
    bsd = objects / "bsd/DEVELOPMENT/bsd_init.o"
    record = json.loads(bsd.with_suffix(".o.json").read_text().strip().rstrip(","))
    source = Path(record["file"]).read_text()
    if (source.count(ROOT_WAIT) != 1 or
            source.count("if (!erhai_wait_for_usb_root())") != 1):
        raise ValueError("Expected the verified USB-root BSD init path")
    linked_bsd = stage / "bsd_init.o"
    compile_args = record["arguments"]
    compile_args[compile_args.index(record["file"])] = str(stage / "bsd_init.c")
    compile_args[compile_args.index("-o") + 1] = str(linked_bsd)
    (stage / "bsd_init.c").write_text(source)
    compile_result = subprocess.run(compile_args, cwd=record["directory"], capture_output=True, text=True)
    if compile_result.returncode:
        raise ValueError("BSD init compilation failed\n" + compile_result.stderr[-4000:])

    archive = stage / ARCHIVE
    shutil.copyfile(args.components, archive)
    block = stage / "ErhaiMuxBlock.o"
    subprocess.run(["llvm-objcopy", "--remove-section=__DATA,__mod_init_func",
                    args.usb_objects / "ErhaiMuxProbe.o", block], check=True)
    original_members = subprocess.check_output(["llvm-ar", "t", archive], text=True).splitlines()
    if any(original_members.count(name) != 1 for name in (block.name, *REPLACEMENTS)):
        raise ValueError("The companion archive does not match the expected USB profile")
    subprocess.run(["llvm-ar", "--format=darwin", "r", archive, block,
                    *(args.usb_objects / name for name in REPLACEMENTS)], check=True)

    filelist = (objects / "link.filelist").read_text().splitlines()
    if filelist.count(str(bsd)) != 1:
        raise ValueError("Expected one BSD init object in the XNU link input list")
    filelist[filelist.index(str(bsd))] = str(linked_bsd)
    inputs = stage / "link.filelist"
    inputs.write_text("\n".join(filelist) + "\n")
    recipe = objects / "mach.development.bcm2837.link/mach.development.bcm2837"
    link_args = shlex.split(recipe.with_suffix(".bcm2837.linkarguments").read_text())
    archives = shlex.split(recipe.with_suffix(".bcm2837.linkarchives").read_text())
    original = [i for i, value in enumerate(archives) if value.endswith(f"/{ARCHIVE}")]
    if len(original) != 1:
        raise ValueError("Expected one Erhai companion archive in the XNU link recipe")
    archives[original[0]] = f"-Wl,-force_load,{archive}"
    kernel = stage / "source-kernel.macho"
    subprocess.run([compile_args[0], "--target=arm64-apple-ios17.0", f"--ld-path={args.linker}",
                    *link_args, "-Wl,-alias_list,all-alias.exp", "-filelist", inputs,
                    "version.o", "lastkerneldataconst.o", "lastkernelconstructor.o",
                    "-o", kernel, *archives], cwd=objects, check=True)
    subprocess.run([sys.executable, args.tree / "scripts/seal-source-crypto-kernel.py", kernel],
                   cwd=args.tree, check=True)
    symbols = set(subprocess.check_output(["llvm-nm", "--defined-only", "-j", kernel],
                                          text=True).splitlines())
    required = {"_erhai_wait_for_usb_root", "_erhai_os_policy_index", "_erhai_systemos_policy_index"}
    if (not required <= symbols or "_erhai_linux_data_probe" in symbols
            or any("hostWrite4k" in symbol for symbol in symbols)
            or ROOT_WAIT.encode() not in kernel.read_bytes()):
        raise ValueError("Linked kernel does not match the USB-only profile")
    args.output.hardlink_to(kernel)
    print(f"Linked and checked USB-only root: {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("tree", "usb-objects", "components", "linker", "output"):
        parser.add_argument(f"--{name}", type=lambda value: Path(value).resolve(), required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".erhai-link-", dir=args.output.parent) as folder:
            link(args, Path(folder))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"link-usb-source-kernel: {error}\n")


if __name__ == "__main__":
    main()
