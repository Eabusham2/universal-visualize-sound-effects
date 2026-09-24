# SoundOverlay - native Win32 (C) build.
#
# Native build on Windows with MinGW-w64:
#     mingw32-make
#
# Cross-compile from Linux/macOS with MinGW-w64:
#     make CC=x86_64-w64-mingw32-gcc WINDRES=x86_64-w64-mingw32-windres
#
# Host unit tests (any OS with a C compiler):
#     make test
#
# Produces SoundOverlay.exe.

CC      ?= gcc
WINDRES ?= windres
HOSTCC  ?= cc
CFLAGS  := -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
LDFLAGS := -municode -mwindows -static
LIBS    := -luser32 -lgdi32 -lole32 -loleaut32 -lavrt -lcomctl32 -luuid -lshell32

SRC := main.c overlay.c audio.c detector.c fft.c profiles.c settings.c
OBJ := $(SRC:.c=.o)
HDR := audio.h detector.h fft.h overlay.h profiles.h settings.h

TARGET := SoundOverlay.exe

all: $(TARGET)

$(TARGET): $(OBJ) resource.o
	$(CC) $(OBJ) resource.o -o $@ $(LDFLAGS) $(LIBS)

# Coarse but correct: rebuild every object when any shared header changes.
%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c $< -o $@

resource.o: resource.rc app.manifest assets/icon.ico
	$(WINDRES) -i resource.rc -o resource.o

# The FFT is plain C, so it is verified natively against a reference DFT.
test: tests/fft_test
	./tests/fft_test

tests/fft_test: tests/fft_test.c fft.c fft.h
	$(HOSTCC) -O2 -Wall -Wextra -I. tests/fft_test.c fft.c -o $@ -lm

clean:
	-rm -f $(OBJ) resource.o $(TARGET) tests/fft_test

.PHONY: all test clean
