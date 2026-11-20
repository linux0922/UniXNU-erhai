"""Build just the iPadOS userspace GPU components from pinned source on macOS."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib


UPSTREAM = "a6ff7768bba6b9f285ac4c3e5b2c5d4904a016a3"
ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "scripts/turnip-erhai-s9-selectors.patch"


def run(*args):
    subprocess.run(args, check=True)


def revision(directory, ref="HEAD"):
    return subprocess.check_output(["git", "-C", directory, "rev-parse", ref], text=True).strip()


def checkout(destination, url, commit, sparse=()):
    destination.mkdir(parents=True, exist_ok=True)
    run("git", "-C", destination, "init", "-q")
    run("git", "-C", destination, "fetch", "--depth=1", url, commit)
    if sparse:
        run("git", "-C", destination, "sparse-checkout", "set", "--cone", *sparse)
    run("git", "-C", destination, "checkout", "--detach", "FETCH_HEAD")
    if revision(destination) != commit:
        raise ValueError(f"checkout revision mismatch: {destination}")


def build(source, out):
    if sys.platform != "darwin" or revision(source) != UPSTREAM or out.exists():
        raise ValueError("require macOS, pinned UniXNU source, and a new output directory")
    components = {item["id"]: item for item in tomllib.loads((source / "components.toml").read_text())["component"]}
    for name in ("Adreno740", "AdrenoPM4", "GopFramebuffer"):
        component = components[name]
        checkout(source / component["path"], component["repository"], component["revision"])
    metal = components["AdrenoMetal"]
    metal_ref = revision(source, f"HEAD:{metal['path']}")
    checkout(source / metal["path"], metal["repository"], metal_ref)
    component = components["NativeMobileDisplay"]
    display = source / ".cache/pagani-display"
    checkout(display, component["repository"], component["revision"], [component["path"]])
    shutil.copytree(display / component["path"], source / component["path"], dirs_exist_ok=True)

    header = source / "drivers/Adreno740/NativeAdrenoABI.h"
    abi_sha = hashlib.sha256(header.read_bytes()).hexdigest()
    if abi_sha != "d1c8a27d6e61787073c2c56a779ff4d5c43d86f2d23ac0c0fba0368af8dcdeae":
        raise ValueError("userspace ABI header differs from the one audited against S9")

    scripts = source / "scripts"
    pin_file = scripts / "data/adreno-compiler-inputs.json"
    pins = json.loads(pin_file.read_text())
    mesa = source / ".cache/mesa-work"
    pin = pins["mesa"]
    checkout(mesa, pin["mirror"], pin["commit"], pin["sparse_directories"])
    if revision(mesa, "HEAD^{tree}") != pin["tree"]:
        raise ValueError("pinned Mesa tree hash differs")
    patch_path = "patches/mesa/turnip-erhai-s9-selectors.patch"
    shutil.copyfile(PATCH, source / patch_path)
    pins["patches"].append(patch_path)
    pin_file.write_text(json.dumps(pins, indent=2) + "\n")
    for patch in pins["patches"]:
        run("git", "-C", mesa, "apply", source / patch)

    translator = json.loads((scripts / "data/gpu-metal-vulkan-inputs.json").read_text())["translator"]
    checkout(source / ".cache/gpu-reference/metal2vulkan", translator["repository"], translator["commit"])
    venv = source / ".cache/mesa-venv"
    run(sys.executable, "-m", "venv", venv)
    run(venv / "bin/python", "-m", "pip", "install", "-r", scripts / "requirements-adreno-compiler.txt")
    turnip_build = scripts / "build-turnip-darwin.py"
    recipe = turnip_build.read_text()
    flags = "flags = ['-target', triple, '-isysroot', sdk, '-fstack-protector-strong']"
    if recipe.count(flags) != 1:
        raise ValueError("pinned Turnip cross-build flag list differs")
    # Flex-generated source requests POSIX mode while Mesa uses Darwin extensions.
    turnip_build.write_text(recipe.replace(flags, flags[:-1] + ", '-D_DARWIN_C_SOURCE']"))
    run(sys.executable, turnip_build, "--target", "ios", "--abi-dir", header.parent)
    run(sys.executable, scripts / "build-adreno-metal-plugin.py", "--chip", "a830", "--no-replay")

    bundle = "NativeAdrenoMetal.bundle"
    shutil.copytree(source / "build/adreno-metal-plugin" / bundle, out / bundle)
    for binary in (out / bundle / "NativeAdrenoMetal", out / bundle / "libvulkan_freedreno.dylib"):
        run("codesign", "--verify", "--strict", binary)
        run("xcrun", "vtool", "-show-build", binary)
    files = sorted(path for path in out.rglob("*") if path.is_file())
    (out / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(out)}\n" for path in files
    ))


if __name__ == "__main__":
    build(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
