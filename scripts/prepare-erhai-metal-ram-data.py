"""Create a disposable APFS Data seed containing only the Metal diagnostic."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import runpy
import stat
import subprocess
import sys
import tempfile
import uuid


STAGE_NAME = "erhai-s9-metal-smoke-private"
STAGE_RECIPE = Path(__file__).with_name("stage-erhai-metal-smoke.py")


def make_seed(stage, packer, output):
    if sys.platform != "darwin" or os.geteuid() != 0 or output.exists():
        raise ValueError("require macOS root and a fresh output directory")
    layout = runpy.run_path(str(STAGE_RECIPE))
    expected = layout["PLUGIN_FILES"] | {layout["PROBE"], "NativeAdrenoMetal.publish"}
    manifest = [line.split("  ", 1) for line in (stage / "SHA256SUMS").read_text().splitlines()]
    if len(manifest) != len(expected) or {name for _, name in manifest} != expected:
        raise ValueError("expected the seven-file Metal diagnostic stage")
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        root = Path(temp)
        image = root / "data.cdr"
        mount = root / "mounted"
        mount.mkdir()
        subprocess.run(["hdiutil", "create", "-size", "1024m", "-layout", "NONE", "-fs", "APFS",
                        "-volname", "ErhaiMetalRAM", "-type", "UDTO", image], check=True)
        attached = plistlib.loads(subprocess.check_output(
            ["hdiutil", "attach", "-nobrowse", "-owners", "on", "-mountpoint", mount, "-plist", image]))
        device = attached["system-entities"][0]["dev-entry"]
        try:
            volume, = (entity for entity in attached["system-entities"] if entity.get("mount-point") == str(mount))
            info = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", volume["dev-entry"]]))
            volume_uuid = str(uuid.UUID(info["VolumeUUID"])).upper()
            for name in ("tmp", "run", "root/Library/Caches"):
                (mount / name).mkdir(parents=True, exist_ok=True)
            (mount / "tmp").chmod(0o1777)
            installed = mount / "tmp" / STAGE_NAME
            installed.mkdir(mode=0o700)
            for digest, name in manifest:
                source = stage / name
                content = source.read_bytes()
                if hashlib.sha256(content).hexdigest() != digest:
                    raise ValueError(f"stage checksum differs: {name}")
                path = installed / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
                mode = stat.S_IMODE(source.stat().st_mode)
                path.chmod(mode)
                if (hashlib.sha256(path.read_bytes()).hexdigest() != digest or path.stat().st_uid != 0 or
                        stat.S_IMODE(path.stat().st_mode) != mode):
                    raise ValueError(f"APFS readback differs: {name}")
        finally:
            subprocess.run(["hdiutil", "detach", device], check=True)
        with image.open("rb") as disk:
            if image.stat().st_size != 1 << 30 or disk.read(4096)[32:36] != b"NXSB":
                raise ValueError("unexpected APFS image geometry")
        final = root / "out"
        final.mkdir()
        packed = final / "data.dbrdisk"
        runpy.run_path(str(packer))["pack"](image, packed)
        report = {"volume_uuid": volume_uuid, "logical_bytes": 1 << 30,
                  "files": dict((name, digest) for digest, name in manifest),
                  "dbrdisk_sha256": hashlib.sha256(packed.read_bytes()).hexdigest()}
        (final / "metal-ram-seed.json").write_text(json.dumps(report, indent=2) + "\n")
        final.rename(output)
    print(f"Verified APFS RAM seed {volume_uuid} at {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("stage", "packer", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    try:
        make_seed(args.stage, args.packer, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
