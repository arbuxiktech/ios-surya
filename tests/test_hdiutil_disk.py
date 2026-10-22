import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from hdiutil_disk import backing_disk


class HdiutilDiskTests(unittest.TestCase):
    def test_selects_partitioned_image_over_synthesized_apfs_container(self):
        attached = {
            'system-entities': [
                {'dev-entry': '/dev/disk4',
                 'content-hint': 'GUID_partition_scheme'},
                {'dev-entry': '/dev/disk4s1',
                 'content-hint': 'Apple_APFS'},
                {'dev-entry': '/dev/disk5',
                 'content-hint': 'Apple_APFS'},
                {'dev-entry': '/dev/disk5s1',
                 'content-hint': 'Apple_APFS',
                 'mount-point': '/tmp/source/AppCryptex'},
            ]
        }
        self.assertEqual(backing_disk(attached), '/dev/disk4')

    def test_returns_single_unpartitioned_hfs_image(self):
        attached = {'system-entities': [
            {'dev-entry': '/dev/disk8', 'content-hint': 'Apple_HFS'},
            {'dev-entry': '/dev/disk8s2', 'mount-point': '/tmp/root'},
        ]}
        self.assertEqual(backing_disk(attached), '/dev/disk8')

    def test_uses_first_whole_disk_if_no_partition_hint_exists(self):
        attached = {'system-entities': [
            {'dev-entry': '/dev/disk9', 'content-hint': 'Apple_APFS'},
            {'dev-entry': '/dev/disk10', 'content-hint': 'Apple_APFS'},
        ]}
        self.assertEqual(backing_disk(attached), '/dev/disk9')

    def test_rejects_plist_without_a_whole_disk_device(self):
        with self.assertRaisesRegex(ValueError, 'no whole-disk device'):
            backing_disk({'system-entities': [
                {'dev-entry': '/dev/disk4s1'}
            ]})


if __name__ == '__main__':
    unittest.main()
