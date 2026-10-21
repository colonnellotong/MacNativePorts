# MacNativePorts — native runtimes for Nintendo Switch games on Apple silicon.
#
# Layout, and why it is split this way:
#
#   shared/runtime/   the Horizon OS host: services, FS, threads, audio, input, the loader.
#                     Answered by macOS instead of by the console. Shared by every game.
#   shared/gfx/       NVN on Metal — the command recorder, the Metal back end, texture and
#                     shader plumbing. Derived from d2r_gfx (MIT, see LICENSE.txt beside it).
#   games/<code>/     everything specific to one game: its runtime/ holds only the files that
#                     differ from the shared host, plus a few game_*.c of its own.
#
# A game's runtime/ is an overlay, not a copy. Same-named files replace the shared one;
# everything else comes from shared/runtime. That is the whole point of the layout: adding a
# game means writing its differences, not forking the host.
#
#   make prepare GAME=dora   # loader tables, RomFS index, font config from this game's data
#   make runtime GAME=dora   # build/dora-native
#   make boot    GAME=dora   # run it
#
# GAME defaults to dora. GAME_WORK (set in games/<code>/game.mk) points at that game's
# extracted and rewritten data.

GAME ?= dora
SHARED := shared
GAME_DIR := games/$(GAME)

-include $(GAME_DIR)/game.mk

WORK ?= $(GAME_WORK)
OUT ?= $(WORK)/build
PY ?= python3
NATIVE_OPT ?= -O2

# ---- sources -----------------------------------------------------------------------------

GAME_SRC := $(wildcard $(GAME_DIR)/runtime/*.c $(GAME_DIR)/runtime/*.m $(GAME_DIR)/runtime/*.S)
GAME_NAMES := $(notdir $(GAME_SRC))
SHARED_ALL_SRC := $(wildcard $(SHARED)/runtime/*.c $(SHARED)/runtime/*.m $(SHARED)/runtime/*.S)
SHARED_SRC := $(filter-out $(addprefix $(SHARED)/runtime/,$(GAME_NAMES)), $(SHARED_ALL_SRC))
RUNTIME_SRC := $(filter-out $(EXCLUDE_SRC), $(GAME_SRC) $(SHARED_SRC))
RUNTIME_HEADERS := $(wildcard $(GAME_DIR)/runtime/*.h) $(wildcard $(SHARED)/runtime/*.h)

GFX_SRC := $(wildcard $(SHARED)/gfx/nvn/*.c $(SHARED)/gfx/nvn/*.m $(SHARED)/gfx/nvn/*.S \
                     $(SHARED)/gfx/tex/*.c $(SHARED)/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst $(SHARED)/%,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard $(SHARED)/gfx/nvn/*.h $(SHARED)/gfx/nvn/include/*.h $(SHARED)/gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc \
             -I$(SHARED)/gfx/nvn/include

# The game's headers come first: a game may add its own headers beside the shared ones.
# runtime.h itself is shared only — two copies would collide under #pragma once.
INCLUDES := -I$(GAME_DIR)/runtime -I$(SHARED)/runtime -I$(SHARED)/gfx -I$(SHARED)/gfx/nvn/include -I$(OUT)

GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime boot clean list

list:
	@echo "$(GAME): $(words $(GAME_SRC)) files of its own, replacing $(words $(filter $(addprefix $(SHARED)/runtime/,$(GAME_NAMES)),$(SHARED_ALL_SRC))) of $(words $(SHARED_ALL_SRC)) shared ones"
	@echo "compiled: $(words $(GAME_SRC)) game + $(words $(SHARED_SRC)) shared = $(words $(RUNTIME_SRC))"

prepare:
	"$(PY)" $(GAME_DIR)/scripts/prepare_runtime.py

$(GENERATED):
	@echo "missing $@: run make prepare GAME=$(GAME)" && false

$(OUT)/obj/%.o: $(SHARED)/% $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/dora-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Wno-deprecated-declarations \
	  $(INCLUDES) \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO \
	  -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc \
	  -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox \
	  -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

runtime: $(OUT)/dora-native

DORA_LOG ?= $(WORK)/logs/boot.log
boot: runtime
	@mkdir -p "$(dir $(DORA_LOG))"
	cd "$(WORK)" && ./build/dora-native rewritten/v0 boot 2> "$(abspath $(DORA_LOG))"; echo "exit $$?; log: $(DORA_LOG)"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/dora-native"
