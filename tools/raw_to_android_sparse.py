#!/usr/bin/env python3
"""Encode a raw block device or image as Android sparse format.

The input is read twice and never modified. 1 MiB windows of all-zero blocks
become DONT_CARE chunks; other windows are stored verbatim. This lets a Mac
runner turn its attached HFS+ sparse image into a fastboot-compatible .simg
without writing a second 16 GiB raw image first.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import zlib

MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_DONT_CARE = 0xCAC3
CHUNK_CRC32 = 0xCAC4
BLOCK_SIZE = 4096
WINDOW_SIZE = 1 << 20
MAX_CHUNK_BLOCKS = 8192


def scan(source: Path, size: int):
    if size <= 0 or size % BLOCK_SIZE:
        raise ValueError('input size must be a positive multiple of 4096 bytes')
    chunks: list[tuple[int, int, int]] = []
    checksum = 0
    digest = hashlib.sha256()
    with source.open('rb', buffering=0) as stream:
        offset = 0
        while offset < size:
            data = stream.read(min(WINDOW_SIZE, size - offset))
            if not data:
                raise ValueError(f'input ended early at byte {offset}')
            if len(data) % BLOCK_SIZE:
                raise ValueError(f'input window at byte {offset} is not block aligned')
            checksum = zlib.crc32(data, checksum)
            digest.update(data)
            kind = CHUNK_DONT_CARE if data.count(b'\0') == len(data) else CHUNK_RAW
            blocks = len(data) // BLOCK_SIZE
            if (chunks and chunks[-1][0] == kind and
                    chunks[-1][2] + blocks <= MAX_CHUNK_BLOCKS):
                old_kind, start, old_blocks = chunks[-1]
                chunks[-1] = (old_kind, start, old_blocks + blocks)
            else:
                chunks.append((kind, offset // BLOCK_SIZE, blocks))
            offset += len(data)
    if offset != size:
        raise ValueError('input size changed while scanning')
    return chunks, checksum & 0xFFFFFFFF, digest.hexdigest()


def encode(source: Path, output: Path, size: int):
    if output.exists():
        raise FileExistsError(f'refusing to overwrite existing output: {output}')
    chunks, checksum, digest = scan(source, size)
    total_blocks = size // BLOCK_SIZE
    if total_blocks > 0xFFFFFFFF or len(chunks) + 1 > 0xFFFFFFFF:
        raise ValueError('Android sparse format field overflow')
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + '.partial')
    if temporary.exists():
        raise FileExistsError(f'refusing to overwrite partial output: {temporary}')
    write_digest = hashlib.sha256()
    write_crc = 0
    try:
        with source.open('rb', buffering=0) as stream, temporary.open('xb') as out:
            out.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BLOCK_SIZE,
                                  total_blocks, len(chunks) + 1, checksum))
            for kind, start, blocks in chunks:
                raw_size = blocks * BLOCK_SIZE if kind == CHUNK_RAW else 0
                out.write(struct.pack('<HHII', kind, 0, blocks, 12 + raw_size))
                stream.seek(start * BLOCK_SIZE)
                remaining = blocks * BLOCK_SIZE
                while remaining:
                    data = stream.read(min(WINDOW_SIZE, remaining))
                    if not data:
                        raise ValueError(f'input ended while writing block {start}')
                    write_digest.update(data)
                    write_crc = zlib.crc32(data, write_crc)
                    if kind == CHUNK_RAW:
                        out.write(data)
                    elif data.count(b'\0') != len(data):
                        raise ValueError(f'source changed in a DONT_CARE chunk at block {start}')
                    remaining -= len(data)
            out.write(struct.pack('<HHII', CHUNK_CRC32, 0, 0, 16))
            out.write(struct.pack('<I', checksum))
        if write_digest.hexdigest() != digest or write_crc & 0xffffffff != checksum:
            raise ValueError('source changed between sparse-image scan and encoding')
        temporary.rename(output)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return {
        'raw_bytes': size,
        'sparse_bytes': output.stat().st_size,
        'block_size': BLOCK_SIZE,
        'total_blocks': total_blocks,
        'data_chunks': len(chunks),
        'crc32': f'{checksum:08x}',
        'raw_sha256': digest,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='raw image or read-only block device')
    parser.add_argument('output', type=Path, help='Android sparse output file')
    parser.add_argument('--size-bytes', type=int,
                        help='required for block devices; regular files use their file size')
    args = parser.parse_args()
    size = args.size_bytes
    if size is None:
        if not args.source.is_file():
            parser.error('--size-bytes is required when input is not a regular file')
        size = args.source.stat().st_size
    result = encode(args.source, args.output, size)
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
