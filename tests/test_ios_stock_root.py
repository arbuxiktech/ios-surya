import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_ios_stock_root


class StockRootTests(unittest.TestCase):
    def test_all_source_app_bundles_must_be_present_in_correct_cryptex(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            roots = {name: base / name for name in ('OS', 'SystemOS', 'AppOS')}
            destination = base / 'root'
            for name, root in roots.items():
                app = root / 'Applications' / f'{name}.app'
                app.mkdir(parents=True)
                (app / 'Info.plist').write_bytes(b'fixture')
            (roots['OS'] / 'System/Library/xpc').mkdir(parents=True)
            (roots['OS'] / 'System/Library/xpc/launchd.plist').write_bytes(b'fixture')
            (roots['SystemOS'] / 'System/Library').mkdir(parents=True)
            for name, root in roots.items():
                target = destination / verify_ios_stock_root.DESTINATIONS[name] / 'Applications' / f'{name}.app'
                target.mkdir(parents=True)
                (target / 'Info.plist').write_bytes(b'fixture')
            (destination / 'System/Library/xpc').mkdir(parents=True)
            (destination / 'System/Library/xpc/launchd.plist').write_bytes(b'fixture')
            (destination / 'private/preboot/Cryptexes/OS/System/Library').mkdir(parents=True)
            result = verify_ios_stock_root.verify(
                roots['OS'], roots['SystemOS'], roots['AppOS'], destination)
            self.assertEqual(result['total_app_bundles'], 3)
            shutil.rmtree(destination / 'private/preboot/Cryptexes/App/Applications/AppOS.app')
            with self.assertRaisesRegex(ValueError, 'missing after copy'):
                verify_ios_stock_root.verify(
                    roots['OS'], roots['SystemOS'], roots['AppOS'], destination)

    def test_incremental_build_inventory_finalizes_only_after_all_cryptexes(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            destination = base / 'root'
            inventory = base / 'inventory.json'
            sources = {name: base / name for name in ('OS', 'SystemOS', 'AppOS')}
            for name, source in sources.items():
                app = source / 'Applications' / f'{name}.app'
                app.mkdir(parents=True)
                (app / 'Info.plist').write_bytes(b'fixture')
                target = destination / verify_ios_stock_root.DESTINATIONS[name] / 'Applications' / f'{name}.app'
                target.mkdir(parents=True)
                (target / 'Info.plist').write_bytes(b'fixture')
            launchd = destination / 'System/Library/xpc/launchd.plist'
            launchd.parent.mkdir(parents=True)
            launchd.write_bytes(b'fixture')
            (destination / 'private/preboot/Cryptexes/OS/System/Library').mkdir(parents=True)
            (destination / 'private/preboot/Cryptexes/App/Applications').mkdir(
                parents=True, exist_ok=True)

            verify_ios_stock_root.add_image('AppOS', sources['AppOS'], destination, inventory)
            verify_ios_stock_root.add_image('SystemOS', sources['SystemOS'], destination, inventory)
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                verify_ios_stock_root.finalize(destination, inventory)
            verify_ios_stock_root.add_image('OS', sources['OS'], destination, inventory)
            result = verify_ios_stock_root.finalize(destination, inventory)
            self.assertEqual(result['total_app_bundles'], 3)


if __name__ == '__main__':
    unittest.main()
