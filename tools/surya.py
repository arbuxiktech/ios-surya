#!/usr/bin/env python3
"""SURYA bring-up ADT. Memory must be supplied from this phone, not sweet.

Keeps upstream's internal sweet-* driver ABI; selects the SURYA kernel target.
This produces an ADT, not a complete iOS installation or boot image.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import adt
import mkadt

CMDLINE = ('debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam '
           '-noprogress rd=md0 sweettouch=0 sweetkeys=0 sweetusb=0 '
           'sweetbattery=0 sweetlog=0 ufsrw=0 rootrw=0')
APPLE_CMDLINE = ('debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam '
                 '-noprogress rd=ufsroot0 ufsroot=userdata unentitled_ios_sim_launch=1 '
                 'ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 '
                 'launchd_unsecure_cache=1 wdt=-1 sweettouch=0 sweetkeys=0 '
                 'sweetusb=0 sweetbattery=0 sweetlog=0')
APPLE_DIAGNOSTIC_CMDLINE = (APPLE_CMDLINE +
    ' sweetnull=launchd,xpcproxy,bluetoothd,fairplayd.H2,triald,backboardd,SpringBoard')
APPLE_GUI_CMDLINE = ('debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress '
                     'rd=ufsroot0 ufsroot=userdata unentitled_ios_sim_launch=1 '
                     'ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 '
                     'launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 '
                     'sweettouch=1 sweetkeys=0 sweetusb=0 sweetbattery=0 sweetlog=0')
# Visible touch bring-up without daemon stderr flooding or console muting.
APPLE_TOUCH_CMDLINE = (APPLE_GUI_CMDLINE.replace('sweetfb_noconsole=1', 'sweetfb_noconsole=0') +
                       ' suryatouchreport=1')
APPLE_GPU_PT_CMDLINE = (APPLE_GUI_CMDLINE.replace('sweetfb_noconsole=1', 'sweetfb_noconsole=0') +
                        ' suryagpummu=1')
APPLE_METAL_CMDLINE = (APPLE_GUI_CMDLINE.replace('cpus=1', 'cpus=8').replace('sweetfb_hz=30', 'sweetfb_hz=60') +
                       ' -no_slto_panic sheng-gpu-addon=1')
UPSTREAM = '11da58bd050672960c0a83b5bef2176047eea479'


def setprop(node, key, value):
    node.props = [(k, f, v) for k, f, v in node.props if k != key]
    node.props.append((key, 0, value))


def child(node, name):
    return next(n for n in node.children
                if dict((k, v) for k, f, v in n.props)['name'] == name.encode() + b'\0')


def validate_memory(base, size):
    # XNU's bootstrap uses 32 MiB block mappings. Keep the whole region aligned.
    if base < 0x80000000 or base % 0x2000000 or size < 0x8000000 or size % 0x2000000:
        raise ValueError('RAM base/size must be 32 MiB aligned; size >= 128 MiB')
    if base + size > 1 << 40:
        raise ValueError('RAM exceeds the measured 40-bit physical address range')
    if base < 0x9ca00000 and base + size > 0x9c000000:
        raise ValueError('RAM overlaps the boot framebuffer')


def build(base, size, *, seed=None, qemu=False, product=None, storage=None, apple_root=False):
    validate_memory(base, size)
    if apple_root and (not storage or not product or size < 0x80000000):
        raise ValueError('Apple root requires storage proof, product ADT and >=2 GiB RAM')
    root = mkadt.build(seed=seed, qemu=qemu, product_path=None)
    setprop(root, 'compatible', mkadt.cstr('D22AP') + mkadt.cstr('iPhone10,3') +
            mkadt.cstr('xiaomi,surya') + mkadt.cstr('qcom,sm7150') + mkadt.cstr('AppleARM'))
    setprop(root, 'surya-port-revision', mkadt.cstr('ramdisk-1'))
    chosen = child(root, 'chosen')
    setprop(chosen, 'dram-base', mkadt.u64(base))
    setprop(chosen, 'dram-size', mkadt.u64(size))
    setprop(chosen, 'firmware-version', mkadt.cstr('surya-shim'))
    setprop(chosen, 'system-firmware-version', mkadt.cstr('surya-shim'))
    setprop(chosen, 'unique-chip-id', mkadt.u64(0x5355525941000001))
    if qemu:
        child(root, 'cpus').children = child(root, 'cpus').children[:1]
    root.children = [n for n in root.children
                     if dict((k, v) for k, f, v in n.props)['name'] != b'sweet-ufs\0']
    if storage is not None:
        c = storage['controller']
        if (qemu or storage.get('board') != 'surya' or storage.get('soc_id') != 365
                or c.get('base') != 0x1d84000 or c.get('size') != 0x3000
                or c.get('compatible') != 'qcom,ufshc' or c.get('status') not in ('ok', 'okay')
                or storage.get('disk_writes_allowed') is not False):
            raise ValueError('storage probe requires measured SURYA controller and writes disabled')
        setprop(chosen, 'surya-ufs-probe', mkadt.u32(1))
        root.children.append(mkadt.node('sweet-ufs', [
            ('reg', mkadt.u64(c['base'], c['size'])),
            ('surya-read-only', mkadt.u32(1)),
        ]))
    if apple_root:
        if storage.get('native_read_verified') is not True:
            raise ValueError('Apple root requires a successful native storage probe')
        layout = storage['root_layout']
        first, count, blocks = (layout[k] for k in ('first_block', 'block_count', 'image_blocks'))
        if not (0 < first <= 0xffffffff and 2 <= blocks <= count
                and first + blocks - 1 <= 0xffffffff):
            raise ValueError('invalid measured root volume bounds')
        setprop(chosen, 'surya-apple-root', mkadt.u32(1))
        ufs = child(root, 'sweet-ufs')
        setprop(ufs, 'surya-read-only', mkadt.u32(0))
        setprop(ufs, 'surya-root-contract', mkadt.u64(first, count, blocks))
    if product:
        props = mkadt.d22ap_product(str(product))
        root.children = [n for n in root.children
                         if dict((k, v) for k, f, v in n.props)['name'] != b'product\0']
        root.children.append(mkadt.node('product', props))
    return root


def validate_payload(blob):
    """Validate the isolated RAM-disk or explicit bounded Apple-root boot profile."""
    import mkpayload
    if len(blob) < mkpayload.SEG_TABLE or blob[:8] != b'SWEETXNU':
        raise ValueError('missing XNU payload header')
    version, count = struct.unpack_from('<II', blob, 8)
    virt, entry, off, length, end, base, size = struct.unpack_from('<7Q', blob, 0x10)
    if version != 1 or not 1 <= count <= 128 or mkpayload.SEG_TABLE + count * 32 > len(blob):
        raise ValueError('bad segment table')
    if off % 8 or off + length > len(blob) or length == 0:
        raise ValueError('bad ADT bounds')
    root, used = adt.parse(blob[off:off + length])
    props = dict((k, v) for k, f, v in root.props)
    if b'xiaomi,surya' not in props.get('compatible', b'').split(b'\0'):
        raise ValueError('payload is not for surya')
    ufs_nodes = [n for n in root.children if n.name == 'sweet-ufs']
    chosen_props = dict((k, v) for k, f, v in child(root, 'chosen').props)
    apple_root = chosen_props.get('surya-apple-root') == mkadt.u32(1)
    probe = chosen_props.get('surya-ufs-probe')
    if probe is not None or ufs_nodes:
        if probe != mkadt.u32(1) or len(ufs_nodes) != 1:
            raise ValueError('incomplete read-only storage probe contract')
        ufs_props = dict((k, v) for k, f, v in ufs_nodes[0].props)
        if (ufs_props.get('reg') != mkadt.u64(0x1d84000, 0x3000)
                or ufs_props.get('surya-read-only') != mkadt.u32(0 if apple_root else 1)):
            raise ValueError('invalid SURYA storage probe mapping')
        contract = ufs_props.get('surya-root-contract')
        if apple_root:
            if not contract or len(contract) != 24 or size < 0x80000000:
                raise ValueError('Apple root is missing bounds or sufficient RAM')
            first, volume_count, blocks = struct.unpack('<3Q', contract)
            if not (0 < first <= 0xffffffff and 2 <= blocks <= volume_count
                    and first + blocks - 1 <= 0xffffffff):
                raise ValueError('Apple root bounds invalid')
            if not any(n.name == 'product' for n in root.children):
                raise ValueError('Apple product identity missing')
        elif contract:
            raise ValueError('RAM-disk probe cannot arm root writes')
    if apple_root and not ufs_nodes:
        raise ValueError('Apple root has no storage controller')
    if mkpayload.adt_dram(blob[off:off + length]) != (base, size):
        raise ValueError('RAM contract mismatch')
    # Validate the same contract used by ADT generation, including address limits.
    validate_memory(base, size)
    cmd = blob[0x48:mkpayload.SEG_TABLE].split(b'\0', 1)[0].decode('ascii')
    allowed_cmdlines = (APPLE_CMDLINE, APPLE_DIAGNOSTIC_CMDLINE, APPLE_GUI_CMDLINE, APPLE_TOUCH_CMDLINE, APPLE_GPU_PT_CMDLINE, APPLE_METAL_CMDLINE) if apple_root else (CMDLINE,)
    if cmd not in allowed_cmdlines:
        raise ValueError('command line does not match the SURYA boot profile')
    touch_chosen = child(root, 'chosen')
    firmware = touch_chosen.prop('surya-touch-firmware')
    if firmware is not None:
        import surya_touch_firmware
        if not apple_root or 'sweettouch=1' not in cmd or touch_chosen.prop('surya-touch-panel') != b'tianma\0':
            raise ValueError('touch firmware requires the explicit Tianma root profile')
        surya_touch_firmware.validate(firmware)
    ranges = []
    table_end = mkpayload.SEG_TABLE + count * 32
    for i in range(count):
        va, vm_size, file_off, file_size = struct.unpack_from('<4Q', blob, mkpayload.SEG_TABLE + i * 32)
        pa = va - virt + base
        if not vm_size or file_size > vm_size or file_off < table_end or file_off % 8 or file_off + file_size > off:
            raise ValueError('invalid segment file bounds')
        if va < virt or pa < base or pa + vm_size > base + size or va + vm_size > end:
            raise ValueError('segment exceeds RAM/kernel bounds')
        if any(pa < hi and pa + vm_size > lo for lo, hi in ranges):
            raise ValueError('overlapping segments')
        ranges.append((pa, pa + vm_size))
    if not any(lo <= entry - virt + base < hi for lo, hi in ranges):
        raise ValueError('entry is outside loaded segments')
    top = (end - virt + base + length + 0x3fff) & ~0x3fff
    if top + 0x4000 > base + size:
        raise ValueError('ADT/boot arguments exceed RAM')
    memory_map = child(child(root, 'chosen'), 'memory-map')
    rd = dict((k, v) for k, f, v in memory_map.props).get('RAMDisk')
    if apple_root:
        if rd:
            raise ValueError('Apple root profile cannot substitute a test RAM disk')
        return dict(board='surya', stage='apple-root', base=hex(base), size=hex(size), cmdline=cmd)
    if not rd or len(rd) != 16:
        raise ValueError('RAM-disk root is required')
    rd_pa, rd_size = struct.unpack('<QQ', rd)
    if not any(lo <= rd_pa and rd_pa + rd_size <= hi for lo, hi in ranges):
        raise ValueError('RAM disk is outside loaded segments')
    return dict(board='surya', stage='ramdisk', base=hex(base), size=hex(size), cmdline=cmd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--dram-base', type=lambda x: int(x, 0), required=True)
    p.add_argument('--dram-size', type=lambda x: int(x, 0), required=True)
    p.add_argument('--memory-evidence', type=Path, required=True, help='local DT/map source; recorded, never inferred from sweet')
    p.add_argument('--product-adt', type=Path)
    p.add_argument('--apple-root', action='store_true', help='prepare bounded writable Apple root profile; do not boot before approved installation')
    p.add_argument('--qemu', action='store_true')
    p.add_argument('--storage-evidence', type=Path, help='live Poco controller evidence; read-only probe only')
    a = p.parse_args()
    if not a.memory_evidence.is_file():
        p.error('--memory-evidence must name an existing local source file')
    storage = json.loads(a.storage_evidence.read_text(encoding='utf-8')) if a.storage_evidence else None
    root = build(a.dram_base, a.dram_size, qemu=a.qemu, product=a.product_adt, storage=storage, apple_root=a.apple_root)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(adt.serialize(root))
    a.out.with_suffix('.json').write_text(json.dumps(dict(board='surya', upstream=UPSTREAM,
        memory_evidence=str(a.memory_evidence.resolve()),
        memory_evidence_sha256=hashlib.sha256(a.memory_evidence.read_bytes()).hexdigest(),
        memory_verified_on_device=False,
        kernel_target='SURYA', qemu=a.qemu, command_line=APPLE_CMDLINE if a.apple_root else CMDLINE,
        apple_root=a.apple_root,
        storage_probe=bool(storage),
        storage_evidence_sha256=hashlib.sha256(a.storage_evidence.read_bytes()).hexdigest() if storage else None,
        status='generated, untested on hardware'), indent=2) + '\n')
    print(a.out)


if __name__ == '__main__':
    main()
