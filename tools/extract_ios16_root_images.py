#!/usr/bin/env python3
"""Extract and manifest-verify the iOS 16.7.16 root and cryptex images.

The accepted source is Apple's iPhone10,3/iPhone10,6 IPSW for build 20H392.
The restore manifest is checked for the D22AP erase identity before the three
root images are streamed out and verified against their SHA-384 digests.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import zipfile

BUILD = '20H392'
VERSION = '16.7.16'
BOARD = 'd22ap'
IMAGES = {
    'OS': ('087-86698-044.dmg', '103b835176572a97ae36bc5ebe4c332c40b921e1f151b844116bbda49959c46c64364d343ceb0c4e9d95a51ba60d0e51'),
    'Cryptex1,SystemOS': ('087-86593-047.dmg', '775e110985ba8fd0dd2ad21d3fd5a896eec8887af219fc0dbc1e55ace791de1d51ecca2477f2e650b24c557cb74a0627'),
    'Cryptex1,AppOS': ('087-86715-047.dmg', '853c4f945ea09b1614bedeaffc777aefdf2c5bcb15f1a66986e965207757246c2d3f8712a5a5c491cfc49a5a45e81502'),
}


def select_entries(manifest: dict) -> dict[str, tuple[str, str]]:
    if (manifest.get('ProductVersion') != VERSION or
            manifest.get('ProductBuildVersion') != BUILD):
        raise ValueError('IPSW is not iOS 16.7.16 build 20H392')
    matches = [identity for identity in manifest.get('BuildIdentities', [])
               if identity.get('Info', {}).get('DeviceClass', '').lower() == BOARD and
               identity.get('Info', {}).get('RestoreBehavior') == 'Erase']
    if not matches:
        raise ValueError('IPSW has no D22AP erase restore identity')
    selected = matches[0].get('Manifest', {})
    entries = {}
    for name, (expected_path, expected_digest) in IMAGES.items():
        item = selected.get(name)
        if not item or item.get('Info', {}).get('Path') != expected_path:
            raise ValueError(f'{name} path does not match the pinned 20H392 manifest')
        digest = item.get('Digest')
        if not isinstance(digest, bytes) or digest.hex() != expected_digest:
            raise ValueError(f'{name} SHA-384 does not match the pinned 20H392 manifest')
        entries[name] = (expected_path, expected_digest)
    return entries


def selected_images(only: str | None) -> dict[str, tuple[str, str]]:
    if only is None:
        return IMAGES
    aliases = {'OS': 'OS', 'SystemOS': 'Cryptex1,SystemOS',
               'AppOS': 'Cryptex1,AppOS'}
    return {aliases[only]: IMAGES[aliases[only]]}


def extract(ipsw: Path, outdir: Path, only: str | None = None) -> dict:
    outdir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ipsw) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        entries = select_entries(manifest)
        entries = {key: entries[key] for key in selected_images(only)}
        result = {}
        for name, (path, expected) in entries.items():
            output = outdir / f'{name.split(",")[-1]}.dmg' if name.startswith('Cryptex1,') else outdir / 'OS.dmg'
            digest = hashlib.sha384()
            written = 0
            with archive.open(path, 'r') as src, output.open('wb') as dst:
                while True:
                    block = src.read(4 * 1024 * 1024)
                    if not block:
                        break
                    dst.write(block)
                    digest.update(block)
                    written += len(block)
            actual = digest.hexdigest()
            if actual != expected:
                output.unlink(missing_ok=True)
                raise ValueError(f'{name} digest mismatch: {actual}')
            result[name] = {'path': str(output), 'bytes': written, 'sha384': actual}
            print(f'verified {name}: {written} bytes SHA384={actual}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ipsw', type=Path)
    parser.add_argument('outdir', type=Path)
    parser.add_argument('--only', choices=('OS', 'SystemOS', 'AppOS'),
                        help='extract one verified image, keeping peak disk use low')
    parser.add_argument('--inspect-only', action='store_true',
                        help='validate the restore manifest without extracting multi-gigabyte images')
    args = parser.parse_args()
    if args.inspect_only:
        with zipfile.ZipFile(args.ipsw) as archive:
            entries = select_entries(plistlib.loads(archive.read('BuildManifest.plist')))
        print(json.dumps({k: {'path': v[0], 'sha384': v[1]} for k, v in entries.items()}, indent=2))
        return
    print(json.dumps(extract(args.ipsw, args.outdir, args.only), indent=2))


if __name__ == '__main__':
    main()
