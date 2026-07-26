# Drive2Iso build. Auto-selects the host backend.
# Build on MSYS2/MinGW (Windows) -> drive2iso.exe, or on Linux -> drive2iso.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Iinclude
LDFLAGS ?=

ifeq ($(OS),Windows_NT)
    BACKEND := src/backend/backend_windows.c src/backend/winutil.c \
               src/backend/winusb.c src/backend/winvss.c
    BIN     := drive2iso.exe
else
    BACKEND := src/backend/backend_linux.c
    BIN     := drive2iso
endif

CORE := $(wildcard src/core/*.c)
SRCS := $(CORE) src/main.c $(BACKEND)
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) drive2iso drive2iso.exe
