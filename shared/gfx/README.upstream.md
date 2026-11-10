# Experimental NVN/Metal adapter

Copied gfx/nvn, gfx/tex and bcdec from D2R Mobile commit
9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf, retaining MIT/embedded licenses.
Dora's shared fork also carries selected cross-game correctness fixes reviewed from
MacNativeGames through 6bc0ec8f9f62bca1b01bd7b714e7306b8523f06f; see
`../../docs/upstream-sync.md` and `../../patches/README.md`.
D2R shader/PSO TSV files were deliberately excluded: they cannot identify TOTK
shaders. This copy is an integration experiment; no game frame is verified.

Local host headers bind the native TOTK runtime. Unknown NVN calls stop instead
of returning success. TOTK needs its own shader identity and reflection mapping.

Local changes: fail on unimplemented calls/properties, separate TOTK shader cache
and control+code map, remove D2R built-in compute kernels, target the game's 55.15
ABI check, fit textures into the game's verified 128-byte allocation, add actual
reached buffer/swapchain APIs. The target version does not imply full API coverage.
Metal 4 protocol warnings and unverified optimizations still require review before
rendering claims. nvn_tex.c adapts deko3d; its original notice is in LICENSE.deko3d.
