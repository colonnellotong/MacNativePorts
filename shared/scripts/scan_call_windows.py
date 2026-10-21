#!/usr/bin/env python3
"""Inventory every `blr x18` call window in an NSO, classified by the instruction before it.

The shared rewriter implements `blr x18` as a two-word window: the word before the call is replayed in a
stub, then the call runs at its original address. Only prefixes the rewriter knows how to replay are
accepted, so a new game can stop the rewrite on a shape TOTK and Bayonetta never produced. This script
lists those shapes before the rewrite runs, so the work can be scoped instead of discovered one crash at
a time.

    python3 scan_call_windows.py <nso file> [--limit N]
"""
import struct
import sys
from collections import Counter, defaultdict

import lz4.block
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

# What call_window.movable_store() accepts today.
KNOWN = ('str', 'stp', 'stur', 'ldr', 'ldp', 'ldur')


def text_of(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        raise ValueError('Not an NSO0')
    flags, = struct.unpack_from('<I', d, 0xc)
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from('<III', d, 0x60)
    fo, _, size = segs[0]
    raw = d[fo:fo + csz[0]]
    return lz4.block.decompress(raw, uncompressed_size=size) if flags & 1 else raw


def main():
    path = sys.argv[1]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[2] == '--limit' else 12
    text = text_of(path)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True

    # Walk every 4 bytes; cheap pre-filter on the encoding before disassembling.
    windows = []
    for off in range(0, len(text) - 4, 4):
        if struct.unpack_from('<I', text, off + 4)[0] != 0xd63f0240:   # blr x18
            continue
        prefix = struct.unpack_from('<I', text, off)[0]
        ins = next(md.disasm(struct.pack('<I', prefix), off), None)
        mnemonic = ins.mnemonic if ins else None
        windows.append((off, mnemonic, ins.op_str if ins else '', prefix))

    shapes = Counter(m for _, m, _, _ in windows)
    print(f'{path}: {len(windows)} `blr x18` windows')
    print('\nprefix instruction histogram:')
    for mnemonic, count in shapes.most_common():
        verdict = 'accepted' if mnemonic in KNOWN else 'REJECTED'
        print(f'  {str(mnemonic):<10} {count:>7}  {verdict}')

    rejected = [w for w in windows if w[1] not in KNOWN]
    if not rejected:
        print('\nEvery window uses a prefix the rewriter already replays.')
        return
    print(f'\n{len(rejected)} windows need a new prefix kind; first {limit}:')
    by_shape = defaultdict(list)
    for w in rejected:
        by_shape[w[1]].append(w)
    for mnemonic, group in by_shape.items():
        print(f'\n  {mnemonic} ({len(group)} sites)')
        for off, _, op_str, word in group[:limit]:
            print(f'    {off:#010x}  {word:#010x}  {mnemonic} {op_str}')


if __name__ == '__main__':
    main()
