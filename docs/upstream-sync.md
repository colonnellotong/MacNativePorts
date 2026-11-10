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

- Reviewed upstream through `6bc0ec8f9f62bca1b01bd7b714e7306b8523f06f` (2026-11-11).
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
- `426f1d79`: resolve eligible indirect-draw arguments on the GPU timeline and replay them through
  Metal indirect draws. The measurement-only Bayonetta 3 experiments from that commit were omitted.

Since the previous review, `games/by3` has no newer `d2r_gfx` change. Most upstream graphics work is
for Ocarina of Time (Switch 2), TOTK/BOTW features, shader contribution and game-specific presentation.
Those changes use different shader models, render-pass identities, UI and runtime assumptions, so they
are not part of Dora's shared layer.

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
