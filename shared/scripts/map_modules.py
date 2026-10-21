#!/usr/bin/env python3
"""Map the three Program modules of Doraemon Story of Seasons for the native loader.

Reads each NSO, decodes its memory image, walks MOD0 / .dynamic, and reports what the
loader needs: segment layout, relocation inventory (RELA + JMPREL + RELR), imported
symbols grouped by providing module, exported symbols, and the init/fini array sizes.

No code is executed and no key is needed: the ExeFS modules are already decrypted.
"""
import argparse
import collections
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'refs/by3-port/games/totk/third_party/d2r'))
import nsoimg

DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT = 5, 6, 10, 11
DT_RELA, DT_RELASZ, DT_RELAENT = 7, 8, 9
DT_JMPREL, DT_PLTRELSZ, DT_PLTREL = 23, 2, 20
DT_HASH, DT_GNU_HASH = 4, 0x6ffffef5
DT_INIT_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAY, DT_FINI_ARRAYSZ = 25, 27, 26, 28
DT_RELR, DT_RELRSZ = 36, 35

R_AARCH64_RELATIVE = 1027


def dynamic(img, m0):
    off = m0 + struct.unpack_from('<i', img, m0 + 4)[0]
    tags = {}
    o = off
    while True:
        tag, val = struct.unpack_from('<qQ', img, o)
        o += 16
        if tag == 0:
            break
        tags.setdefault(tag, val)
    return off, tags


def cstr(img, p):
    e = img.index(b'\0', p)
    return img[p:e].decode('latin1')


def symbols(img, tags):
    """Defined and undefined dynamic symbols, in table order."""
    if DT_SYMTAB not in tags:
        return []
    tab, strtab = tags[DT_SYMTAB], tags[DT_STRTAB]
    total = 0
    if DT_HASH in tags:
        total = struct.unpack_from('<I', img, tags[DT_HASH] + 4)[0]
    elif DT_GNU_HASH in tags:
        nbuckets, symoff, bloom, shift = struct.unpack_from('<IIII', img, tags[DT_GNU_HASH])
        buckets = struct.unpack_from(f'<{nbuckets}I', img, tags[DT_GNU_HASH] + 16)
        chains = tags[DT_GNU_HASH] + 16 + 4 * nbuckets + 4 * bloom
        total = symoff
        for b in buckets:
            if b < symoff:
                continue
            while True:
                chain = struct.unpack_from('<I', img, chains + 4 * (b - symoff))[0]
                total = max(total, b + 1)
                b += 1
                if chain & 1:
                    break
    else:
        raise SystemExit('no DT_HASH / DT_GNU_HASH: symbol count unknown')
    out = []
    for i in range(1, total):
        name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, tab + 24 * i)
        out.append(dict(name=cstr(img, strtab + name_off) if name_off else '',
                        value=value, size=size, bind=info >> 4, type=info & 0xF,
                        shndx=shndx, defined=shndx != 0))
    return out


def relocations(img, tags, syms):
    by_type = collections.Counter()
    by_provider = collections.Counter()
    imports = collections.Counter()
    unresolved = []
    entries = 0

    # RELR first: relative only, no symbol.
    if DT_RELR in tags:
        for _where, _addend in nsoimg.decode_relr(img, tags, 0, len(img)):
            by_type[R_AARCH64_RELATIVE] += 1
            entries += 1

    for tab_tag, size_tag, ent in ((DT_RELA, DT_RELASZ, 24), (DT_JMPREL, DT_PLTRELSZ, 24)):
        tab = tags.get(tab_tag)
        if not tab:
            continue
        for i in range(tags.get(size_tag, 0) // ent):
            off, info, addend = struct.unpack_from('<QQq', img, tab + ent * i)
            typ, si = info & 0xFFFFFFFF, info >> 32
            by_type[typ] += 1
            entries += 1
            if typ == R_AARCH64_RELATIVE or not si:
                continue
            sym = syms[si - 1] if si - 1 < len(syms) else None
            name = sym['name'] if sym else f'<sym {si}>'
            if sym is not None and sym['defined']:
                continue
            imports[name] += 1
            module = name.split('::')[0] if '::' in name else (name.split('_')[0] if '_' in name else name)
            by_provider[module] += 1
            if typ not in (257, 1025, 1026):      # ABS64, GLOB_DAT, JUMP_SLOT
                unresolved.append(dict(symbol=name, type=typ, offset=off))

    return dict(total=entries, by_type=dict(sorted(by_type.items())),
                imports=dict(sorted(imports.items(), key=lambda kv: -kv[1])),
                import_count=sum(imports.values()),
                by_provider=dict(sorted(by_provider.items(), key=lambda kv: -kv[1])),
                unexpected=unresolved)


def segment_layout(path):
    d = Path(path).read_bytes()
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from('<I', d, 0x3c)[0]
    csz = struct.unpack_from('<III', d, 0x60)
    out = []
    for i, (fo, mo, sz) in enumerate(segs):
        out.append(dict(index=i, file_offset=fo, memory_offset=mo, size=sz,
                        compressed=bool(flags >> i & 1), stored=csz[i],
                        perms=('r-x' if i == 0 else 'rw-' if i == 1 else 'rw-')))
    out.append(dict(index=3, kind='bss', memory_offset=segs[2][1] + segs[2][2], size=bss))
    return dict(nso_size=len(d), flags=flags, segments=out,
                image_size=segs[2][1] + segs[2][2] + bss)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('exefs', type=Path, help='directory holding main, sdk, subsdk0, rtld')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()

    report = dict(game='Doraemon Story of Seasons: Friends of the Great Kingdom',
                  title_id='0100F5E0177AC000', modules={}, totals=dict(relocations=0, imports=0, image=0))
    export_sets = {}
    for name in ('main', 'subsdk0', 'sdk', 'rtld'):
        path = args.exefs / name
        img = nsoimg.load(path)
        m0 = nsoimg.mod0(img)
        dyn_off, tags = dynamic(img, m0)
        syms = symbols(img, tags)
        rel = relocations(img, tags, syms)
        exports = sorted({s['name'] for s in syms if s['defined'] and s['name']})
        init = tags.get(DT_INIT_ARRAY)
        report['modules'][name] = dict(
            path=str(path), **segment_layout(path),
            mod0_offset=m0, dynamic_offset=dyn_off,
            symbol_count=len(syms), export_count=len(exports),
            export_names=len(exports),
            init_array=dict(offset=init, size=tags.get(DT_INIT_ARRAYSZ, 0),
                            entries=tags.get(DT_INIT_ARRAYSZ, 0) // 8 if init else 0),
            fini_array=dict(offset=tags.get(DT_FINI_ARRAY), size=tags.get(DT_FINI_ARRAYSZ, 0),
                            entries=tags.get(DT_FINI_ARRAYSZ, 0) // 8 if tags.get(DT_FINI_ARRAY) else 0),
            relocations=rel)
        export_sets[name] = exports
        report['totals']['relocations'] += rel['total']
        report['totals']['imports'] += rel['import_count']
        report['totals']['image'] += report['modules'][name]['image_size']

    # Which module provides what an importer asks for. The full export tables stay out of the
    # report (sdk alone exports tens of thousands of names) and live in exports.json.
    exported = {}
    for name, names in export_sets.items():
        for sym in names:
            exported.setdefault(sym, name)
    for name, mod in report['modules'].items():
        resolved = collections.Counter()
        missing = []
        for sym in mod['relocations']['imports']:
            provider = exported.get(sym)
            if provider is None:
                missing.append(sym)
            else:
                resolved[provider] += 1
        mod['cross_module'] = dict(resolved_by=dict(sorted(resolved.items(), key=lambda kv: -kv[1])),
                                   resolvable=sum(resolved.values()), unresolved=sorted(missing))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    (args.output.parent / 'exports.json').write_text(json.dumps(export_sets, indent=0) + '\n')

    print(f"{'module':10s} {'image':>12s} {'syms':>7s} {'reloc':>9s} {'imports':>8s} {'init':>5s}")
    for name, mod in report['modules'].items():
        print(f"{name:10s} {mod['image_size']:12,d} {mod['symbol_count']:7,d} "
              f"{mod['relocations']['total']:9,d} {mod['relocations']['import_count']:8,d} "
              f"{mod['init_array']['entries']:5d}")
    print(f"{'TOTAL':10s} {report['totals']['image']:12,d} {'':7s} "
          f"{report['totals']['relocations']:9,d} {report['totals']['imports']:8,d}")


if __name__ == '__main__':
    main()
