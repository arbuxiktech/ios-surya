#!/usr/bin/env python3
"""Pack the compiled NativeAdrenoMetal bundle, daemon, plists, and display preferences into an uncompressed tar archive.

The resulting archive is embedded as the ADT property 'surya-metal-bundle'.
On boot with 'sheng-gpu-addon=1', the SURYA kernel extracts this archive into /private/var/
and performs read-only union mounts over /System/Library/Extensions and /System/Library/LaunchDaemons.
"""
import io
from pathlib import Path
import plistlib
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
EXTRACTED = ROOT / 'out/metal/extracted'
OUT_TAR = ROOT / 'out/metal/surya-metal-bundle.tar'


def add_data(tar, arcname, data, mode=0o644):
    ti = tarfile.TarInfo(name=arcname)
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 1700000000
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


def main():
    if not (EXTRACTED / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal').exists():
        sys.exit(f'Missing {EXTRACTED}/NativeAdrenoMetal.bundle/NativeAdrenoMetal')

    OUT_TAR.parent.mkdir(parents=True, exist_ok=True)

    pref_dict = {
        'CAEnableDeepFramebuffer': False,
        'CAEnableFramebufferCompression': False,
        'CAEnableIOSurfaceCompression': False,
        'CAMaximumFrameRate': 120,
        'CAFrameRateRangeMaximum': 120,
        'CADisableProMotionLimiting': True,
        'AppleMaxFrameRate': 120,
    }
    pref_bytes = plistlib.dumps(pref_dict, fmt=plistlib.FMT_XML)

    count = 0
    with tarfile.open(OUT_TAR, mode='w') as tar:
        # 1. NativeAdrenoMetal.bundle
        for f in sorted((EXTRACTED / 'NativeAdrenoMetal.bundle').rglob('*')):
            if any(part.endswith('.dSYM') for part in f.parts):
                continue
            rel = f.relative_to(EXTRACTED).as_posix()
            arcname = 'ngpu/addons/' + rel
            if f.is_dir():
                ti = tarfile.TarInfo(name=arcname)
                ti.mode = 0o755
                ti.type = tarfile.DIRTYPE
                ti.mtime = 1700000000
                tar.addfile(ti)
            else:
                mode = 0o755 if f.name in ('NativeAdrenoMetal', 'libvulkan_freedreno.dylib') else 0o644
                add_data(tar, arcname, f.read_bytes(), mode=mode)
            count += 1

        # 2. Markers
        add_data(tar, 'ngpu/addons/NativeAdrenoMetal.publish', b'1\n')
        add_data(tar, 'ngpu/addons/NativeAdrenoMetal.clients', b'*\n')
        count += 2

        # 3. Binaries and daemons
        add_data(tar, 'ngpu/bin/adrenoprobed', (EXTRACTED / 'adrenoprobed').read_bytes(), mode=0o755)
        add_data(tar, 'ngpu/daemons/org.blu-sharky.adrenoprobed.plist', (EXTRACTED / 'org.blu-sharky.adrenoprobed.plist').read_bytes())
        add_data(tar, 'ngpu/daemons/com.apple.iomfb_fdr_loader.plist', (EXTRACTED / 'com.apple.iomfb_fdr_loader.plist').read_bytes())
        add_data(tar, 'ngpu/daemons/com.apple.bluetoothd.plist', (EXTRACTED / 'com.apple.bluetoothd.plist').read_bytes())
        add_data(tar, 'ngpu/trust.json', (EXTRACTED / 'trust.json').read_bytes())
        count += 5

        # 4. Display preferences (120 Hz ProMotion)
        for d in ['mobile', 'root']:
            add_data(tar, f'{d}/Library/Preferences/backboardd.plist', pref_bytes)
            add_data(tar, f'{d}/Library/Preferences/.GlobalPreferences.plist', pref_bytes)
            add_data(tar, f'{d}/Library/Preferences/com.apple.CoreAnimation.plist', pref_bytes)
            add_data(tar, f'{d}/Library/Preferences/com.apple.backboardd.plist', pref_bytes)
            add_data(tar, f'{d}/Library/Preferences/com.apple.springboard.plist', pref_bytes)
            count += 5

    tar_size = OUT_TAR.stat().st_size
    print(f'Wrote {OUT_TAR} ({count} entries, {tar_size} bytes / {tar_size/1024/1024:.2f} MB)')


if __name__ == '__main__':
    main()
