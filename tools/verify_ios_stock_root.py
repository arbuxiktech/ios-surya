#!/usr/bin/env python3
"""Verify the iOS 16 root contains every app bundle found in all IPSW images."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

APP_DIRS = ('Applications', 'System/Applications',
            'System/Library/CoreServices/Applications')
DESTINATIONS = {
    'OS': Path('.'),
    'SystemOS': Path('private/preboot/Cryptexes/OS'),
    'AppOS': Path('private/preboot/Cryptexes/App'),
}


def app_bundles(image_root: Path) -> list[str]:
    found = set()
    for relative in APP_DIRS:
        base = image_root / relative
        if not base.is_dir():
            continue
        for path in base.rglob('*.app'):
            if path.is_dir():
                found.add(path.relative_to(image_root).as_posix())
    return sorted(found)


def verify(os_root: Path, systemos_root: Path, appos_root: Path,
           destination: Path) -> dict:
    sources = {'OS': os_root, 'SystemOS': systemos_root, 'AppOS': appos_root}
    expected = {}
    for label, source in sources.items():
        apps = app_bundles(source)
        if label != 'AppOS' and not apps:
            raise ValueError(f'{label} image contains no app bundles')
        prefix = DESTINATIONS[label]
        missing = [str(prefix / relative) for relative in apps
                   if not (destination / prefix / relative).is_dir()]
        if missing:
            raise ValueError(f'{label} stock app bundles missing after copy: {missing[:12]}')
        expected[label] = apps

    if not (destination / 'System/Library/xpc/launchd.plist').is_file():
        raise ValueError('OS image is missing System/Library/xpc/launchd.plist')
    if not (destination / DESTINATIONS['SystemOS'] / 'System/Library').is_dir():
        raise ValueError('SystemOS cryptex was not copied into the root image')
    if not (destination / DESTINATIONS['AppOS'] / 'System/Library/Assistant').is_dir():
        raise ValueError('AppOS cryptex was not copied into the root image')
    return {
        'product_version': '16.7.16',
        'build': '20H392',
        'apps_by_image': {label: {'count': len(apps), 'paths': apps}
                          for label, apps in expected.items()},
        'total_app_bundles': sum(map(len, expected.values())),
        'root_layout': {label: str(path) for label, path in DESTINATIONS.items()},
    }


def add_image(kind: str, image_root: Path, destination: Path,
              inventory_path: Path) -> dict:
    if kind not in DESTINATIONS:
        raise ValueError(f'unknown source image: {kind}')
    if inventory_path.exists():
        inventory = json.loads(inventory_path.read_text(encoding='utf-8'))
        if inventory.get('build') != '20H392':
            raise ValueError('existing stock app inventory belongs to another build')
    else:
        inventory = {'product_version': '16.7.16', 'build': '20H392',
                     'apps_by_image': {}, 'root_layout': {
                         label: str(path) for label, path in DESTINATIONS.items()}}
    apps_by_image = inventory.setdefault('apps_by_image', {})
    if kind in apps_by_image:
        raise ValueError(f'{kind} was already added to this root image')
    apps = app_bundles(image_root)
    if kind != 'AppOS' and not apps:
        raise ValueError(f'{kind} image contains no stock .app bundles')
    prefix = DESTINATIONS[kind]
    missing = [str(prefix / relative) for relative in apps
               if not (destination / prefix / relative).is_dir()]
    if missing:
        raise ValueError(f'{kind} stock app bundles missing after copy: {missing[:12]}')
    if kind == 'OS' and not (destination / 'System/Library/xpc/launchd.plist').is_file():
        raise ValueError('OS image is missing System/Library/xpc/launchd.plist')
    if kind == 'SystemOS' and not (destination / prefix / 'System/Library').is_dir():
        raise ValueError('SystemOS cryptex was not copied into the root image')
    if kind == 'AppOS' and not (destination / prefix / 'System/Library/Assistant').is_dir():
        raise ValueError('AppOS cryptex was not copied into the root image')
    apps_by_image[kind] = {'count': len(apps), 'paths': apps}
    inventory['total_app_bundles'] = sum(item['count'] for item in apps_by_image.values())
    inventory_path.parent.mkdir(parents=True, exist_ok=True)
    inventory_path.write_text(json.dumps(inventory, indent=2) + '\n', encoding='utf-8')
    return {'image': kind, 'app_bundles': len(apps),
            'total_app_bundles': inventory['total_app_bundles']}


def finalize(destination: Path, inventory_path: Path) -> dict:
    inventory = json.loads(inventory_path.read_text(encoding='utf-8'))
    expected_kinds = set(DESTINATIONS)
    actual_kinds = set(inventory.get('apps_by_image', {}))
    if actual_kinds != expected_kinds:
        raise ValueError(f'inventory images are incomplete: expected {sorted(expected_kinds)}, '
                         f'found {sorted(actual_kinds)}')
    for kind, entry in inventory['apps_by_image'].items():
        prefix = DESTINATIONS[kind]
        missing = [str(prefix / relative) for relative in entry['paths']
                   if not (destination / prefix / relative).is_dir()]
        if missing:
            raise ValueError(f'{kind} stock app bundles missing from final image: {missing[:12]}')
    if not (destination / 'System/Library/xpc/launchd.plist').is_file():
        raise ValueError('root image is missing launchd.plist')
    if not (destination / DESTINATIONS['SystemOS'] / 'System/Library').is_dir():
        raise ValueError('SystemOS cryptex is missing from final image')
    if not (destination / DESTINATIONS['AppOS'] / 'System/Library/Assistant').is_dir():
        raise ValueError('AppOS cryptex is missing from final image')
    inventory['total_app_bundles'] = sum(
        item['count'] for item in inventory['apps_by_image'].values())
    return inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', type=Path)
    parser.add_argument('--systemos', type=Path)
    parser.add_argument('--appos', type=Path)
    parser.add_argument('--kind', choices=tuple(DESTINATIONS))
    parser.add_argument('--image', type=Path)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--output', type=Path,
                        help='write a complete inventory after checking all three sources')
    parser.add_argument('--inventory', type=Path,
                        help='incremental inventory used while adding one image at a time')
    parser.add_argument('--finalize', action='store_true',
                        help='verify all incrementally inventoried bundles in the final root')
    args = parser.parse_args()
    if args.finalize:
        if not args.inventory:
            parser.error('--finalize requires --inventory')
        result = finalize(args.root, args.inventory)
    elif args.kind and args.image and args.inventory:
        result = add_image(args.kind, args.image, args.root, args.inventory)
    elif args.os and args.systemos and args.appos and args.output:
        result = verify(args.os, args.systemos, args.appos, args.root)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    else:
        parser.error('provide --finalize, --kind/--image/--inventory, or all three source roots and --output')
    print(json.dumps({k: v for k, v in result.items() if k != 'apps_by_image'}, indent=2))


if __name__ == '__main__':
    main()
