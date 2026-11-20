"""Verify and stage the Metal probe and plugin side by side for a test process."""

import argparse
import hashlib
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile


BUNDLE = "NativeAdrenoMetal.bundle"
PROBE = "erhai-metal-smoke"
PLUGIN_FILES = {
    f"{BUNDLE}/Info.plist",
    f"{BUNDLE}/NativeAdrenoMetal",
    f"{BUNDLE}/_CodeSignature/CodeResources",
    f"{BUNDLE}/gpu-build.json",
    f"{BUNDLE}/libvulkan_freedreno.dylib",
}


def verify(data, digest, name):
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"SHA-256 mismatch: {name}")


def stage(plugin, probe_dir, output):
    if output.exists():
        raise ValueError(f"output already exists: {output}")
    with tarfile.open(plugin, "r:gz") as tar:
        members = [entry for entry in tar.getmembers() if not entry.isdir()]
        entries = {entry.name.removeprefix("./"): entry for entry in members}
        if (len(entries) != len(members) or set(entries) != PLUGIN_FILES | {"SHA256SUMS"}
                or any(not entry.isfile() for entry in members)):
            raise ValueError("unexpected files in the plugin archive")
        sums = tar.extractfile(entries["SHA256SUMS"]).read().decode().splitlines()
        pins = {name: digest for digest, name in (line.split("  ", 1) for line in sums)}
        if len(sums) != len(pins) or set(pins) != PLUGIN_FILES:
            raise ValueError("unexpected plugin checksum manifest")
        files = {name: tar.extractfile(entries[name]).read() for name in pins}
    for name, data in files.items():
        verify(data, pins[name], name)
    probe_sum = (probe_dir / "SHA256SUMS").read_text().split()
    if len(probe_sum) != 2 or probe_sum[1] != PROBE:
        raise ValueError("unexpected probe checksum manifest")
    probe = (probe_dir / PROBE).read_bytes()
    verify(probe, probe_sum[0], PROBE)
    files[PROBE] = probe
    files["NativeAdrenoMetal.publish"] = b"1\n"

    executable = {PROBE, f"{BUNDLE}/NativeAdrenoMetal", f"{BUNDLE}/libvulkan_freedreno.dylib"}
    with tempfile.TemporaryDirectory(dir=output.parent) as directory:
        staging = Path(directory) / "stage"
        staging.mkdir()
        for name, data in files.items():
            path = staging.joinpath(*PurePosixPath(name).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if name in executable else 0o644)
        (staging / "SHA256SUMS").write_text("".join(
            f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(files.items())
        ))
        staging.rename(output)
    print(f"Verified and staged {len(files)} files at {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for entry in ("plugin", "probe_dir", "output"):
        parser.add_argument(entry, type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    try:
        stage(args.plugin, args.probe_dir, args.output)
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.exit(1, f"{error}\n")
