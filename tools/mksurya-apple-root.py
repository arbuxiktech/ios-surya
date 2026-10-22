#!/usr/bin/env python3
"""Prepare an Apple launchd boot payload; never installs or boots it on a phone."""
import argparse
from pathlib import Path
import mkpayload
import surya
import adt
import surya_touch_firmware

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('kernel', type=Path)
p.add_argument('adt', type=Path)
p.add_argument('out', type=Path)
p.add_argument('--diagnostic', action='store_true', help='tee selected daemon stderr from /dev/null to console')
p.add_argument('--gui', action='store_true', help='clean GUI boot: mute console on first frame present and set sweetfb_hz=30')
p.add_argument('--touch-diagnostic', action='store_true', help='enable touch with visible console diagnostics')
p.add_argument('--cpus', choices=('1', '8'), help='select one or all eight SM7150 CPU cores (default: 8)')

p.add_argument('--touch-firmware', type=Path, help='pinned Poco Tianma controller RAM firmware')
p.add_argument('--gmu-firmware', type=Path, help='pinned Adreno 618 GMU firmware')
p.add_argument('--sqe-firmware', type=Path, help='pinned Adreno 618 SQE firmware')

p.add_argument('--gpu-table-diagnostic', action='store_true', help='pause before launchd for offline XNU GPU table allocation checks')
p.add_argument('--metal', action='store_true', help='native Metal accelerated boot with GPU addon union mount')
p.add_argument('--metal-bundle', type=Path, help='path to surya-metal-bundle.tar archive')

a = p.parse_args()
if sum((a.diagnostic, a.gui, a.touch_diagnostic, a.gpu_table_diagnostic, a.metal)) > 1:
    raise SystemExit('select only one boot profile')
if a.metal and a.cpus == '1':
    raise SystemExit('--metal requires --cpus 8')
kernel = a.kernel.read_bytes()
if b'SURYA: bounded Apple root writes armed' not in kernel:
    raise SystemExit('kernel lacks bounded SURYA Apple-root support')
m = mkpayload.parse_macho(kernel)
if m['cputype'] != 0x100000c or m['cpusub'] != 0:
    raise SystemExit('SURYA requires plain arm64')
cpu_count = int(a.cpus or '8')
base_cmdline = (surya.APPLE_METAL_CMDLINE if a.metal else
                surya.APPLE_GPU_PT_CMDLINE if a.gpu_table_diagnostic else
                surya.APPLE_TOUCH_CMDLINE if a.touch_diagnostic else
                surya.APPLE_GUI_CMDLINE if a.gui else
                surya.APPLE_DIAGNOSTIC_CMDLINE if a.diagnostic else
                surya.APPLE_CMDLINE)
cmdline = base_cmdline.replace('cpus=1', f'cpus={cpu_count}')
if a.gpu_table_diagnostic and b'SURYA GPU PAGE TABLE DIAGNOSTIC' not in kernel:
    raise SystemExit('kernel lacks SURYA GPU table diagnostic')
adt_path = a.adt
ROOT = Path(__file__).resolve().parents[1]
if (a.cpus is not None or a.metal or a.touch_firmware or a.gmu_firmware or
        a.sqe_firmware or a.gpu_table_diagnostic or a.metal_bundle):
    tree, end = adt.parse(a.adt.read_bytes())
    if end != a.adt.stat().st_size:
        raise SystemExit('unexpected trailing ADT bytes')
    chosen = surya.child(tree, 'chosen')
    surya.set_cpu_count(tree, cpu_count)
    if a.touch_firmware:
        if not (a.gui or a.touch_diagnostic or a.gpu_table_diagnostic or a.metal):
            raise SystemExit('touch firmware requires a touch-enabled boot profile')
        if b'SURYA TOUCH: loading verified Tianma image' not in kernel:
            raise SystemExit('kernel lacks the bounded Tianma SRAM loader')
        fw = surya_touch_firmware.validate(a.touch_firmware.read_bytes())
        surya.setprop(chosen, 'surya-touch-panel', b'tianma\0')
        surya.setprop(chosen, 'surya-touch-firmware', fw)
    if a.gmu_firmware:
        gmu_fw = a.gmu_firmware.read_bytes()
        if len(gmu_fw) != 32768:
            raise SystemExit(f'invalid GMU firmware size: {len(gmu_fw)}')
        surya.setprop(chosen, 'surya-gmu-firmware', gmu_fw)
    sqe_path = a.sqe_firmware or ((ROOT / 'research/firmware/a630_sqe.fw') if (a.gpu_table_diagnostic or a.metal) else None)
    if sqe_path and sqe_path.exists():
        sqe_fw = sqe_path.read_bytes()
        if len(sqe_fw) != 32136:
            raise SystemExit(f'invalid SQE firmware size: {len(sqe_fw)}')
        surya.setprop(chosen, 'surya-sqe-firmware', sqe_fw)
    if a.metal_bundle:
        bundle_tar = a.metal_bundle.read_bytes()
        surya.setprop(chosen, 'surya-metal-bundle', bundle_tar)
    adt_path = a.out.with_suffix('.adt')
    adt_path.parent.mkdir(parents=True, exist_ok=True)
    adt_path.write_bytes(adt.serialize(tree))
blob, info = mkpayload.build(str(a.kernel), str(adt_path), cmdline)
result = surya.validate_payload(blob)
if result['stage'] != 'apple-root':
    raise SystemExit('expected an explicit Apple-root ADT')
a.out.parent.mkdir(parents=True, exist_ok=True)
a.out.write_bytes(blob)
print(a.out, info['size'], 'bytes; bounded Apple-root profile')
print('Do not boot before the root filesystem is installed with owner approval.')
