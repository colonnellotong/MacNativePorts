#!/usr/bin/env python3
"""Name the symbol that contains an offset in a rewritten module.

The crash handler can only print "sdk+0x317b30": it has no symbol table of its own,
because the guest images are loaded from rewritten binaries. This reads the .dynsym
that is still in the image and prints the enclosing function, which turns a crash
line into a name.

  scripts/lookup_symbol.py sdk 0x317b30 0x3187e4        # one or more offsets
  scripts/lookup_symbol.py main 0x419a9c --work ../dora_build
  scripts/lookup_symbol.py sdk --calls 0x317b30         # who calls into this address
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_WORK = os.path.join(HERE, "..", "..", "..", "dora_build")


def load(work, name):
    module_map = json.load(open(os.path.join(work, "30_modules", "module-map.json")))
    meta = module_map["modules"][name]
    path = os.path.join(work, "20_rewritten", "%s.patched.bin" % name)
    return open(path, "rb").read(), meta


def dynamic(image, offset):
    tags = {}
    p = offset
    while True:
        tag, value = struct.unpack_from("<QQ", image, p)
        if tag == 0:
            break
        tags[tag] = value
        p += 16
    return tags


def symbol_count(image, tags):
    """The runtime sizes .dynsym from DT_HASH.nchain (prepare_runtime.py does the same).
    Guessing from the string table instead stops early whenever a module's dynsym is not
    the last thing before the hash tables, which is why main used to resolve to only 49 names."""
    if 4 in tags:
        return struct.unpack_from("<I", image, tags[4] + 4)[0]
    return (tags[10] - tags[6]) // tags[11]


def symbols(image, tags, functions_only):
    symtab, strtab = tags[6], tags[5]
    syment, strsz = tags[11], tags[10]
    out = []
    i = 1
    while i < symbol_count(image, tags):
        nameoff, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", image, symtab + i * syment)
        if nameoff >= strsz:
            break
        end = image.index(b"\0", strtab + nameoff)
        name = image[strtab + nameoff:end].decode("utf-8", "replace")
        kind = info & 0xF
        if not functions_only or (kind == 2 and size):
            out.append((value, size, kind, shndx, info >> 4, name))
        i += 1
    out.sort()
    return out


def owner(syms, address):
    best = None
    for value, size, kind, shndx, binding, name in syms:
        if value <= address < value + size:
            best = (value, size, kind, shndx, binding, name)
        elif value > address:
            break
    return best


def callers(image, text_end, target, syms):
    try:
        import capstone
        from capstone.arm64 import ARM64_OP_IMM
    except ImportError:
        sys.exit("capstone is needed for --calls: pip install capstone")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    hits = []
    for insn in md.disasm(image[:text_end], 0):
        if insn.mnemonic not in ("bl", "b"):
            continue
        if not insn.operands or insn.operands[0].type != ARM64_OP_IMM:
            continue
        if insn.operands[0].imm in target:
            hits.append((insn.address, insn.mnemonic, insn.operands[0].imm))
    for where, mnemonic, to in hits:
        sym = owner(syms, where)
        print("%08x  %-2s -> %08x  %s" % (where, mnemonic, to, sym[5] if sym else "?"))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module")
    ap.add_argument("offsets", nargs="*", type=lambda s: int(s, 0))
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--calls", action="store_true", help="list call sites of the given addresses instead")
    ap.add_argument("--all", action="store_true", help="include object symbols, not just functions")
    args = ap.parse_args()

    image, meta = load(args.work, args.module)
    tags = dynamic(image, meta["dynamic_offset"])
    syms = symbols(image, tags, not args.all)
    print("%s: %d symbols, %d functions" % (args.module, len(syms), len(syms)))

    if args.calls:
        callers(image, meta["text_end"], set(args.offsets), syms)
        return
    for off in args.offsets:
        found = owner(syms, off)
        if not found:
            print("%08x  (no symbol)" % off)
            continue
        value, size, kind, shndx, binding, name = found
        print("%08x  %s +0x%x  (size %d, %s)" % (off, name, off - value, size, "defined" if shndx else "imported"))


if __name__ == "__main__":
    main()
