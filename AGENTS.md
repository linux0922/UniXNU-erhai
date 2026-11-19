# Reproduce the Erhai USB boot

The target is OnePlus Pad 2 Pro OPD2413 (erhai), iPadOS 17.7.11 build 21H461, with the system volumes streamed over USB. The reference run reached the lock screen, accepted a physical swipe to the Home Screen, then accepted Home Screen swipes and taps.

Start with [the USB source-build guide](docs/usb-source-build.md). Use its pinned upstream sources and artifacts from the user's own build. Read the current checkout's guide before starting a device session. Treat the status of each build stage as a prerequisite for the next.

1. Check the device model and existing boot setup. Ask for the user's approval before any reboot. Use only temporary `fastboot boot` for this profile. Keep the user's Android/Ubuntu boot setup, partitions and slots intact.
2. Run `scripts/prepare-usb-inputs.py` on Linux or WSL. Keep the private signing key outside the Git checkout. Use the same two manifests for the kernel build and for the USB server.
3. Build or locate the local UEFI, OPD2413 DTB, USB/NT36536 kernel, ADT, original trust bundle, volatile Data image and menu config described in the guide. Check every input before packaging. Report the first missing input by name if the current checkout cannot build it. The original upstream's S1–S6 diagnostic image uses a different profile.
4. Run `scripts/package-usb-boot.py` only when all inputs are available. Record the boot image SHA-256 and which local build produced the kernel and firmware. Verify that Fastboot targets the intended device before using the image.
5. After temporary boot, run `scripts/serve-usb-blocks.py --images-dir <directory>` on Windows when Apple USBMux sees the device. Leave the server running while the guest uses the volumes.
6. Ask the user to confirm the lock-screen clock and bottom bar, swipe up using the built-in screen, and confirm the Home Screen. Then ask for one horizontal swipe and to tap Settings. Record which of those actions actually worked. Frame counters and successful USB reads are progress indicators.

Push project changes to `main`, or target `main` when proposing a pull request. Publish source, scripts, hashes and redacted test evidence. Keep proprietary payloads, private keys and device identifiers in the local build workspace.
