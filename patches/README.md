# Changes to upstream d2r_gfx

`shared/gfx/` is a fork of `d2r_gfx` from MacNativeGames' `by3-port` branch. The changes below are
already applied in the source here; this file records what they are and why, so a future upstream
sync does not silently drop them.

A generated diff is not shipped: the two upstream snapshots available locally (the `by3-port` copy
and the plain `MacNativeGames` copy) differ from each other by several hundred lines, so a patch
against either would not apply to the other, and neither is a stable base.

## 1. Multisample resolve in `copy_t2t()`

**File:** `shared/gfx/nvn/nvn_mtl.m`
**Symptom without it:** the whole screen is a flat purple, audio plays fine, and Metal rejects every
submission from the first frame on:

```
MTL4DebugComputeCommandEncoder copyFromTexture:... failed assertion
[sourceTexture sampleCount](1) and [destinationTexture sampleCount](2) must be equal.
```

then `MTL4CommandQueueErrorDomain error 1` for the rest of the run — the driver ignores all further
work, so nothing is ever drawn.

**Why it happens:** Unity renders this game at 2× multisample and resolves the MSAA target into the
presented texture at the end of every frame. NVN expresses that as `nvnCommandBufferCopyTextureToTexture`
between two textures that differ only in sample count. Metal's blit requires both ends to match.

**What was added:** `copy_t2t_resolve()` — a render pass that does nothing but resolve. The
multisample texture is bound as colour attachment 0 with `MTLLoadActionLoad` and
`MTLStoreActionStoreAndMultisampleResolve`, and the resolved image lands in the single-sample
texture through `resolveTexture`. The store action keeps the multisample texture populated too,
because the game goes on sampling it afterwards.

## 2. Filtered scaling in `copy_t2t()`

**File:** `shared/gfx/nvn/nvn_mtl.m`
**Needed by:** `nvnCommandBufferDownsample`, which the game calls once per frame at 1920×1080 → 1920×1080.

A `blitCommandEncoder` copy is only defined for equal sizes, so any size change is dispatched as a
compute kernel instead: `k_t2s_msl`, one thread per destination pixel, sampling the source with a
linear sampler at `(g + 0.5) / size`. `copy_t2t_scale()` builds the pipeline, binds 2D views for the
chosen level and slice of each texture, and flushes the render pass before dispatching.

Guarded to the case it can express: both ends 2D, both offsets zero, same pixel format, and no copy
flags other than `ENGINE_2D` / `LINEAR_FILTER`. Anything else still reports red instead of drawing
something wrong.

## 3. Sample-count diagnostic

**File:** `shared/gfx/nvn/nvn_mtl.m`, start of `copy_t2t()`

When the two ends disagree on sample count, the serials, sizes, formats and flags of both are
logged, once each. This is what identified the 1 → 2 copy above; it costs nothing when no such copy
happens and stays useful for the next game.
