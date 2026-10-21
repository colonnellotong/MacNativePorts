# Third-party notices

MacNativePorts contains no game content. What it does contain is derived from the following
projects, each under its own licence. The full texts sit beside the code they cover.

## MacNativeGames

`shared/runtime/`, `shared/gfx/` (as a fork of d2r_gfx), the rewrite tooling and the prebuilt
shader translator all originate in [MacNativeGames](https://github.com/ly05010419/MacNativeGames),
taken from its `by3-port` branch.

> MIT License — Copyright (c) 2026 MacNativeGames contributors

Full text: `shared/runtime/LICENSE.runtime.txt`.

## d2r_gfx

`shared/gfx/` is a fork of the NVN-on-Metal layer from MacNativeGames. This project's changes to it
are listed in `patches/` and applied on top of the upstream files.

> MIT License — Copyright (c) 2026 jhaizhou

Full text: `shared/gfx/LICENSE.txt`.

## deko3d

`d2r_gfx` builds on deko3d for its NVN surface.

> Copyright (C) 2018-2020 fincs — zlib-style licence

Full text: `shared/gfx/LICENSE.deko3d.txt`.

## bcdec

Block-compression decoding, used by `shared/gfx/tex/nvn_bcdec.c`.

> Sergii "iOrange" Kudlai, 2022 — dual licensed MIT / public domain, use either

Full text: `shared/gfx/third_party/bcdec/LICENSE`.

## SPIRV-Cross

Shaders are translated from SPIR-V to MSL with
[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross).

> Apache License 2.0 — Copyright (c) 2015-2026 The Khronos Group Inc.

SPIRV-Cross is built from source at build time and is not vendored here.

## Game trademarks

Every game named in this repository — including *Doraemon Story of Seasons: Friends of the Great
Kingdom* — remains the property of its publisher. Nintendo Switch is a trademark of Nintendo. This
project is not affiliated with, endorsed by, or sponsored by any of them.
