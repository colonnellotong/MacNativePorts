# Doraemon Story of Seasons native runtime.
#
# The Horizon host is a fork of the shared runtime (refs/by3-port/games/by3/runtime, which is itself
# that folder's fork of totk/runtime): the files were copied in once and this game's differences sit
# beside them — game_version.h, dora_fs_ext.c (the nn::fs surface Unity uses), dora_stubs.c (the two
# host services this game does not need) and dora_nvn_trace.c (the NVN API recorder).
#
# The NVN-on-Metal layer is compiled from the shared d2r_gfx unchanged, as by3 does: the per-engine
# adapter is the thin layer in front of it, not a replacement for it.
#
#   make prepare   # scripts/prepare_runtime.py: loader tables, RomFS index, fonts config
#   make runtime   # ../dora_build/build/dora-native
#   make boot      # run it (the work folder holds extracted/ and rewritten/)

BASE := ../../refs/by3-port/games
TOTK := $(BASE)/totk
DORA := ../..
WORK ?= $(DORA)/dora_build
OUT := $(WORK)/build
PY ?= python3
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard $(TOTK)/third_party/d2r_gfx/gfx/nvn/*.c $(TOTK)/third_party/d2r_gfx/gfx/nvn/*.m \
                        $(TOTK)/third_party/d2r_gfx/gfx/nvn/*.S $(TOTK)/third_party/d2r_gfx/gfx/tex/*.c \
                        $(TOTK)/third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst $(TOTK)/%,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard $(TOTK)/third_party/d2r_gfx/gfx/nvn/*.h $(TOTK)/third_party/d2r_gfx/gfx/nvn/include/*.h \
                          $(TOTK)/third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc \
             -I$(TOTK)/third_party/d2r_gfx/gfx/nvn/include

# opus_codec.c and movie_host.c stay out: this game's audio is CRIWARE ADX2 (decoded in the guest)
# and its video is Sofdec2, so libopus and libvpx are neither needed nor linked.
RUNTIME_SRC := $(filter-out runtime/opus_codec.c runtime/movie_host.c,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime boot clean

prepare:
	"$(PY)" scripts/prepare_runtime.py

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: $(TOTK)/% $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/dora-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Wno-deprecated-declarations \
	  -I$(TOTK)/third_party/d2r_gfx/gfx/nvn/include -I$(OUT) \
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
