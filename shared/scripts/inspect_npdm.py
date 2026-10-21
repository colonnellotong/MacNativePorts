#!/usr/bin/env python3
"""Read a game's NPDM and NSO headers: title ID, service permissions, filesystem access, build IDs.

Stage 0 of a port answers one question: what does this title ask the Switch for? The NPDM lists the
services it may open and the filesystem paths it may mount; the NSO headers give each module's build ID
and segment sizes. Nothing here is game-specific.

    python3 inspect_npdm.py <exefs dir>
"""
import json
import re
import struct
import sys
from pathlib import Path

CAPABILITY = {
    0x00: 'kernel_flags', 0x01: 'syscall_mask', 0x02: 'handle_table_size', 0x03: 'debug_flags',
    0x04: 'wakeup_value', 0x05: 'permitted_irq', 0x06: 'permitted_irq_pair',
    0x07: 'application_type', 0x08: 'min_kernel_version', 0x09: 'min_application_version',
    0x0A: 'unsupported_thread_info',
}

FS_ACCESS = ['None', 'RomFS', 'SaveData', 'Content', 'ContentRegister', 'ContentExport', 'Device',
             'ImageDirectory', 'ContentTemp', 'ContentSeed']


def sections(npdm):
    """ACI0 and ACID bodies, found by their magic (the header's own offsets are not trusted)."""
    aci, acid = npdm.find(b'ACI0'), npdm.find(b'ACID')
    if aci < 0 or acid < 0:
        raise ValueError('Not an NPDM (no ACI0/ACID)')
    return aci, acid


def aci_fields(npdm, aci):
    program_id, = struct.unpack_from('<Q', npdm, aci + 0x10)
    fs_off, fs_size, sac_off, sac_size, kc_off, kc_size = struct.unpack_from('<IIIIII', npdm, aci + 0x20)
    return program_id, (fs_off, fs_size), (sac_off, sac_size), (kc_off, kc_size)


def services(blob):
    """Service access control: [max sessions][name] repeated.

    Most NPDMs NUL-terminate each name. This one does not, so the names are recovered as the printable
    runs: every max-sessions byte is below 0x20 and therefore splits them on its own.
    """
    out = []
    for match in re.finditer(rb'[\x20-\x7e]+', blob):
        name = match.group().decode('ascii')
        limit = blob[match.start() - 1] if match.start() else 0
        out.append(dict(name=name, max_sessions=limit))
    return out


def fs_access(blob):
    """Filesystem access control: [type][path or NUL] repeated; types 1-3 carry a path."""
    out, i = [], 0
    while i < len(blob):
        kind = blob[i]
        i += 1
        if kind in (1, 2, 3):
            end = blob.find(b'\0', i)
            if end < 0:
                break
            out.append(dict(type=FS_ACCESS[kind] if kind < len(FS_ACCESS) else f'type_{kind}',
                            path=blob[i:end].decode('utf-8', 'replace')))
            i = end + 1
        elif kind in (4, 5):
            out.append(dict(type=FS_ACCESS[kind] if kind < len(FS_ACCESS) else f'type_{kind}', path=None))
            i += 16
        else:
            break
    return out


def kernel_caps(blob):
    out, i = [], 0
    while i + 4 <= len(blob):
        kind, length = blob[i], blob[i + 1]
        body = blob[i + 4:i + 4 + length]
        if length > len(blob) - i - 4:
            break
        name = CAPABILITY.get(kind, f'type_{kind:#x}')
        if kind == 0x0A:
            value = [struct.unpack_from('<I', body, j)[0] & 0x7f for j in range(0, len(body), 4)]
        elif len(body) == 4:
            value = struct.unpack('<I', body)[0]
        else:
            value = body.hex()
        out.append(dict(capability=name, value=value))
        i += 4 + length
    return out


def nso_header(path):
    raw = path.read_bytes()[:0x100]
    if raw[:4] != b'NSO0':
        return dict(name=path.name, error='not an NSO0')
    flags, = struct.unpack_from('<I', raw, 0xc)
    mod_offset, = struct.unpack_from('<I', raw, 0x4)
    segments = [struct.unpack_from('<III', raw, 0x10 + i * 0x10) for i in range(3)]
    build_id = raw[0x40:0x60]
    return dict(name=path.name, size=path.stat().st_size,
                uncompressed=sum(s[2] for s in segments),
                segments=[dict(file=s[0], memory=s[1], size=s[2], compressed=bool(flags >> i & 1))
                          for i, s in enumerate(segments)],
                build_id=build_id.hex(),
                module_object=mod_offset != 0)


def main():
    root = Path(sys.argv[1])
    npdm_path = root / 'main.npdm'
    npdm = npdm_path.read_bytes()
    aci, acid = sections(npdm)
    program_id, fs_span, sac_span, kc_span = aci_fields(npdm, aci)

    def block(offset, size):
        return npdm[aci + offset: aci + offset + size]

    report = dict(
        npdm=dict(path=str(npdm_path), size=len(npdm), aci0_offset=aci, acid_offset=acid),
        program_id=f'{program_id:016X}',
        service_permissions=[s['name'] for s in services(block(*sac_span))],
        service_max_sessions={s['name']: s['max_sessions'] for s in services(block(*sac_span))},
        filesystem_access=fs_access(block(*fs_span)),
        kernel_capabilities=kernel_caps(block(*kc_span)),
        modules=[nso_header(p) for p in sorted(root.iterdir())
                 if p.is_file() and p.name not in ('main.npdm',) and p.read_bytes()[:4] == b'NSO0'],
    )
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
