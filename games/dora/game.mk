# dora — Doraemon Story of Seasons: Friends of the Great Kingdom
#
# This file declares only what is different about this game. The build reads it after
# shared/runtime, and every file listed in a game's runtime/ directory replaces the file of
# the same name in shared/runtime. Nothing here is a copy of the shared host.

GAME_TITLE    := Doraemon Story of Seasons: Friends of the Great Kingdom
GAME_TITLE_ID := 0100F5E0177AC000

# Two files of the shared host are not compiled for this game:
# its audio is CRIWARE ADX2, decoded inside the guest, so libopus is never reached;
# its one movie is Sofdec2, and the video host is not wired up yet.
EXCLUDE_SRC := $(SHARED)/runtime/opus_codec.c $(SHARED)/runtime/movie_host.c

# Where this game's own extracted and rewritten data live.
GAME_WORK ?= $(CURDIR)/dora_build
