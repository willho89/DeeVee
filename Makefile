include Makefile.common

ifeq ($(origin CC),default)
	ifeq ($(OS),Windows_NT)
		CC := gcc
	else
		CC := cc
	endif
endif

OBJ := $(SOURCES_C:.c=.o)
TEST_EXE := tests/test_content
TEST_DISC_EXE := tests/test_disc
TEST_DECODER_EXE := tests/test_decoder
PROBE_EXE := tools/deevee_probe

ifeq ($(OS),Windows_NT)
	TARGET := $(TARGET_NAME)_libretro.dll
	SHARED := -shared
	EXPORTS := -Wl,--export-all-symbols
	TEST_EXE := tests/test_content.exe
	TEST_DISC_EXE := tests/test_disc.exe
	TEST_DECODER_EXE := tests/test_decoder.exe
	PROBE_EXE := tools/deevee_probe.exe
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		TARGET := $(TARGET_NAME)_libretro.dylib
		SHARED := -dynamiclib
	else
		TARGET := $(TARGET_NAME)_libretro.so
		SHARED := -shared
	endif
	FPIC := -fPIC
endif

CLEAN_FILES := $(OBJ) $(TARGET) tests/test_content tests/test_content.exe \
	tests/test_disc tests/test_disc.exe tests/test_decoder tests/test_decoder.exe \
	tools/deevee_probe tools/deevee_probe.exe

.PHONY: all clean probe test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(SHARED) $(FPIC) $(EXPORTS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(FPIC) -c -o $@ $<

$(TEST_EXE): tests/test_content.c src/deevee_content.c src/deevee_content.h
	$(CC) $(CSTD) $(WARNFLAGS) -Isrc -o $@ tests/test_content.c src/deevee_content.c

$(TEST_DISC_EXE): tests/test_disc.c src/deevee_disc.c src/deevee_disc.h src/deevee_dvd.c src/deevee_dvd.h src/deevee_iso.c src/deevee_iso.h src/deevee_content.c src/deevee_content.h
	$(CC) $(CFLAGS) -o $@ tests/test_disc.c src/deevee_disc.c src/deevee_dvd.c src/deevee_iso.c src/deevee_content.c $(CHD_SOURCES_C) $(LDFLAGS)

$(TEST_DECODER_EXE): tests/test_decoder.c src/deevee_decoder.c src/deevee_decoder.h
	$(CC) $(CFLAGS) -o $@ tests/test_decoder.c src/deevee_decoder.c $(LDFLAGS)

$(PROBE_EXE): tools/deevee_probe.c src/deevee_disc.c src/deevee_disc.h src/deevee_dvd.c src/deevee_dvd.h src/deevee_iso.c src/deevee_iso.h src/deevee_content.c src/deevee_content.h src/deevee_audio.c src/deevee_audio.h src/deevee_dvdnav.c src/deevee_dvdnav.h src/deevee_subpicture.c src/deevee_subpicture.h
	$(CC) $(CFLAGS) -o $@ tools/deevee_probe.c src/deevee_disc.c src/deevee_dvd.c src/deevee_iso.c src/deevee_content.c src/deevee_decoder.c src/deevee_audio.c src/deevee_dvdnav.c src/deevee_subpicture.c $(CHD_SOURCES_C) $(LDFLAGS)

probe: $(PROBE_EXE)

test: $(TEST_EXE) $(TEST_DISC_EXE) $(TEST_DECODER_EXE)
	./$(TEST_EXE)
	./$(TEST_DISC_EXE)
	./$(TEST_DECODER_EXE)

clean:
ifeq ($(OS),Windows_NT)
ifneq ($(MSYSTEM),)
	rm -f $(CLEAN_FILES)
else
	-cmd /C del /Q /F $(subst /,\,$(CLEAN_FILES)) 2>NUL
endif
else
	rm -f $(CLEAN_FILES)
endif
