#!/usr/bin/env python3
"""Select the backing whole-disk device from hdiutil attach -plist output."""
from __future__ import annotations

import plistlib
import re
import sys
from pathlib import Path


def backing_disk(plist: dict) -> str:
    """Return the attached image device, ignoring synthesized APFS disks.

    APFS images expose both the hdiutil backing disk and one or more
    synthesized container disks in ``system-entities``. hdiutil lists the
    backing device first; where available, a partition-scheme content hint
    identifies it explicitly.
    """
    entities = plist.get('system-entities', [])
    devices = []
    for entity in entities:
        device = entity.get('dev-entry', '')
        if re.fullmatch(r'/dev/disk\d+', device) and device not in devices:
            devices.append(device)
    if not devices:
        raise ValueError('hdiutil plist contains no whole-disk device')

    partition_schemes = {
        entity.get('dev-entry')
        for entity in entities
        if entity.get('content-hint') in {
            'GUID_partition_scheme', 'Apple_partition_scheme'
        }
        and entity.get('dev-entry') in devices
    }
    if len(partition_schemes) == 1:
        return next(iter(partition_schemes))
    return devices[0]


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f'usage: {Path(sys.argv[0]).name} ATTACH-PLIST')
    with open(sys.argv[1], 'rb') as stream:
        print(backing_disk(plistlib.load(stream)))


if __name__ == '__main__':
    main()
