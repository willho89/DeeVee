# DeeVee — libretro DVD-Video core
#
#   make                       build deps (first time) + core, DEEVEE_CSS=static
#   make DEEVEE_CSS=dlopen     libdvdread dlopen()s a user-supplied libdvdcss
#   make DEEVEE_CSS=none       no CSS (CI / "missing library" path)
#   make DEBUG=1               -O0 -g core build
#
# See CLAUDE.md §6 and decision D-003.

.DEFAULT_GOAL := all

TARGET_NAME   := deevee
DEEVEE_CSS    ?= static
BUILD_DIR     ?= build
DEBUG         ?= 0

ifeq ($(filter $(DEEVEE_CSS),static dlopen none),)
  $(error DEEVEE_CSS must be one of: static dlopen none (got '$(DEEVEE_CSS)'))
endif

UNAME := $(shell uname -s)
ifeq ($(OS),Windows_NT)
  platform ?= win
else ifeq ($(UNAME),Darwin)
  platform ?= osx
else
  platform ?= unix
endif

ifeq ($(platform),unix)
  TARGET  := $(TARGET_NAME)_libretro.so
  SHARED  := -shared -Wl,--version-script=link.T -Wl,--no-undefined
else ifeq ($(platform),osx)
  TARGET  := $(TARGET_NAME)_libretro.dylib
  SHARED  := -dynamiclib
else ifeq ($(platform),win)
  TARGET  := $(TARGET_NAME)_libretro.dll
  SHARED  := -shared -static-libgcc -Wl,--version-script=link.T
endif

include Makefile.deps
include Makefile.common

PKG_CONFIG ?= pkg-config
PKGS       := dvdnav dvdread libavcodec libswscale libswresample libavutil
ifeq ($(DEEVEE_CSS),static)
  PKGS += libdvdcss
endif
# Evaluated lazily: the .pc files only exist once deps are built.
DEPS_CFLAGS = $(shell $(DEPS_ENV) $(PKG_CONFIG) --cflags $(PKGS))
DEPS_LIBS   = $(shell $(DEPS_ENV) $(PKG_CONFIG) --static --libs $(PKGS))

CSS_DEFINE := -DDEEVEE_CSS_MODE=\"$(DEEVEE_CSS)\"

ifeq ($(DEBUG),1)
  OPTFLAGS := -O0 -g
else
  OPTFLAGS := -O2 -g
endif

CFLAGS   += -std=c99 -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden $(OPTFLAGS)
CPPFLAGS += $(INCFLAGS) $(CSS_DEFINE) -D_POSIX_C_SOURCE=200809L
LDFLAGS  += $(SHARED)
LIBS     := -lm

OBJ_DIR := $(BUILD_DIR)/obj-css-$(DEEVEE_CSS)
# Upstream unity.c omits libchdr_codec_avhuff.c (referenced by chd.c).
LIBCHDR_OBJS := $(OBJ_DIR)/libchdr/unity.o $(OBJ_DIR)/libchdr/codec_avhuff.o
OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES_C)) $(LIBCHDR_OBJS)

all: $(TARGET)

$(TARGET): $(DEPS_STAMPS) $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS) $(DEPS_LIBS) $(LIBS)

# Objects depend on the deps being installed (headers). Order-only so a dep
# rebuild doesn't force a full core rebuild; -MMD tracks header changes.
$(OBJ_DIR)/libchdr/unity.o: $(LIBCHDR_DIR)/unity.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBCHDR_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/libchdr/codec_avhuff.o: $(LIBCHDR_DIR)/src/libchdr_codec_avhuff.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBCHDR_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: %.c | $(DEPS_STAMPS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(DEPS_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

distclean: clean
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)

.PHONY: all clean distclean

# ---- tests -----------------------------------------------------------------
HARNESS     := $(BUILD_DIR)/harness
FIXTURE_DIR := $(BUILD_DIR)/fixtures

$(HARNESS): tests/harness.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g $(INCFLAGS) -o $@ $< -ldl

harness: $(HARNESS)

FIXTURES := $(addprefix $(FIXTURE_DIR)/,ntsc43.iso pal169.iso avs_ac3.iso avs_lpcm.iso avs_mp2.iso avs_dts.iso subs.iso region2.iso)

$(FIXTURES) &: tests/fixtures/make_fixture.sh tests/fixtures/make_spu_png.py
	tests/fixtures/make_fixture.sh $(FIXTURE_DIR)

# DVD CHDs of selected fixtures (chdman from mame-tools).
CHD_FIXTURES := $(addprefix $(FIXTURE_DIR)/,avs_ac3.chd subs.chd)

$(FIXTURE_DIR)/%.chd: $(FIXTURE_DIR)/%.iso
	chdman createdvd -f -i $< -o $@ >/dev/null

fixture: $(FIXTURES) $(CHD_FIXTURES)

# M0 smoke test: ISO (stream callbacks) and VIDEO_TS folder (path open).
check: $(TARGET) $(HARNESS) $(FIXTURES) $(CHD_FIXTURES)
	@mkdir -p $(BUILD_DIR)/system
	$(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43.iso 600 $(BUILD_DIR)/system
	$(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43/VIDEO_TS/VIDEO_TS.IFO 600 $(BUILD_DIR)/system
	$(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/pal169.iso 600 $(BUILD_DIR)/system
	HARNESS_AVSYNC=1 $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/avs_ac3.iso 1500 $(BUILD_DIR)/system
	HARNESS_AVSYNC=1 $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/avs_lpcm.iso 1500 $(BUILD_DIR)/system
	HARNESS_OPT_deevee_subtitles=on $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/subs.iso 650 $(BUILD_DIR)/system
	@echo "== CHD must match ISO (video hash, audio, A/V offset)"
	@for f in avs_ac3 subs; do \
	  a=$$(HARNESS_AVSYNC=1 $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/$$f.iso 900 $(BUILD_DIR)/system 2>/dev/null | grep -E '^frames:|offset ms'); \
	  b=$$(HARNESS_AVSYNC=1 $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/$$f.chd 900 $(BUILD_DIR)/system 2>/dev/null | grep -E '^frames:|offset ms'); \
	  if [ "$$a" = "$$b" ]; then echo "   $$f: CHD == ISO"; else echo "   $$f: CHD != ISO"; exit 1; fi; done
	@echo "== Region: auto passes the disc's region check, forced region 1 fails it"
	@HARNESS_DEBUG=1 $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/region2.iso 60 $(BUILD_DIR)/system 2>&1 | grep -q "cell change: title 1 " && echo "   auto -> title 1 (ok)"
	@HARNESS_DEBUG=1 HARNESS_OPT_deevee_region=1 $(HARNESS) ./$(TARGET) $(FIXTURE_DIR)/region2.iso 60 $(BUILD_DIR)/system 2>&1 | grep -q "cell change: title 2 " && echo "   forced 1 -> title 2 (ok)"

.PHONY: harness fixture check
