"""Host checks for board separation and payload rejection before RAM writes."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import adt
import mkadt
import mkpayload
import surya


def props(n):
    return dict((k, v) for k, f, v in n.props)


def payload(apple=False, cmdline=None, cpu_count=None):
    base, virt = 0x100000000, 0xfffffe0007000000
    tree = surya.build(base, 0xfc000000 if apple else 0x20000000, seed=b'0' * 64)
    if cpu_count is not None:
        surya.set_cpu_count(tree, cpu_count)
    if apple:
        surya.setprop(surya.child(tree, 'chosen'), 'surya-apple-root', mkadt.u32(1))
        surya.setprop(surya.child(tree, 'chosen'), 'surya-ufs-probe', mkadt.u32(1))
        tree.children.append(mkadt.node('sweet-ufs', [
            ('reg', mkadt.u64(0x1d84000, 0x3000)), ('surya-read-only', mkadt.u32(0)),
            ('surya-root-contract', mkadt.u64(3688984, 26430947, 4194304))]))
        tree.children.append(mkadt.node('product', []))
    tree_blob = adt.serialize(tree) if apple else mkpayload.add_ramdisk_to_adt(adt.serialize(tree), base + 0x4000, 0x4000)
    off = mkpayload.SEG_TABLE + 64
    dt_off = off + 0x8000
    b = bytearray(dt_off)
    struct.pack_into('<8sII7Q', b, 0, b'SWEETXNU', 1, 2, virt,
                     virt + 0x100, dt_off, len(tree_blob), virt + 0x8000,
                     base, 0xfc000000 if apple else 0x20000000)
    cmd = (cmdline or (surya.APPLE_CMDLINE if apple else surya.CMDLINE)).encode()
    b[0x48:0x48 + len(cmd)] = cmd
    struct.pack_into('<4Q', b, mkpayload.SEG_TABLE, virt, 0x4000, off, 0x4000)
    struct.pack_into('<4Q', b, mkpayload.SEG_TABLE + 32, virt + 0x4000, 0x4000, off + 0x4000, 0x4000)
    b += tree_blob
    return b


class SuryaTests(unittest.TestCase):
    def test_board_identity_and_ramdisk_only(self):
        tree = surya.build(0x100000000, 0x20000000, seed=b'0' * 64)
        compat = props(tree)['compatible'].split(b'\0')
        self.assertIn(b'xiaomi,surya', compat)
        self.assertNotIn(b'xiaomi,sweet', compat)
        self.assertEqual(len(surya.child(tree, 'cpus').children), 8)
        self.assertNotIn(b'sweet-ufs\0', [props(n)['name'] for n in tree.children])
        self.assertEqual(mkpayload.adt_dram(adt.serialize(tree)), (0x100000000, 0x20000000))

    def test_apple_smp_profile_requires_eight_sm7150_cores(self):
        good = payload(apple=True, cmdline=surya.APPLE_SMP_CMDLINE, cpu_count=8)
        self.assertEqual(surya.validate_payload(good)['stage'], 'apple-root')
        bad = payload(apple=True, cmdline=surya.APPLE_SMP_CMDLINE, cpu_count=1)
        with self.assertRaisesRegex(ValueError, 'complete SM7150'):
            surya.validate_payload(bad)

    def test_all_default_apple_profiles_select_all_eight_cores(self):
        profiles = (surya.APPLE_CMDLINE, surya.APPLE_DIAGNOSTIC_CMDLINE,
                    surya.APPLE_GUI_CMDLINE, surya.APPLE_TOUCH_CMDLINE,
                    surya.APPLE_GPU_PT_CMDLINE, surya.APPLE_METAL_CMDLINE)
        for command_line in profiles:
            with self.subTest(command_line=command_line):
                self.assertIn('cpus=8', command_line.split())
                self.assertEqual(surya.validate_payload(payload(
                    apple=True, cmdline=command_line))['stage'], 'apple-root')

    def test_cpu_topology_rebuild_preserves_boot_node_and_unique_phandles(self):
        tree = surya.build(0x100000000, 0xfc000000, seed=b'0' * 64)
        cpus = surya.child(tree, 'cpus')
        boot_cpu = cpus.children[0]
        surya.set_cpu_count(tree, 1)
        self.assertIs(cpus.children[0], boot_cpu)
        surya.set_cpu_count(tree, 8)
        self.assertIs(cpus.children[0], boot_cpu)
        self.assertEqual(len(cpus.children), 8)
        all_phandles = [v for node in surya._walk(tree) for k, f, v in node.props
                        if k in ('AAPL,phandle', 'phandle') and len(v) == 4]
        self.assertEqual(len(all_phandles), len(set(all_phandles)))

    def test_qemu_uses_virtual_interrupt_map(self):
        tree = surya.build(0xb0000000, 0x20000000, qemu=True, seed=b'0' * 64)
        gic = surya.child(surya.child(tree, 'arm-io'), 'gic')
        self.assertEqual(props(gic)['virtual-timer-intid'], mkadt.u32(27))

    def test_storage_probe_requires_measured_controller_and_no_writes(self):
        evidence = dict(board='surya', soc_id=365, disk_writes_allowed=False,
                        controller=dict(base=0x1d84000, size=0x3000,
                                        compatible='qcom,ufshc', status='ok'))
        tree = surya.build(0x100000000, 0x20000000, seed=b'0' * 64, storage=evidence)
        self.assertEqual(props(surya.child(tree, 'chosen'))['surya-ufs-probe'], mkadt.u32(1))
        self.assertEqual(props(surya.child(tree, 'sweet-ufs'))['surya-read-only'], mkadt.u32(1))
        for key, value in [('base', 0x1234000), ('size', 0x8000), ('status', 'disabled')]:
            bad = {**evidence, 'controller': {**evidence['controller'], key: value}}
            with self.subTest(key=key), self.assertRaises(ValueError):
                surya.build(0x100000000, 0x20000000, storage=bad)
        with self.assertRaises(ValueError):
            surya.build(0x100000000, 0x20000000, storage={**evidence, 'disk_writes_allowed': True})

    def test_memory_rejects_unaligned_or_framebuffer_overlap(self):
        for base, size in [(0x100000001, 0x20000000), (0x100000000, 1),
                           (0x9c000000, 0x20000000), (1 << 40, 0x20000000)]:
            with self.subTest(base=base, size=size), self.assertRaises(ValueError):
                surya.build(base, size)

    def test_apple_root_payload_is_separate_from_test_ramdisk(self):
        b = payload(apple=True)
        self.assertEqual(surya.validate_payload(b)['stage'], 'apple-root')
        cmd = surya.CMDLINE.encode()
        b[0x48:mkpayload.SEG_TABLE] = bytes(mkpayload.SEG_TABLE - 0x48)
        b[0x48:0x48 + len(cmd)] = cmd
        with self.assertRaises(ValueError):
            surya.validate_payload(b)

    def test_apple_diagnostic_payload_keeps_exact_storage_policy(self):
        b = payload(apple=True)
        cmd = surya.APPLE_DIAGNOSTIC_CMDLINE.encode()
        self.assertLess(len(cmd), mkpayload.BOOT_LINE_LENGTH)
        b[0x48:mkpayload.SEG_TABLE] = bytes(mkpayload.SEG_TABLE - 0x48)
        b[0x48:0x48 + len(cmd)] = cmd
        self.assertEqual(surya.validate_payload(b)['stage'], 'apple-root')
        bad = b.replace(b'sweetusb=0', b'sweetusb=1')
        with self.assertRaises(ValueError):
            surya.validate_payload(bad)

    def test_apple_gui_payload_keeps_exact_storage_policy(self):
        b = payload(apple=True)
        cmd = surya.APPLE_GUI_CMDLINE.encode()
        self.assertLess(len(cmd), mkpayload.BOOT_LINE_LENGTH)
        b[0x48:mkpayload.SEG_TABLE] = bytes(mkpayload.SEG_TABLE - 0x48)
        b[0x48:0x48 + len(cmd)] = cmd
        self.assertEqual(surya.validate_payload(b)['stage'], 'apple-root')
        bad = b.replace(b'sweetusb=0', b'sweetusb=1')
        with self.assertRaises(ValueError):
            surya.validate_payload(bad)

    def test_touch_diagnostic_enables_only_touch_and_visible_console(self):
        b = payload(apple=True)
        cmd = surya.APPLE_TOUCH_CMDLINE.encode()
        self.assertLess(len(cmd), mkpayload.BOOT_LINE_LENGTH)
        b[0x48:mkpayload.SEG_TABLE] = bytes(mkpayload.SEG_TABLE - 0x48)
        b[0x48:0x48 + len(cmd)] = cmd
        self.assertEqual(surya.validate_payload(b)['stage'], 'apple-root')
        self.assertIn(b'sweettouch=1', cmd)
        self.assertIn(b'sweetfb_noconsole=0', cmd)
        self.assertIn(b'suryatouchreport=1', cmd)
        self.assertNotIn('suryatouchreport', surya.APPLE_GUI_CMDLINE)
        for gated in (b'sweetusb=0', b'sweetkeys=0', b'sweetbattery=0', b'sweetlog=0'):
            self.assertIn(gated, cmd)
        with self.assertRaises(ValueError):
            surya.validate_payload(b.replace(b'sweetusb=0', b'sweetusb=1'))

    def test_gpu_table_profile_preserves_root_and_other_gates(self):
        b = payload(apple=True)
        cmd = surya.APPLE_GPU_PT_CMDLINE.encode()
        b[0x48:mkpayload.SEG_TABLE] = bytes(mkpayload.SEG_TABLE - 0x48)
        b[0x48:0x48 + len(cmd)] = cmd
        self.assertEqual(surya.validate_payload(b)['stage'], 'apple-root')
        self.assertIn(b'suryagpummu=1', cmd)
        self.assertNotIn('suryagpummu', surya.APPLE_GUI_CMDLINE)
        self.assertNotIn(b'suryatouchreport', cmd)
        self.assertIn(b'sweetusb=0', cmd)
        with self.assertRaises(ValueError):
            surya.validate_payload(b.replace(b'sweetusb=0', b'sweetusb=1'))

    def test_valid_ramdisk_payload(self):
        self.assertEqual(surya.validate_payload(payload())['board'], 'surya')

    def test_truncated_header_and_segment_table(self):
        b = payload()
        for length in (0, 8, mkpayload.SEG_TABLE - 1, mkpayload.SEG_TABLE + 32):
            with self.subTest(length=length), self.assertRaises(ValueError):
                surya.validate_payload(b[:length])

    def test_writable_command_line_rejected(self):
        b = payload()
        start = b.index(b'ufsrw=0')
        b[start:start + 7] = b'ufsrw=1'
        with self.assertRaisesRegex(ValueError, 'command line'):
            surya.validate_payload(b)

    def test_wrong_board_rejected(self):
        b = payload().replace(b'xiaomi,surya', b'xiaomi,sweet')
        with self.assertRaisesRegex(ValueError, 'not for surya'):
            surya.validate_payload(b)

    def test_outside_ram_entry_and_overlapping_segments(self):
        for offset, value in [(0x18, 0), (mkpayload.SEG_TABLE, 0xfffffe0107000000),
                              (mkpayload.SEG_TABLE + 32, 0xfffffe0007000000),
                              (mkpayload.SEG_TABLE + 16, 0), (0x20, 1 << 63)]:
            b = payload()
            struct.pack_into('<Q', b, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                surya.validate_payload(b)

    def test_upstream_sweet_still_selects_sweet(self):
        root = mkadt.build(seed=b'0' * 64, product_path=None)
        self.assertIn(b'xiaomi,sweet', props(root)['compatible'].split(b'\0'))
        self.assertEqual(len(surya.child(root, 'cpus').children), 8)


if __name__ == '__main__':
    unittest.main()
