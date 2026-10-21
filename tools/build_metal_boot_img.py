#!/usr/bin/env python3
"""Build boot-surya-metal-full.img for Poco X3 NFC with native Metal GPU acceleration."""
import hashlib
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / 'xnu/out-surya-apple-root/kernel.development.surya'
BASE_ADT = ROOT / 'shim/out/surya/apple-root.adt'
PAYLOAD_BIN = ROOT / 'research/apple-root-ready/unixnu-metal-payload.bin'
TOUCH_FW = ROOT / 'research/firmware/tianma_firmware.bin'
GMU_FW = ROOT / 'research/firmware/a618_gmu.bin'
SQE_FW = ROOT / 'research/firmware/a630_sqe.fw'
BUNDLE_TAR = ROOT / 'out/metal/surya-metal-bundle.tar'
SHIM_IMAGE = ROOT / 'shim/out/surya/Image'
DTB = ROOT / 'research/stock-surya/boot-unpacked/dtb'
MKBOOTIMG = Path('C:/Users/Arbuxik/Documents/ChatGPT/fdsfsdfdsfsdfffff/upstream/edk2-msm/tools/mkbootimg.py')
OUT_IMG = ROOT / 'research/apple-root-ready/boot-surya-metal-full.img'


def main():
    if not KERNEL.exists():
        sys.exit(f'Missing kernel: {KERNEL}')
    if not BUNDLE_TAR.exists():
        print('Building bundle tar...')
        subprocess.run([sys.executable, str(ROOT / 'tools/pack_metal_addon.py')], check=True)

    print('Step 1: Assembling Metal boot payload with ADT and firmware...')
    cmd_payload = [
        sys.executable, str(ROOT / 'tools/mksurya-apple-root.py'),
        str(KERNEL), str(BASE_ADT), str(PAYLOAD_BIN),
        '--touch-firmware', str(TOUCH_FW),
        '--gmu-firmware', str(GMU_FW),
        '--sqe-firmware', str(SQE_FW),
        '--metal-bundle', str(BUNDLE_TAR),
        '--metal'
    ]
    subprocess.run(cmd_payload, check=True)

    payload_bytes = PAYLOAD_BIN.read_bytes()
    payload_sha = hashlib.sha256(payload_bytes).hexdigest()
    print(f'Payload built: {len(payload_bytes)} bytes ({len(payload_bytes)/1024/1024:.2f} MB), SHA256: {payload_sha}')

    print('Step 2: Packaging Android Boot Image v2 (boot-surya-metal-full.img)...')
    cmd_bootimg = [
        sys.executable, str(MKBOOTIMG),
        '--header_version', '2',
        '--kernel', str(SHIM_IMAGE),
        '--ramdisk', str(PAYLOAD_BIN),
        '--dtb', str(DTB),
        '--base', '0',
        '--kernel_offset', '0x00008000',
        '--ramdisk_offset', '0x01000000',
        '--tags_offset', '0x00000100',
        '--pagesize', '4096',
        '-o', str(OUT_IMG)
    ]
    subprocess.run(cmd_bootimg, check=True)

    img_bytes = OUT_IMG.read_bytes()
    img_sha = hashlib.sha256(img_bytes).hexdigest()
    print(f'SUCCESS! Boot image: {OUT_IMG}')
    print(f'Size: {len(img_bytes)} bytes ({len(img_bytes)/1024/1024:.2f} MB)')
    print(f'SHA256: {img_sha}')
    print('\nTo boot:')
    print(f'  fastboot boot {OUT_IMG}')


if __name__ == '__main__':
    main()
