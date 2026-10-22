import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import raw_to_android_sparse


class AndroidSparseTests(unittest.TestCase):
    def test_encode_round_trip_for_zero_and_raw_chunks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'raw.img'
            output = root / 'raw.simg'
            payload = (bytes(1 << 20) + bytes((i * 37) & 0xff for i in range(1 << 20))
                       + bytes(2 << 20) + b'not-zero' * 512)
            payload += bytes((-len(payload)) % raw_to_android_sparse.BLOCK_SIZE)
            source.write_bytes(payload)

            info = raw_to_android_sparse.encode(source, output, len(payload))

            raw = source.read_bytes()
            sparse = output.read_bytes()
            fields = __import__('struct').unpack_from('<IHHHHIIII', sparse)
            self.assertEqual(fields[:5], (raw_to_android_sparse.MAGIC, 1, 0, 28, 12))
            self.assertEqual(fields[5], raw_to_android_sparse.BLOCK_SIZE)
            self.assertEqual(fields[6], len(raw) // raw_to_android_sparse.BLOCK_SIZE)
            self.assertEqual(fields[8], zlib.crc32(raw) & 0xffffffff)

            cursor = 28
            rebuilt = bytearray()
            chunk_types = []
            for _ in range(fields[7]):
                kind, reserved, block_count, total_size = __import__('struct').unpack_from(
                    '<HHII', sparse, cursor)
                self.assertEqual(reserved, 0)
                cursor += 12
                chunk_types.append(kind)
                if kind == raw_to_android_sparse.CHUNK_DONT_CARE:
                    self.assertEqual(total_size, 12)
                    rebuilt.extend(bytes(block_count * raw_to_android_sparse.BLOCK_SIZE))
                elif kind == raw_to_android_sparse.CHUNK_RAW:
                    data_size = block_count * raw_to_android_sparse.BLOCK_SIZE
                    self.assertEqual(total_size, 12 + data_size)
                    rebuilt.extend(sparse[cursor:cursor + data_size])
                    cursor += data_size
                elif kind == raw_to_android_sparse.CHUNK_CRC32:
                    self.assertEqual((block_count, total_size), (0, 16))
                    self.assertEqual(__import__('struct').unpack_from('<I', sparse, cursor)[0],
                                     zlib.crc32(raw) & 0xffffffff)
                    cursor += 4
                else:
                    self.fail(f'unexpected sparse chunk type {kind:#x}')
            self.assertEqual(cursor, len(sparse))
            self.assertEqual(bytes(rebuilt), raw)
            self.assertEqual(chunk_types[-1], raw_to_android_sparse.CHUNK_CRC32)
            self.assertEqual(info['raw_sha256'], __import__('hashlib').sha256(raw).hexdigest())


if __name__ == '__main__':
    unittest.main()
