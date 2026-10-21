#!/usr/bin/env python3
"""Bind the native loader to the verified rewrite and RomFS of this game.

Writes <out>/runtime_tables.h (modules, symbols, relocations, gates), romfs_config.h and
shared_fonts_config.h, plus the RomFS index the runtime reads at start. Adapted from the shared
builders (games/totk/scripts/prepare_runtime.py and its RomFS and font siblings, via by3); every
input is checked against assets/expected-v0.json before anything is written, so a stale or
half-written rewrite cannot reach the loader.

Paths are arguments rather than a fixed layout, because this game's build stages its data under
different directory names than the other two.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
DORA_ROOT = HERE.parents[1]
D2R = DORA_ROOT / 'refs/by3-port/games/totk/third_party/d2r'
sys.path.insert(0, str(D2R))
from relr import decode_relr   # noqa: E402  (the shared rewrite tooling's RELR decoder)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_digest(expected, name):
    """The rewrite hash, in this game's manifest shape (per module) or the shared one (per file)."""
    if 'rewritten_sha256' in expected:
        return expected['rewritten_sha256'][f'{name}.patched.bin']
    return expected['modules'][name]['patched_sha256']


def ctor_flag(image, tags, layout, name):
    """The constructor-state word DT_INIT tests (the loader checks it reads 2 after DT_INIT).

    The first adrp + ldr w pair on the same register within DT_INIT's first 16 instructions is
    decoded, so a shape other than the compiler's usual pair is visible instead of assumed."""
    init = tags[12]
    words = [struct.unpack_from('<I', image, init + 4 * i)[0] for i in range(16)]
    for i, a in enumerate(words):
        if a & 0x9f000000 != 0x90000000:
            continue
        reg = a & 31
        for j in range(i + 1, len(words)):
            w = words[j]
            if w & 0xffc00000 == 0xb9400000 and (w >> 5) & 31 == reg:
                imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
                imm = (imm ^ (1 << 20)) - (1 << 20)
                flag = ((init + 4 * i) & ~4095) + (imm << 12) + ((w >> 10) & 0xfff) * 4
                assert layout['DATA_OFF'] <= flag < layout['KEY_PAGE_OFF'], f'{name}: ctor flag outside data'
                print(f'{name}: DT_INIT ctor flag at {flag:#x} (adrp x{reg} at +{4 * i:#x}, ldr at +{4 * j:#x})')
                return flag
    raise ValueError(f'{name}: no constructor-state load in DT_INIT')


def module_table(rewritten, exefs, expected, name):
    path = rewritten / f'{name}.patched.bin'
    image = path.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    assert digest == expected_digest(expected, name), f'Unverified rewrite: {name}'
    assert sha(exefs / name) == expected['exefs_sha256'][name], f'Unverified ExeFS: {name}'
    layout = {}
    for line in (rewritten / f'{name}.fixups.tsv').read_text().splitlines():
        _, key, value = line.split()
        layout[key] = int(value, 0)
    mod0 = struct.unpack_from('<I', image, 4)[0]
    assert image[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags = {}
    for offset in range(dyn, dyn + 4096 * 16, 16):
        tag, value = struct.unpack_from('<qQ', image, offset)
        if tag == 0:
            break
        tags[tag] = value
    else:
        raise ValueError('Unterminated dynamic table')
    if 4 in tags:
        nsym = struct.unpack_from('<I', image, tags[4] + 4)[0]
    else:
        raw_header = (exefs / name).read_bytes()[:256]
        _, _, symbol_offset, symbol_size = struct.unpack_from('<IIII', raw_header, 0x90)
        assert layout['RO_OFF'] + symbol_offset == tags[6] and symbol_size % 24 == 0
        nsym = symbol_size // 24
    assert tags[11] == 24 and tags[9] == 24 and tags[20] == 7
    symbols = []
    for i in range(nsym):
        no, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
        end = image.index(0, tags[5] + no, tags[5] + tags[10])
        symbols.append(dict(name=image[tags[5] + no:end].decode(), value=value, size=size,
                            defined=bool(section), binding=info >> 4, type=info & 15))
    relocs = [dict(where=where, kind=1027, symbol=0, addend=addend)
              for where, addend in decode_relr(image, tags, layout['DATA_OFF'], layout['KEY_PAGE_OFF'])]
    relocated = {reloc['where'] for reloc in relocs}
    for address, size in [(tags[7], tags[8]), (tags[23], tags[2])]:
        assert size % 24 == 0
        for offset in range(address, address + size, 24):
            where, info, addend = struct.unpack_from('<QQq', image, offset)
            kind, si = info & 0xffffffff, info >> 32
            assert kind in [257, 1025, 1026, 1027] and si < nsym
            assert layout['DATA_OFF'] <= where <= layout['KEY_PAGE_OFF'] - 8
            assert kind != 1027 or 0 <= addend < layout['KEY_PAGE_OFF']
            assert where not in relocated, 'Duplicate RELR/RELA relocation'
            relocated.add(where)
            relocs.append(dict(where=where, kind=kind, symbol=si, addend=addend))
    gates = []
    for line in (rewritten / f'{name}.gates.tsv').read_text().splitlines():
        ret, kind, arg, original = line.split()
        assert kind in ['cntpct', 'cntvct', 'svc']
        gates.append(dict(ret=int(ret, 0), timer=kind != 'svc', arg=int(arg), original=int(original, 0)))
    return dict(name=name, sha256=digest, size=len(image), layout=layout, tags=tags, symbols=symbols,
                relocs=relocs, gates=gates, ctor_flag=ctor_flag(image, tags, layout, name))


def probe_plt(image, layout, slot):
    """The PLT entry that resolves nn::os::GetSystemTick, used by the loader's timing self-check.

    A PLT slot loads its target address from the GOT and branches to it; the address it loads is the
    symbol's slot, which is what identifies the right stub among the others in the same page."""
    found = []
    for address in range(layout['TEXT_END'] - 0x10000, layout['TEXT_END'] - 12, 4):
        a, b, c, d = struct.unpack_from('<IIII', image, address)
        if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220:
            continue
        imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        if (address & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8 == slot:
            found.append(address)
    return found


def runtime_tables(rewritten, exefs, out, expected):
    modules = [module_table(rewritten, exefs, expected, n) for n in ['main', 'subsdk0', 'sdk']]
    main = modules[0]
    target = '_ZN2nn2os13GetSystemTickEv'
    slot = next(r['where'] for r in main['relocs']
                if main['symbols'][r['symbol']]['name'] == target and r['kind'] == 1026)
    image = (rewritten / 'main.patched.bin').read_bytes()
    plt = probe_plt(image, main['layout'], slot)
    if len(plt) != 1:
        print(f'warning: GetSystemTick PLT matches {len(plt)} sites; the timing self-check needs one')
    lines = ['/* Generated locally from validated player images; do not commit. */']
    for n, m in enumerate(modules):
        lines.append(f'static const Reloc reloc_{n}[] = {{')
        lines.extend(f'{{{r["where"]}ULL,{r["kind"]},{r["symbol"]},{r["addend"]}LL}},' for r in m['relocs'])
        lines.append('};')
        lines.append(f'static const Symbol symbol_{n}[] = {{')
        lines.extend('{' + f'{json.dumps(s["name"])},{s["value"]}ULL,{s["size"]}ULL,'
                           f'{int(s["defined"])},{s["binding"]},{s["type"]}' + '},' for s in m['symbols'])
        lines.append('};')
        lines.append(f'static const Gate gate_{n}[] = {{')
        lines.extend(f'{{{g["ret"]}ULL,{g["original"]}ULL,{int(g["timer"])},{g["arg"]}}},' for g in m['gates'])
        if not m['gates']:
            lines.append('{0,0,0,0},')
        lines.append('};')
    lines.append('static Module modules[] = {')
    for n, m in enumerate(modules):
        l = m['layout']
        lines.append('{' + f'.name={json.dumps(m["name"])},.digest={json.dumps(m["sha256"])},'
                           f'.file_size={m["size"]}ULL,.text_end={l["TEXT_END"]}ULL,.ro_off={l["RO_OFF"]}ULL,'
                           f'.data_off={l["DATA_OFF"]}ULL,.patch_off={l["PATCH_OFF"]}ULL,.key_off={l["KEY_PAGE_OFF"]}ULL,'
                           f'.symbols=symbol_{n},.nsym={len(m["symbols"])},.relocs=reloc_{n},.nreloc={len(m["relocs"])},'
                           f'.gates=gate_{n},.ngate={len(m["gates"])},.init={m["tags"].get(12, 0)}ULL,'
                           f'.ctor_flag={m["ctor_flag"]}ULL,.init_array={m["tags"].get(25, 0)}ULL,'
                           f'.init_size={m["tags"].get(27, 0)}ULL' + '},')
    lines.append('};')
    lines.append(f'#define PROBE_PLT {plt[0] if plt else 0}ULL\n#define PROBE_SLOT {slot}ULL')
    (out / 'runtime_tables.h').write_text('\n'.join(lines) + '\n')
    (out.parent / 'analysis').mkdir(exist_ok=True)
    (out.parent / 'analysis/runtime-layout.json').write_text(json.dumps(modules, indent=2) + '\n')
    return dict(modules=len(modules), relocations=sum(len(m['relocs']) for m in modules),
                main_plt=hex(plt[0]) if plt else None)


def romfs_config(romfs, out, expected):
    manifest = json.loads((romfs / 'index.json').read_text())
    assert manifest['status'] == 'pass' and manifest['romfs_sha256'] == expected['romfs']['sha256']
    assert (romfs / 'romfs.bin').stat().st_size == manifest['romfs_size'] == expected['romfs']['size']
    strings, rows = bytearray(), bytearray()
    for item in manifest['files']:
        name = item['path'].encode() + b'\0'
        rows.extend(struct.pack('<QQII', item['offset'], item['size'], len(strings), len(name)))
        strings.extend(name)
    index = struct.pack('<8sQQ', b'TOTKRFS1', len(manifest['files']), 24 + len(rows)) + rows + strings
    (romfs / 'runtime-index.bin').write_bytes(index)
    (out / 'romfs_config.h').write_text(f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ULL\n'
                                        f'#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n'
                                        f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
    return dict(files=len(manifest['files']), index_bytes=len(index))


def shared_fonts_config(work, out):
    """The six system fonts, when extracted to <work>/extracted/firmware/shared-fonts.

    This game ships its own fonts inside RomFS (Unity and CRIWARE text), so the rows are expected to
    stay empty; they exist so the loader's shared-font path has something valid to read."""
    folder = work / 'extracted/firmware/shared-fonts'
    if not (folder / 'manifest.json').exists():
        rows = ['{"",0ULL,0ULL,""}'] * 6
        state = 'missing (this game is expected not to need them)'
    else:
        manifest = json.loads((folder / 'manifest.json').read_text())
        assert manifest['status'] == 'pass' and [f['type'] for f in manifest['fonts']] == list(range(6))
        rows = []
        for f in manifest['fonts']:
            path = folder / f"{f['type']}-{f['name']}.bfttf"
            assert path.stat().st_size == f['bfttf_bytes'] == f['font_bytes'] + 8
            rows.append(f'{{"{path}",{f["bfttf_bytes"]}ULL,{f["font_bytes"]}ULL,"{f["sha256"]}"}}')
        state = 'loaded from ' + str(folder)
    (out / 'shared_fonts_config.h').write_text(
        '#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS ' + ','.join(rows) + '\n')
    return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rewritten', type=Path, default=DORA_ROOT / 'dora_build/20_rewritten')
    parser.add_argument('--exefs', type=Path, default=DORA_ROOT / 'dora_build/10_extracted/exefs')
    parser.add_argument('--romfs', type=Path, default=DORA_ROOT / 'dora_build/10_extracted/romfs')
    parser.add_argument('--out', type=Path, default=DORA_ROOT / 'dora_build/build')
    parser.add_argument('--expected', type=Path, default=HERE / 'assets/expected-v0.json')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    expected = json.loads(args.expected.read_text())
    print(json.dumps(dict(runtime=runtime_tables(args.rewritten, args.exefs, args.out, expected),
                          romfs=romfs_config(args.romfs, args.out, expected),
                          shared_fonts=shared_fonts_config(args.out.parent, args.out))))


if __name__ == '__main__':
    main()
