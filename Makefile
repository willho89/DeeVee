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

ifeq ($(OS),Windows_NT)
	TARGET := $(TARGET_NAME)_libretro.dll
	SHARED := -shared
	EXPORTS := -Wl,--export-all-symbols
	TEST_EXE := tests/test_content.exe
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

CLEAN_FILES := $(OBJ) $(TARGET) tests/test_content tests/test_content.exe

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(SHARED) $(FPIC) $(EXPORTS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(FPIC) -c -o $@ $<

$(TEST_EXE): tests/test_content.c src/deevee_content.c src/deevee_content.h
	$(CC) $(CSTD) $(WARNFLAGS) -Isrc -o $@ tests/test_content.c src/deevee_content.c

test: $(TEST_EXE)
	./$(TEST_EXE)

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
