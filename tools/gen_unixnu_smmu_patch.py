#!/usr/bin/env python3
"""Embed pinned UniXNU table/context code and SURYA integration into XNU."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def new_file(name, data):
    if b'\r' in data or not data.endswith(b'\n'):
        raise ValueError('kernel source must use LF with final newline')
    digest = hashlib.sha1(f'blob {len(data)}\0'.encode() + data).hexdigest()
    return (f'diff --git a/bsd/dev/{name} b/bsd/dev/{name}\n'
            f'new file mode 100644\nindex 0000000..{digest[:7]}\n'
            f'--- /dev/null\n+++ b/bsd/dev/{name}\n'
            f'@@ -0,0 +1,{data.count(bytes([10]))} @@\n' +
            ''.join('+' + line for line in data.decode().splitlines(keepends=True)))


def generate():
    vendor = ROOT / 'graphics/vendor/unixnu/ArmSmmu4K'
    files = [(p.name, p.read_bytes()) for p in (
        vendor / 'ArmSmmu4K.h', vendor / 'ArmSmmu4K.c',
        vendor / 'ArmSmmu4KContext.h', vendor / 'ArmSmmu4KContext.c',
        vendor / 'ArmSmmu4KProgram.h', vendor / 'ArmSmmu4KProgram.c',
        vendor / 'ArmSmmu4KStreams.h', vendor / 'ArmSmmu4KStreams.c',
        ROOT / 'graphics/native/SuryaNativeAdrenoABI.h',
        ROOT / 'graphics/native/surya_unixnu_smmu.h',
        ROOT / 'graphics/native/surya_unixnu_smmu.c')]
    # Preserve the upstream MIT notice inside the source patch as well.
    files.append(('ArmSmmu4K.LICENSE', (vendor / 'LICENSE').read_bytes()))
    patch = ''.join(new_file(name, data) for name, data in files)
    patch += '''diff --git a/bsd/conf/files b/bsd/conf/files
--- a/bsd/conf/files
+++ b/bsd/conf/files
@@ -109,6 +109,11 @@
 bsd/dev/sweet_keys.c\t\t\tstandard
 bsd/dev/sweet_touch.c\t\t\tstandard
 bsd/dev/surya_gpu_pt.c\t\t\tstandard
+bsd/dev/ArmSmmu4K.c\t\t\tstandard
+bsd/dev/ArmSmmu4KContext.c\t\t\tstandard
+bsd/dev/ArmSmmu4KProgram.c\t\t\tstandard
+bsd/dev/ArmSmmu4KStreams.c\t\t\tstandard
+bsd/dev/surya_unixnu_smmu.c\t\tstandard
 bsd/dev/sweet_rpmh.c\t\t\tstandard
 bsd/dev/sweet_spmi.c\t\t\tstandard
 bsd/dev/sweet_usb.c\t\t\tstandard
'''
    return patch


if __name__ == '__main__':
    target = ROOT / 'xnu/patches/0093-surya-unixnu-smmu-foundation.patch'
    target.write_bytes(generate().encode())
    print(target.name)
