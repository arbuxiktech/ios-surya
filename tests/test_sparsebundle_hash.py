import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import hash_sparsebundle


class SparseBundleHashTests(unittest.TestCase):
    def test_bundle_hash_covers_paths_and_file_contents_deterministically(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary) / 'root.sparsebundle'
            (bundle / 'bands').mkdir(parents=True)
            (bundle / 'Info.plist').write_bytes(b'plist')
            band = bundle / 'bands' / '0'
            band.write_bytes(b'data')
            first = hash_sparsebundle.digest_bundle(bundle)
            self.assertEqual(first, hash_sparsebundle.digest_bundle(bundle))
            band.write_bytes(b'data!')
            self.assertNotEqual(first, hash_sparsebundle.digest_bundle(bundle))


if __name__ == '__main__':
    unittest.main()
