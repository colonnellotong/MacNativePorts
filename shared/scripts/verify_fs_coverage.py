#!/usr/bin/env python3
"""Check that every nn::fs symbol the game imports resolves through the override chain.

The chain is runtime_override -> runtime_game_override (this game's adapter) -> runtime_fs_override
(RomFS) -> runtime_fs_host_override (save/temp/cache storage). This reads the tables straight out of
the sources, so a table that drifts away from the game's imports fails here instead of at runtime.

Also verifies that no two tables claim the same symbol: the chain returns the first hit, so a
duplicate would silently shadow the later one.
"""
import argparse
import json
import pathlib
import re
import sys

# symbol -> (file, c function). The regex covers the three shapes the tables use.
ENTRY = re.compile(r'\{"(_ZN2nn2fs[A-Za-z0-9_$.]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\}')


def table(path):
    src = path.read_text()
    return {m.group(1): (path.name, m.group(2)) for m in ENTRY.finditer(src)}


def arity(sym):
    """Parameter count from the Itanium mangled name: letters are types, E ends the parameter list."""
    body = sym[len('_ZN'):]
    i = 0
    while i < len(body) and (body[i].isdigit() or body[i].isdigit() is False and body[i].isalpha()):
        if body[i].isdigit():
            n = 0
            while i < len(body) and body[i].isdigit():
                n = n * 10 + int(body[i])
                i += 1
            i += n
        else:
            break
    if i < len(body) and body[i] == 'E':
        i += 1
    params, depth = [], 0
    buf = ''
    for c in body[i:]:
        if c in 'PKRVO':
            buf += c
            continue
        if c.isdigit():
            buf += c
            continue
        if depth == 0 and buf:
            params.append(buf)
            buf = ''
        elif depth > 0:
            buf += c
        depth += c == 'N'
        if c == 'E' and depth:
            depth -= 1
    if buf:
        params.append(buf)
    return len([p for p in params if p != 'v'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--module-map', required=True, type=pathlib.Path)
    ap.add_argument('--runtime', required=True, type=pathlib.Path, help='the shared runtime directory')
    ap.add_argument('--adapter', required=True, type=pathlib.Path, help='dora_fs_ext.c')
    ap.add_argument('--json', type=pathlib.Path)
    args = ap.parse_args()

    imports = {}
    doc = json.loads(args.module_map.read_text())
    for mod in ('main', 'subsdk0', 'sdk'):
        for sym, n in doc['modules'][mod]['relocations']['imports'].items():
            if '_ZN2nn2fs' in sym:
                imports[sym] = imports.get(sym, 0) + n

    layers = [
        ('adapter', table(args.adapter)),
        ('romfs', table(args.runtime / 'romfs.c')),
        ('fs_host', table(args.runtime / 'fs_host.c')),
    ]

    owner, duplicates = {}, []
    for name, tab in layers:
        for sym in tab:
            if sym in owner:
                duplicates.append((sym, owner[sym], name))
            else:
                owner[sym] = name

    resolved, missing = {}, []
    for sym in imports:
        if sym in owner:
            layer = owner[sym]
            tab = dict(layers)[layer]
            src, fn = tab[sym]
            resolved[sym] = dict(layer=layer, source=src, function=fn, imports=imports[sym],
                                 params=arity(sym))
        else:
            missing.append(sym)

    report = dict(
        game_imports=len(imports),
        resolved=len(resolved),
        missing=len(missing),
        duplicates=[dict(symbol=s, first=a, second=b) for s, a, b in duplicates],
        by_layer={name: sum(1 for v in resolved.values() if v['layer'] == name) for name, _ in layers},
        unresolved=missing,
        detail=dict(sorted(resolved.items())),
    )
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n')

    print(f"game nn::fs imports : {len(imports)}")
    print(f"resolved            : {len(resolved)}")
    for name, _ in layers:
        print(f"  via {name:8s}    : {report['by_layer'][name]}")
    print(f"missing             : {len(missing)}")
    for s in missing:
        print(f"  MISSING {s}")
    if duplicates:
        print(f"duplicate claims    : {len(duplicates)}")
        for s, a, b in duplicates:
            print(f"  {a} and {b} both claim {s}")

    ok = not missing and not duplicates
    print(f"\n{'PASS' if ok else 'FAIL'}: chain is {'complete and unambiguous' if ok else 'incomplete'}")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
