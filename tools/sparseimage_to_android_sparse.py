#!/usr/bin/env python3
"""Convert a macOS sparse disk image to Android sparse format via hdiutil."""
from __future__ import annotations

import argparse
import plistlib
from pathlib import Path
import re
import subprocess
import sys

import raw_to_android_sparse


def run(args: list[str]) -> bytes:
    result = subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result.stdout


def convert(image: Path, output: Path):
    attached = plistlib.loads(run([
        'hdiutil', 'attach', '-plist', '-nomount', '-readonly', '-noverify', str(image)
    ]))
    devices = [entry['dev-entry'] for entry in attached.get('system-entities', [])
               if re.fullmatch(r'/dev/disk\d+', entry.get('dev-entry', ''))]
    if len(devices) != 1:
        raise RuntimeError(f'expected one raw HFS+ disk device, got {devices}')
    device = devices[0]
    try:
        details = plistlib.loads(run(['diskutil', 'info', '-plist', device]))
        size = int(details['TotalSize'])
        raw_device = details.get('RawDeviceNode') or device.replace('/dev/disk', '/dev/rdisk', 1)
        return raw_to_android_sparse.encode(Path(raw_device), output, size)
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, help='unpartitioned HFS+ .sparseimage')
    parser.add_argument('output', type=Path, help='Android sparse .simg output')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('this wrapper needs macOS hdiutil; use raw_to_android_sparse.py for a raw file')
    import json
    print(json.dumps(convert(args.image, args.output), sort_keys=True))


if __name__ == '__main__':
    main()
