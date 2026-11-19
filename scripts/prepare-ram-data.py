#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, UniXNU contributors. All rights reserved.
# See docs/LICENSE-Pagani.txt for the retained upstream license.
"""Prepare volatile APFS Data from the pinned iPadOS 21H461 OS, preserving var ownership.

Adapted from the OnePlus 13T device repo (UniXNU-Pagani_SM8750) scripts/prepare-ram-data.py.
Changes for this tablet: the original Apple DMG members are attached directly with
hdiutil (no UDIF decode), the SystemOS hash is validated through its mounted
contents instead of a decoded-file hash, and DBRDISK1 packing is left to the host.

Requires macOS and root.
"""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import plistlib
import runpy
import shutil
import stat
import subprocess
import sys
import uuid

# Same-build hardware wait/crash loops documented by UniXNU-sheng and UniXNU-tb128fu.
DISABLED_JOBS = {
    'com.apple.watchdogd': ['/usr/libexec/watchdogd'],
    'com.apple.identityservicesd': ['/System/Library/PrivateFrameworks/IDS.framework/identityservicesd.app/identityservicesd'],
    'com.apple.locationd': ['/usr/libexec/locationd'],
    'com.apple.CommCenter': ['/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter'],
    'com.apple.bluetoothd': ['/usr/sbin/bluetoothd'],
    'com.apple.PerfPowerServices': ['/usr/libexec/PerfPowerServices'],
    'com.apple.iomfb_fdr_loader': ['/usr/bin/IOMFB_FDR_Loader', '--launchd_boot'],
    'com.apple.iconservices.iconservicesagent': ['/System/Library/CoreServices/iconservicesagent'],
}
COMPAT_FIXUPS = (
    ('system', 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.15', 0x421e8bc, '28ad1bb0008544f9'),
    ('system', 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.13', 0x1b05bec, '44008052'),
    ('system', 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.13', 0x1b05b08, '44008052'),
    ('os', 'System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver/BTAudioHALPlugin', 0x38bec, 'ffc300d1f44f01a9'),
)
OS_SHA256 = '3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d'


def verify_userspace_compat(original, system):
    for volume, relative, offset, expected in COMPAT_FIXUPS:
        if volume == 'system' and system is None:
            # The archived Cryptex SystemOS member is a sealed Apple container that
            # hdiutil cannot attach; the decoded image was verified on the host.
            continue
        path = (original if volume == 'os' else system) / relative
        with path.open('rb') as stream:
            stream.seek(offset)
            actual = stream.read(len(bytes.fromhex(expected)))
        if actual.hex() != expected:
            raise ValueError('Unexpected 21H461 compatibility instructions: ' + relative)


def prepare_userspace(target):
    for relative in ('containers/Bundle', 'containers/Data/System',
                     'containers/Shared/SystemGroup', 'containers/Temp'):
        (target / relative).mkdir(mode=0o755, parents=True, exist_ok=True)
    preferences = target / 'mobile/Library/Preferences'
    preferences.mkdir(mode=0o755, parents=True, exist_ok=True)
    for relative in (
        'containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist',
        'mobile/Library/Preferences/com.apple.MobileGestalt.plist',
        'mobile/Library/Preferences/com.apple.mobilegestalt.plist',
    ):
        (target / relative).unlink(missing_ok=True)
    owner = (target / 'mobile').stat()
    settings = {
        'com.apple.springboard': dict(
            SBDisableAutoDim=True, SBDontDimOrLockOnAC=True, SBDisableIdleSleep=True,
            BKNoWatchdogs=True, FBLaunchWatchdogExceptions={'com.apple.purplebuddy': 100.0},
            MCForceShowCloudConfigurationUI=False),
        'com.apple.migration': dict(LastSystemVersion='21H461',
            DMUserDataDisposition={'SyntheticDidUpgrade': False, 'buildVersion': '21H461'}),
        'com.apple.purplebuddy': dict(SetupDone=True, SetupFinishedAllSteps=True,
            SetupVersion=11, daemonizeNeedsToRun=False),
        'com.apple.purplebuddy.notbackedup': dict.fromkeys((
            'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy', 'ShouldRunKeychainSyncMiniBuddy',
            'ShouldRunMesaMiniBuddy', 'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
            'SilentICDPUpgrade'), False),
        'com.apple.managedconfiguration.notbackedup': dict(
            LockdownActivationIndicatesCloudConfigurationAvailable=False),
    }
    for name, additions in settings.items():
        path = preferences / (name + '.plist')
        values = plistlib.loads(path.read_bytes()) if path.exists() else {}
        values.update(additions)
        path.write_bytes(plistlib.dumps(values, fmt=plistlib.FMT_BINARY))
        os.chown(path, owner.st_uid, owner.st_gid)
        path.chmod(0o600)


def prepare_launch_policy(target, daemons):
    for label, arguments in DISABLED_JOBS.items():
        job = plistlib.loads((daemons / (label + '.plist')).read_bytes())
        if job.get('Label') != label or job.get('ProgramArguments') != arguments:
            raise ValueError('Unexpected original launch policy: ' + label)
    for relative in ('db', 'db/com.apple.xpc.launchd'):
        directory = target / relative
        if directory.is_symlink():
            raise ValueError('Refuse symlink in launch policy path')
        directory.mkdir(mode=0o755, exist_ok=True)
    paths = [directory / name for name in ('disabled.plist', 'disabled.migrated')]
    if any(path.exists() or path.is_symlink() for path in paths):
        raise ValueError('Refuse to overwrite an existing launch policy')
    policy = dict.fromkeys(DISABLED_JOBS, True)
    for path, data in zip(paths, (plistlib.dumps(policy), b'')):
        with path.open('xb') as stream:
            stream.write(data)
        path.chmod(0o644)
        if path.read_bytes() != data:
            raise ValueError('Launch policy readback mismatch')
    return list(policy)


@contextmanager
def mounted(image, target, readonly=False):
    target.mkdir(parents=True, exist_ok=True)
    options = ['-readonly', '-noautofsck'] if readonly else []
    attached = plistlib.loads(subprocess.check_output([
        'hdiutil', 'attach', '-nobrowse', '-owners', 'on', *options,
        '-mountpoint', str(target), '-plist', str(image)]))
    device = attached['system-entities'][0]['dev-entry']
    try:
        volumes = [e for e in attached['system-entities'] if e.get('mount-point') == str(target)]
        if len(volumes) != 1:
            raise ValueError('Require exactly one mounted volume')
        yield volumes[0]['dev-entry']
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)


def prepare(source, systemos, output, data_bytes, packer=None):
    if sys.platform != 'darwin':
        raise ValueError('Preparing the APFS seed requires macOS and hdiutil')
    if os.geteuid() != 0:
        raise ValueError('Run as root to preserve original uid/gid and modes')
    if output.exists():
        raise ValueError('Refuse to overwrite an existing RAM Data seed')
    with source.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != OS_SHA256:
            raise ValueError('OS input hash mismatch')
    output.mkdir(parents=True)
    image = output / 'data.cdr'
    subprocess.run(['hdiutil', 'create', '-size', f'{data_bytes // 1024**2}m', '-layout', 'NONE',
                    '-fs', 'APFS', '-volname', 'OwnerData', '-type', 'UDTO', str(image)], check=True)
    original, target = output / 'os', output / 'data'
    count = total = 0
    with mounted(source, original, readonly=True):
        if systemos is not None:
            with mounted(systemos, output / 'systemos', readonly=True):
                verify_userspace_compat(original, output / 'systemos')
        else:
            verify_userspace_compat(original, None)
        if hashlib.sha256((original / 'sbin/launchd').read_bytes()).hexdigest() != \
                '8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25':
            raise ValueError('Original launchd identity mismatch')
        with mounted(image, target) as device:
            info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', device]))
            identity = str(uuid.UUID(info['VolumeUUID'])).upper()
            source_var = original / 'private/var'
            for current, dirs, files in os.walk(source_var, followlinks=False):
                dirs[:] = [name for name in dirs if name != 'staged_system_apps']
                dest = target / Path(current).relative_to(source_var)
                dest.mkdir(exist_ok=True)
                for name in [None, *dirs, *files]:
                    src = Path(current) if name is None else Path(current) / name
                    dst = dest if name is None else dest / name
                    metadata = src.lstat()
                    if src.is_symlink():
                        os.symlink(os.readlink(src), dst)
                        if name in dirs:
                            dirs.remove(name)
                    elif src.is_dir():
                        dst.mkdir(exist_ok=True)
                    elif src.is_file():
                        shutil.copy2(src, dst)
                        count += 1
                        total += metadata.st_size
                    else:
                        raise ValueError(f'Unsupported seed file: {src}')
                    os.chown(dst, metadata.st_uid, metadata.st_gid, follow_symlinks=False)
                    if not src.is_symlink():
                        os.chmod(dst, stat.S_IMODE(metadata.st_mode))
                    actual = dst.lstat()
                    if (actual.st_uid, actual.st_gid, stat.S_IMODE(actual.st_mode)) != (
                            metadata.st_uid, metadata.st_gid, stat.S_IMODE(metadata.st_mode)):
                        raise ValueError(f'Seed ownership/mode mismatch: {src}')
            for name in ('keybags', 'mobile/tmp', 'tmp', 'run'):
                directory = target / name
                if not directory.exists():
                    directory.mkdir(parents=True)
                    parent = directory.parent.stat()
                    os.chown(directory, parent.st_uid, parent.st_gid)
            os.chmod(target / 'tmp', 0o1777)
            prepare_userspace(target)
            disabled_jobs = prepare_launch_policy(target, original / 'System/Library/LaunchDaemons')
    with image.open('rb') as stream:
        header = stream.read(4096)
    if image.stat().st_size != data_bytes or header[32:36] != b'NXSB':
        raise ValueError('Unexpected APFS seed geometry')
    report = dict(volume_uuid=identity, data_bytes=data_bytes,
                  source_regular_files=count, source_regular_bytes=total,
                  guest_mutable=True, persistent=False,
                  bringup_disabled_jobs=disabled_jobs,
                  userspace_compat_original_instructions_verified=True,
                  mobilegestalt_cache_cleared=True, setup_skipped=True, auto_dim_disabled=True,
                  ownership_preserved=True, os_inputs_modified=False, hardware_tested=False)
    if packer:
        report['packed'] = runpy.run_path(str(packer))['pack'](image, output / 'data.dbrdisk')
    (output / 'seed.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='original OS dmg')
    parser.add_argument('--systemos', type=Path, default=None,
                        help='original SystemOS dmg (optional; sealed images are skipped)')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--data-mib', type=int, default=1024)
    parser.add_argument('--packer', type=Path, help='pinned UniXNU packed_ramdisk.py')
    args = parser.parse_args()
    try:
        packer = args.packer.resolve(strict=True) if args.packer else None
        prepare(args.source.resolve(), args.systemos.resolve() if args.systemos else None,
                args.output.resolve(), args.data_mib * 1024 * 1024, packer)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'prepare-ram-data: {error}\n')
