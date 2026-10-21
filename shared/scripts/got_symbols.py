#!/usr/bin/env python3
"""Name the imported functions a rewritten module calls, through its own GOT/PLT.

A game module's .dynsym holds only what it imports and the handful of functions it
exports, so a crash offset inside it ("main+0x384a80") names nothing. Every call it
makes to the SDK still goes through a PLT stub that loads a GOT slot, and the loader
table records which symbol each slot was filled with — so the callee can be named
from the image alone.

  scripts/got_symbols.py main --at 0x384a80            # disassemble around an offset
  scripts/got_symbols.py main --at 0x384a80 --span 0x40
  scripts/got_symbols.py main --stubs                  # every PLT stub it has
"""
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_WORK = os.path.join(HERE, "..", "..", "..", "dora_build")
MODULE_INDEX = {"main": 0, "subsdk0": 1, "sdk": 2}


def load_tables(work, index):
    """The loader tables are generated C; only the two arrays this needs are read."""
    path = os.path.join(work, "build", "runtime_tables.h")
    text = open(path, "r", errors="replace").read()
    want = "static const %s %s_%d[] = {"
    out = {}
    for kind in ("Reloc", "Symbol"):
        start = text.find(want % (kind, kind.lower(), index))
        if start < 0:
            sys.exit("no %s_%d in %s" % (kind.lower(), index, path))
        end = text.index("\n};", start)
        body = text[text.index("{", start):end]
        out[kind.lower()] = re.findall(r"\{([^{}]*)\}", body)
    relocs = []
    for item in out["reloc"]:
        where, kind, symbol, addend = [p.strip() for p in item.split(",")]
        relocs.append((int(where.rstrip("UL"), 0), int(kind), int(symbol), int(addend.rstrip("L"), 0)))
    symbols = []
    for item in out["symbol"]:
        name, value, size, defined, binding, stype = [p.strip() for p in item.split(",")]
        symbols.append(name.strip('"'))
    return relocs, symbols


def got_names(relocs, symbols):
    """GOT slot address -> imported name (the slots the PLT stubs load from)."""
    names = {}
    for where, kind, symbol, _ in relocs:
        if kind in (1025, 1026, 257) and 0 < symbol < len(symbols) and symbols[symbol]:
            names[where] = symbols[symbol]
    return names


def local_stub(image, target, names):
    """A PLT stub is four instructions: adrp xN, page / ldr xM,[xN,#lo] / add xN,xN,#lo / br xM.
    Decoding it on demand beats scanning the text: capstone stops at the first undecodable word,
    so a whole-image scan needs skipdata and turns into ten million skipped words."""
    try:
        import capstone
    except ImportError:
        sys.exit("capstone is needed: pip install capstone")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    insns = list(md.disasm(image[target:target + 16], target))
    if len(insns) < 4:
        return None
    a, b, c, d = insns[:4]
    if not (a.mnemonic == "adrp" and b.mnemonic == "ldr" and c.mnemonic == "add" and d.mnemonic == "br"):
        return None
    page = a.operands[1].imm
    slot = page + b.operands[1].mem.disp
    return slot, names.get(slot)


def plt_stubs(image, text_end, names):
    """Every stub in the image. Slow (skipdata over the whole text); only for --stubs."""
    try:
        import capstone
    except ImportError:
        sys.exit("capstone is needed: pip install capstone")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    md.skipdata = True
    stubs = {}
    window = []
    for insn in md.disasm(image[:text_end], 0):
        if insn.id == 0:
            window = []
            continue
        window.append(insn)
        if len(window) > 4:
            window.pop(0)
        if len(window) < 4:
            continue
        a, b, c, d = window
        if not (a.mnemonic == "adrp" and b.mnemonic == "ldr" and c.mnemonic == "add" and d.mnemonic == "br"):
            continue
        if not (a.operands[0].reg == b.operands[0].reg == c.operands[0].reg):
            continue
        slot = a.operands[1].imm + b.operands[1].mem.disp
        stubs[a.address] = (slot, names.get(slot))
    return stubs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", choices=sorted(MODULE_INDEX))
    ap.add_argument("--at", type=lambda s: int(s, 0))
    ap.add_argument("--span", type=lambda s: int(s, 0), default=0x60)
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--stubs", action="store_true")
    args = ap.parse_args()

    index = MODULE_INDEX[args.module]
    module_map = json.load(open(os.path.join(args.work, "30_modules", "module-map.json")))
    meta = module_map["modules"][args.module]
    text_end = meta["segments"][0]["size"]
    image = open(os.path.join(args.work, "20_rewritten", "%s.patched.bin" % args.module), "rb").read()

    relocs, symbols = load_tables(args.work, index)
    names = got_names(relocs, symbols)
    if args.stubs:
        stubs = plt_stubs(image, text_end, names)
        print("%s: %d relocations, %d symbols, %d resolvable GOT slots, %d PLT stubs"
              % (args.module, len(relocs), len(symbols), len(names), len(stubs)))
        for at in sorted(stubs):
            print("%08x -> %s" % (at, stubs[at][1] or "?"))
        return
    print("%s: %d relocations, %d symbols, %d resolvable GOT slots"
          % (args.module, len(relocs), len(symbols), len(names)))
    if args.at is None:
        return

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    start = max(0, args.at - args.span)
    cache = {}
    for insn in md.disasm(image[start:args.at + args.span], start):
        note = ""
        if insn.mnemonic in ("bl", "b") and insn.operands:
            target = insn.operands[0].imm
            if target not in cache:
                cache[target] = local_stub(image, target, names)
            stub = cache[target]
            if stub:
                note = "   ; " + (stub[1] or "GOT@%#x" % stub[0])
        print("%08x  %-8s %-28s%s" % (insn.address, insn.mnemonic, insn.op_str, note))


if __name__ == "__main__":
    main()
