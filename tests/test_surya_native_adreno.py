"""Unit tests for Surya NativeAdreno IOKit driver and device-tree integration."""
from pathlib import Path
import hashlib, json, runpy, struct, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]


class SuryaNativeAdrenoTests(unittest.TestCase):
    def test_adt_includes_arm_io_gpu_node(self):
        import sys
        sys.path.insert(0, str(ROOT / 'tools'))
        import mkadt
        tree = mkadt.build(highmem=True)

        def find_node(n, path):
            parts = path.strip('/').split('/')
            cur = n
            for p in parts:
                found = None
                for c in cur.children:
                    if c.name == p:
                        found = c
                        break
                if not found:
                    return None
                cur = found
            return cur

        gpu = find_node(tree, 'arm-io/gpu')
        self.assertIsNotNone(gpu, "arm-io/gpu node must exist in ADT")
        props = {k: v for k, f, v in gpu.props}
        self.assertEqual(props['name'], b'gpu\0')
        self.assertEqual(props['device_type'], b'gpu\0')
        self.assertIn(b'qcom,adreno\0', props['compatible'])
        self.assertIn(b'qcom,adreno-618\0', props['compatible'])
        self.assertIn(b'arm-io,gpu\0', props['compatible'])
        # Chip ID must be 0x06010800 (Adreno 618)
        self.assertEqual(props['qcom,chipid'], struct.pack('<I', 0x06010800))

    def test_patch_0094_is_generated_from_canonical_sources(self):
        tool = runpy.run_path(str(ROOT / 'tools/gen_surya_native_adreno_patch.py'))
        expected = (ROOT / 'xnu/patches/0094-surya-unixnu-native-adreno.patch').read_text(encoding='utf-8')
        self.assertEqual(tool['generate'](), expected)

    def test_native_adreno_wire_abi_sizes_and_constants(self):
        abi_header = (ROOT / 'graphics/native/SuryaNativeAdrenoABI.h').read_text()
        self.assertIn('NativeAdrenoAbiVersion = 1', abi_header)
        self.assertIn('NativeAdrenoAbiMaxBuffers = 16', abi_header)
        self.assertIn('NativeAdrenoMethodQuery = 1', abi_header)
        self.assertIn('NativeAdrenoMethodBoAlloc = 2', abi_header)
        self.assertIn('NativeAdrenoMethodBoMap = 3', abi_header)
        self.assertIn('NativeAdrenoMethodBoFree = 4', abi_header)
        self.assertIn('NativeAdrenoMethodSubmit = 5', abi_header)
        self.assertIn('NativeAdrenoMethodWaitFence = 6', abi_header)
        self.assertIn('NativeAdrenoMethodGetTimestamp = 7', abi_header)
        self.assertIn('NativeAdrenoMethodGetStatus = 8', abi_header)
        self.assertIn('NativeAdrenoMethodSetMetadata = 9', abi_header)
        self.assertIn('NativeAdrenoMethodGetMetadata = 10', abi_header)
        self.assertIn('NativeAdrenoMethodCreateQueue = 11', abi_header)
        self.assertIn('NativeAdrenoMethodCloseQueue = 12', abi_header)
        self.assertIn('NativeAdrenoMethodBoCpuMap = 13', abi_header)
        self.assertIn('NativeAdrenoMethodBoCpuUnmap = 14', abi_header)
        self.assertIn('NativeAdrenoMethodGetDeviceInfo = 15', abi_header)
        self.assertIn('NativeAdrenoMethodBoImport = 16', abi_header)


if __name__ == '__main__':
    unittest.main()
