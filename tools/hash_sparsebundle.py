#!/usr/bin/env python3
"""Hash every file in a macOS sparsebundle in stable relative-path order."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def digest_bundle(bundle: Path) -> str:
    if not bundle.is_dir() or not (bundle / 'Info.plist').is_file():
        raise ValueError(f'not a macOS sparsebundle: {bundle}')
    digest = hashlib.sha256()
    for path in sorted(item for item in bundle.rglob('*') if item.is_file()):
        relative = path.relative_to(bundle).as_posix().encode('utf-8')
        digest.update(len(relative).to_bytes(4, 'big'))
        digest.update(relative)
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
                digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    value = digest_bundle(args.bundle)
    args.output.write_text(f'{value}  {args.bundle.name}/\n', encoding='ascii')
    print(value)


if __name__ == '__main__':
    main()
