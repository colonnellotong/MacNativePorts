# MacNativeGames upstream sync

MacNativeGames is tracked as the `upstream` Git remote:

```text
https://github.com/ly05010419/MacNativeGames.git
```

MacNativePorts is not laid out as a fork of that repository. Its `shared/runtime` began with
`games/by3/runtime`, while `shared/gfx` began with the By3 copy of `d2r_gfx`; game-specific files are
overlays under `games/<code>/runtime`. Upstream updates therefore need to be reviewed and ported by
behaviour rather than merged by path.

## Last review

- Reviewed upstream through `9fe3fdc1d08ce5e1a64d120f257d79cc2dc7c0ca` (2026-11-08).
- The old upstream `by3-port` branch has been merged into `main` and removed. Use `upstream/main`.
- The By3 runtime itself has no material change needed by Dora since this repository was created.

The following cross-game fixes from the newer TOTK host were ported:

- `3c482a7`: correct NVN sizes for cube arrays, linear block-compressed textures and packaged 3D
  block depth.
- `a166602`: close the dirty-page generation/protection races and take texture upload stamps before
  protection. The TOTK glyph-cache edge-page optimization was left out because it is a performance
  specialization rather than part of the race fix.
- `f864eb0`: clamp invalid guest texture descriptors before Metal validates them and aborts.
- `dab728a`: match Horizon kernel synchronization for `SignalToAddress`, SystemEvent rebinding and
  targeted synchronization wakeups.
- `9505582`: keep a dirty-page range alive while its pages are being protected, closing a race with
  memory-pool retirement.
- `0dc98066`: retire visibility overflow buffers without evicting the reusable ring buffers from
  Metal residency.

The newer By3 GPU-timeline indirect-draw path (`426f1d79`) is intentionally not enabled for Dora.
It fixes Bayonetta 3's per-object visibility-driven indirect records; Dora has not shown that record
shape, and the implementation carries a large By3-specific look-ahead and compute pass.

TOTK presentation features such as 240 fps timing, MetalFX modes, ultrawide output, pipeline archives,
shader seed lists and TOTK render-pass recognition are intentionally not copied into the shared layer.
They depend on TOTK-specific shaders, serials, render targets or UI behaviour. A new game may adopt an
idea from them in its own overlay after measuring that game's behaviour.

## Updating

1. Fetch `upstream/main` and record its new commit hash above.
2. Review changes first in `games/by3`, then cross-game correctness fixes in `games/totk/runtime` and
   `games/totk/third_party/d2r_gfx/gfx`.
3. Port the smallest coherent behaviour into `shared`; keep game identifiers and measured constants in
   a game overlay.
4. Run the upstream CPU probe associated with a fix, compile the Dora overlay, and launch the game when
   its private work directory is available.
5. Record imported and intentionally skipped commits in this file.
