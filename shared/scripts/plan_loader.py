#!/usr/bin/env python3
"""Plan the native loader's symbol resolution before a single byte is relocated.

The loader binds an import in this order: host override table (by symbol name) ->
another module's export -> a linker-provided boundary marker. Anything left over has to
be written by hand, so this script exists to name that set exactly, per module, and to
show which host subsystem would take each symbol if the runtime were reused as-is.

Inputs are the module map produced by map_modules.py and the runtime source directory
whose override tables are scanned for registered names. Nothing is executed.
"""
import argparse
import collections
import json
import re
import sys
from pathlib import Path

# Linker boundary markers: the loader sets these to the section they name.
BOUNDARY = {
    '__rel_dyn_start', '__rel_dyn_end', '__rel_plt_start', '__rel_plt_end',
    '__rela_dyn_start', '__rela_dyn_end', '__rela_plt_start', '__rela_plt_end',
    '__got_start', '__got_end', '__EX_start', '__EX_end',
    '__tdata_align_abs', '__tdata_align_rel', '__tbss_align_abs', '__tbss_align_rel',
}


def runtime_overrides(runtime_dir):
    """symbol -> the runtime source file that registers it."""
    table = {}
    for path in sorted(list(runtime_dir.glob('*.c')) + list(runtime_dir.glob('*.m'))):
        text = path.read_text(errors='replace')
        for m in re.finditer(r'"((?:_Z|nn)[A-Za-z0-9_$.]+)"\s*,', text):
            table.setdefault(m.group(1), path.name)
    return table


def demangle_namespace(sym):
    m = re.match(r'_ZN2nn(\d+)([A-Za-z0-9_]+)', sym)
    if m:
        return 'nn::' + m.group(2)
    if sym.startswith('_Z'):
        return 'C++ 其他'
    return 'C 库'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('module_map', type=Path)
    ap.add_argument('--runtime', type=Path, required=True, help='runtime source directory to scan for overrides')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()

    doc = json.loads(args.module_map.read_text())
    overrides = runtime_overrides(args.runtime)

    exports_path = args.module_map.parent / 'exports.json'
    export_sets = json.loads(exports_path.read_text()) if exports_path.exists() else {}
    exported = {}
    for mod_name, names in export_sets.items():
        for sym in names:
            exported.setdefault(sym, mod_name)

    report = dict(runtime=str(args.runtime), override_symbols=len(overrides), modules={},
                  gaps=collections.Counter(), gap_symbols=[])
    for name, mod in doc['modules'].items():
        kinds = collections.Counter()
        gap = []
        for sym, count in mod['relocations']['imports'].items():
            if sym in BOUNDARY:
                kinds['boundary'] += count
            elif sym in overrides:
                kinds['host_override'] += count
            elif sym in exported:
                kinds['cross_module'] += count
            else:
                kinds['unbound'] += count
                gap.append(sym)
        report['modules'][name] = dict(imports=mod['relocations']['import_count'],
                                       resolution=dict(kinds),
                                       needs_host=sorted(gap))
        for sym in gap:
            report['gaps'][demangle_namespace(sym)] += 1
        report['gap_symbols'].extend(sorted(gap))

    report['gaps'] = dict(report['gaps'].most_common())
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')

    print(f"宿主 override 登记符号: {len(overrides)}")
    print(f"{'module':10s} {'导入':>7s} {'boundary':>9s} {'override':>9s} {'跨模块':>8s} {'未绑定':>8s}")
    for name, mod in report['modules'].items():
        k = mod['resolution']
        print(f"{name:10s} {mod['imports']:7,d} {k.get('boundary',0):9,d} "
              f"{k.get('host_override',0):9,d} {k.get('cross_module',0):8,d} {k.get('unbound',0):8,d}")
    print("\n按命名空间统计『无 override 且非 boundary』的符号:")
    for k, v in report['gaps'].items():
        print(f"  {v:6d}  {k}")


if __name__ == '__main__':
    main()
