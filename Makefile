# DeeVee — libretro DVD-Video core
#
#   make                       build deps (first time) + core, DEEVEE_CSS=static
#   make DEEVEE_CSS=dlopen     libdvdread dlopen()s a user-supplied libdvdcss
#   make DEEVEE_CSS=none       no CSS (CI / "missing library" path)
#   make DEBUG=1               -O0 -g core build
#   make platform=win          Windows x86_64 DLL; on Linux cross-compiles with
#                              MinGW-w64 (CROSS=x86_64-w64-mingw32-), tests
#                              run under Wine; build dir build/win64
#   make platform=linux-arm64  Linux aarch64 / armhf .so, cross-compiled
#   make platform=linux-armhf  (aarch64-linux-gnu- / arm-linux-gnueabihf-),
#                              tests run under qemu-user
#   make platform=android-arm64  Android arm64-v8a / armeabi-v7a
#   make platform=android-armv7  (NDK=~/android-ndk/android-ndk-r27c, API 21);
#                              build + link only (no Android runtime here)
#
# See CLAUDE.md §6 and decision D-003.

.DEFAULT_GOAL := all

TARGET_NAME   := deevee
DEEVEE_CSS    ?= static
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
  BUILD_DIR ?= build
  HOST_LIBS := -ldl
else ifeq ($(platform),osx)
  TARGET  := $(TARGET_NAME)_libretro.dylib
  SHARED  := -dynamiclib
  BUILD_DIR ?= build/osx
else ifeq ($(platform),win)
  TARGET  := $(TARGET_NAME)_libretro.dll
  # Fully static (libgcc, winpthreads): the DLL needs only Windows' own DLLs.
  SHARED  := -shared -static -static-libgcc -Wl,--version-script=link.T -Wl,--no-undefined
  BUILD_DIR ?= build/win64
  EXE     := .exe
  ifneq ($(OS),Windows_NT)
    # Cross from Linux. The -posix toolchain has the pthreads libdvdnav uses.
    CROSS   ?= x86_64-w64-mingw32-
    CC      := $(CROSS)gcc-posix
    AR      := $(CROSS)ar
    RUN     ?= WINEDEBUG=-all wine
    CROSS_SYSTEM := windows
    CROSS_CPU    := x86_64
    FFMPEG_TARGET := --enable-cross-compile --cross-prefix=$(CROSS) --cc=$(CC) \
                     --target-os=mingw32 --arch=x86_64
  endif
else ifneq ($(filter linux-arm64 linux-armhf,$(platform)),)
  BUILD_DIR ?= build/$(platform)
  # Same file name as the native core: keep it in the build dir so it never
  # replaces the host's deevee_libretro.so.
  TARGET  := $(BUILD_DIR)/$(TARGET_NAME)_libretro.so
  SHARED  := -shared -Wl,--version-script=link.T -Wl,--no-undefined
  HOST_LIBS := -ldl
  ifeq ($(platform),linux-arm64)
    CROSS        ?= aarch64-linux-gnu-
    CROSS_CPU    := aarch64
    FFMPEG_ARCH  := aarch64
    RUN          ?= qemu-aarch64 -L /usr/aarch64-linux-gnu
  else
    CROSS        ?= arm-linux-gnueabihf-
    CROSS_CPU    := arm
    FFMPEG_ARCH  := arm
    RUN          ?= qemu-arm -L /usr/arm-linux-gnueabihf
    # GCC 7.1 ABI note on packed bitfields (libdvdread's nav_types.h): every
    # object here is built by the same compiler, so it doesn't apply.
    HOST_CFLAGS  := -Wno-psabi
  endif
  CC      := $(CROSS)gcc
  AR      := $(CROSS)ar
  # Keep the oldest symbol versions for distros older than the build's
  # glibc (see src/dv_glibc_compat.c).
  EXTRA_SOURCES := src/dv_glibc_compat.c
  SHARED  += -Wl,--wrap=fmod
  CROSS_SYSTEM  := linux
  FFMPEG_TARGET := --enable-cross-compile --cross-prefix=$(CROSS) --cc=$(CC) \
                   --target-os=linux --arch=$(FFMPEG_ARCH)
else ifneq ($(filter android-arm64 android-armv7,$(platform)),)
  NDK       ?= $(HOME)/android-ndk/android-ndk-r27c
  NDK_BIN   := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
  BUILD_DIR ?= build/$(platform)
  TARGET    := $(BUILD_DIR)/$(TARGET_NAME)_libretro_android.so
  # --as-needed: FFmpeg's configure always adds -landroid -lmediandk on
  # Android; nothing we build uses them.
  SHARED    := -shared -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--as-needed
  ifeq ($(platform),android-arm64)
    CC          := $(NDK_BIN)/aarch64-linux-android21-clang
    CROSS_CPU   := aarch64
    FFMPEG_ARCH := --arch=aarch64
  else
    CC          := $(NDK_BIN)/armv7a-linux-androideabi21-clang
    CROSS_CPU   := arm
    FFMPEG_ARCH := --arch=arm --cpu=armv7-a
  endif
  AR      := $(NDK_BIN)/llvm-ar
  STRIP   := $(NDK_BIN)/llvm-strip
  CROSS_SYSTEM  := android
  # No MediaCodec/JNI: DeeVee decodes in software; keeps libmediandk out.
  FFMPEG_TARGET := --enable-cross-compile --target-os=android $(FFMPEG_ARCH) --cc=$(CC) \
                   --disable-mediacodec --disable-jni \
                   --ar=$(AR) --nm=$(NDK_BIN)/llvm-nm --ranlib=$(NDK_BIN)/llvm-ranlib \
                   --strip=$(STRIP)
else
  $(error unknown platform '$(platform)' (unix, osx, win, linux-arm64, linux-armhf, android-arm64, android-armv7))
endif

include Makefile.deps
include Makefile.common

PKG_CONFIG ?= pkg-config
PKGS       := dvdnav dvdread libavfilter libavcodec libswscale libswresample libavutil
ifeq ($(DEEVEE_CSS),static)
  PKGS += libdvdcss
endif
# Evaluated lazily: the .pc files only exist once deps are built.
DEPS_CFLAGS = $(shell $(DEPS_ENV) $(PKG_CONFIG) --cflags $(PKGS))
DEPS_LIBS   = $(shell $(DEPS_ENV) $(PKG_CONFIG) --static --libs $(PKGS))

CSS_DEFINE := -DDEEVEE_CSS_MODE=\"$(DEEVEE_CSS)\"
ifeq ($(DEEVEE_CSS),static)
  CSS_DEFINE += -DDEEVEE_CSS_STATIC
endif

ifeq ($(DEBUG),1)
  OPTFLAGS := -O0 -g
else
  OPTFLAGS := -O2 -g
endif

CFLAGS   += -std=c99 -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden $(OPTFLAGS) $(HOST_CFLAGS)
CPPFLAGS += $(INCFLAGS) $(CSS_DEFINE) -D_POSIX_C_SOURCE=200809L
LDFLAGS  += $(SHARED)
LIBS     := -lm

OBJ_DIR := $(BUILD_DIR)/obj-css-$(DEEVEE_CSS)
# Upstream unity.c omits libchdr_codec_avhuff.c (referenced by chd.c).
LIBCHDR_OBJS := $(OBJ_DIR)/libchdr/unity.o $(OBJ_DIR)/libchdr/codec_avhuff.o
OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES_C) $(EXTRA_SOURCES)) $(LIBCHDR_OBJS)

all: $(TARGET)

# Every DEEVEE_CSS mode links to the same file name: relink when the mode
# changes, even if the target is newer than that mode's objects.
CSS_STAMP := $(BUILD_DIR)/.css-mode
$(shell mkdir -p $(BUILD_DIR); [ "$$(cat $(CSS_STAMP) 2>/dev/null)" = "$(DEEVEE_CSS)" ] || echo $(DEEVEE_CSS) > $(CSS_STAMP))

$(TARGET): $(DEPS_STAMPS) $(OBJECTS) $(CSS_STAMP)
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
HARNESS     := $(BUILD_DIR)/harness$(EXE)
RUN_HARNESS := $(RUN) $(HARNESS)
# Fixture discs are platform independent: one set for every build.
FIXTURE_DIR := build/fixtures

$(HARNESS): tests/harness.c
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g $(INCFLAGS) -o $@ $< $(HOST_LIBS)

harness: $(HARNESS)

FIXTURES := $(addprefix $(FIXTURE_DIR)/,ntsc43.iso pal169.iso avs_ac3.iso avs_lpcm.iso avs_mp2.iso avs_dts.iso subs.iso region2.iso)

$(FIXTURES) &: tests/fixtures/make_fixture.sh tests/fixtures/make_spu_png.py
	tests/fixtures/make_fixture.sh $(FIXTURE_DIR)

# DVD CHDs of selected fixtures (chdman from mame-tools).
CHD_FIXTURES := $(addprefix $(FIXTURE_DIR)/,avs_ac3.chd subs.chd)

$(FIXTURE_DIR)/%.chd: $(FIXTURE_DIR)/%.iso
	chdman createdvd -f -i $< -o $@ >/dev/null

$(FIXTURE_DIR)/scrambled.iso: $(FIXTURE_DIR)/ntsc43.iso tests/fixtures/scramble.py
	python3 tests/fixtures/scramble.py $< $@

fixture: $(FIXTURES) $(CHD_FIXTURES) $(FIXTURE_DIR)/scrambled.iso

# M0 smoke test: ISO (stream callbacks) and VIDEO_TS folder (path open).
check: $(TARGET) $(HARNESS) $(FIXTURES) $(CHD_FIXTURES) $(FIXTURE_DIR)/scrambled.iso
	@mkdir -p $(BUILD_DIR)/system
	$(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43.iso 600 $(BUILD_DIR)/system
	$(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43/VIDEO_TS/VIDEO_TS.IFO 600 $(BUILD_DIR)/system
	$(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/pal169.iso 600 $(BUILD_DIR)/system
	HARNESS_AVSYNC=1 HARNESS_OPT_deevee_deinterlace=off $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/avs_ac3.iso 1500 $(BUILD_DIR)/system
	HARNESS_AVSYNC=1 HARNESS_OPT_deevee_deinterlace=off $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/avs_lpcm.iso 1500 $(BUILD_DIR)/system
	HARNESS_OPT_deevee_subtitles=on $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/subs.iso 650 $(BUILD_DIR)/system
	@echo "== CHD must match ISO (video hash, audio, A/V offset)"
	@for f in avs_ac3 subs; do \
	  a=$$(HARNESS_AVSYNC=1 $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/$$f.iso 900 $(BUILD_DIR)/system 2>/dev/null | grep -E '^frames:|offset ms'); \
	  b=$$(HARNESS_AVSYNC=1 $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/$$f.chd 900 $(BUILD_DIR)/system 2>/dev/null | grep -E '^frames:|offset ms'); \
	  if [ "$$a" = "$$b" ]; then echo "   $$f: CHD == ISO"; else echo "   $$f: CHD != ISO"; exit 1; fi; done
	@echo "== TV Display Type: 16:9 picture -> 4:3 output on a 4:3 TV"
	@for m in 16:9:1.7778 4:3_letterbox:1.3333 4:3_panscan:1.3333; do \
	  opt=$${m%:*}; want=$${m##*:}; \
	  HARNESS_OPT_deevee_tv_display=$$opt $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/pal169.iso 120 $(BUILD_DIR)/system 2>/dev/null \
	    | grep -q "^av: 720x576 .*aspect $$want" && echo "   $$opt -> $$want (ok)" || { echo "   $$opt: expected aspect $$want"; exit 1; }; done
	@echo "== Encrypted data that can't be decrypted: alert once, halt (no End of Disc loop)"
	@out=$$(HARNESS_DEBUG=1 $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/scrambled.iso 300 $(BUILD_DIR)/system 2>&1); \
	  echo "$$out" | grep -q "OSD: This disc is encrypted" && ! echo "$$out" | grep -q "End of disc" \
	  && echo "$$out" | grep -qE "scrambled 1[^0-9]*$$" && echo "   scrambled.iso: alert + halt (ok)" \
	  || { echo "   scrambled.iso: FAILED"; exit 1; }
	@echo "== Save state: resume point in a new session lands on the saved picture; Restart re-runs First Play"
	@HARNESS_STATE=$(BUILD_DIR)/check.state HARNESS_SAVE=400 HARNESS_DUMP=$(BUILD_DIR)/state-a HARNESS_DUMP_FRAMES=520 \
	  $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43.iso 521 $(BUILD_DIR)/system 2>/dev/null | grep -q "serialize at frame 400: ok" \
	  && out=$$(HARNESS_DEBUG=1 HARNESS_STATE=$(BUILD_DIR)/check.state HARNESS_LOAD=0 \
	    $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43.iso 200 $(BUILD_DIR)/system 2>&1) \
	  && echo "$$out" | grep -q "unserialize at frame 0: ok" && echo "$$out" | grep -q "Resumed from save state" \
	  && echo "   save/load (ok)" || { echo "   save/load: FAILED"; exit 1; }
	@out=$$(HARNESS_DEBUG=1 HARNESS_RESET=300 $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/ntsc43.iso 330 $(BUILD_DIR)/system 2>&1); \
	  echo "$$out" | grep -q "OSD: Restarting disc" && echo "   reset (ok)" || { echo "   reset: FAILED"; exit 1; }
	@echo "== Region: auto passes the disc's region check, forced region 1 fails it"
	@HARNESS_DEBUG=1 $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/region2.iso 60 $(BUILD_DIR)/system 2>&1 | grep -q "cell change: title 1 " && echo "   auto -> title 1 (ok)"
	@HARNESS_DEBUG=1 HARNESS_OPT_deevee_region=1 $(RUN_HARNESS) ./$(TARGET) $(FIXTURE_DIR)/region2.iso 60 $(BUILD_DIR)/system 2>&1 | grep -q "cell change: title 2 " && echo "   forced 1 -> title 2 (ok)"

# Regression checks on the local, uncommitted TestISOs/ (skipped if absent).
# Menu cases found against VLC in M6c (tests/oracle/).
TIMERIC := TestISOs/TIMERIC.iso
check-discs: $(TARGET) $(HARNESS)
	@if [ ! -f $(TIMERIC) ]; then echo "== $(TIMERIC) not found, skipping"; exit 0; fi; \
	echo "== TIMERIC: sub-menu after activation shows its menu SPU + highlight (WAIT -> still)"; \
	out=$$(HARNESS_DEBUG=1 HARNESS_INPUT="839:RIGHT,959:A" $(RUN_HARNESS) ./$(TARGET) $(TIMERIC) 1100 $(BUILD_DIR)/system 2>&1); \
	echo "$$out" | grep -qE '^\[f9[6-9][0-9] .*spu: show' && echo "$$out" | grep -qE '^\[f9[5-9][0-9] .*highlight: button 1 ' \
	  && echo "   More Shorts menu highlighted (ok)" || { echo "   More Shorts menu: FAILED"; exit 1; }; \
	echo "== TIMERIC: menu reached when a short ends is highlighted (held-clock PCI)"; \
	out=$$(HARNESS_DEBUG=1 HARNESS_INPUT="839:DOWN,959:A,1319:LEFT,1439:A,1798:A" $(RUN_HARNESS) ./$(TARGET) $(TIMERIC) 6300 $(BUILD_DIR)/system 2>&1); \
	echo "$$out" | grep -qE '^\[f6[01][0-9][0-9] .*highlight: button 1 ' \
	  && echo "   Field Day menu after 'Meet Campy!' highlighted (ok)" || { echo "   return menu: FAILED"; exit 1; }; \
	echo "== TIMERIC: moving onto an auto-action button follows its jump (photo gallery)"; \
	out=$$(HARNESS_DEBUG=1 HARNESS_INPUT="839:UP,959:A,1319:RIGHT,1678:RIGHT" $(RUN_HARNESS) ./$(TARGET) $(TIMERIC) 1800 $(BUILD_DIR)/system 2>&1); \
	echo "$$out" | grep -qE '^\[f1678 .*navstate: cellN=1 pgN=1 pgc_length=36000' \
	  && echo "   gallery page 2 -> 3 (ok)" || { echo "   auto-action move: FAILED"; exit 1; }; \
	echo "== TIMERIC: state saved mid-short resumes and returns to the same menu/button (disc registers kept)"; \
	HARNESS_STATE=$(BUILD_DIR)/timeric.state HARNESS_SAVE=3000 HARNESS_INPUT="839:DOWN,959:A,1319:LEFT,1439:A,1798:A" \
	  $(RUN_HARNESS) ./$(TARGET) $(TIMERIC) 3001 $(BUILD_DIR)/system >/dev/null 2>&1; \
	out=$$(HARNESS_DEBUG=1 HARNESS_STATE=$(BUILD_DIR)/timeric.state HARNESS_LOAD=0 $(RUN_HARNESS) ./$(TARGET) $(TIMERIC) 3200 $(BUILD_DIR)/system 2>&1); \
	echo "$$out" | grep -qE '^\[f30[5-9][0-9] .*VTS 19 -> 19 \(domain 4\)' && echo "$$out" | grep -qE '^\[f3[01][0-9][0-9] .*highlight: button 1 ' \
	  && echo "   resume mid-short -> Field Day menu, button 1 (ok)" || { echo "   resume mid-short: FAILED"; exit 1; }

.PHONY: harness fixture check check-discs
