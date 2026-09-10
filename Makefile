CC := cc
CFLAGS := -std=gnu99
CLIBS := -lm -lraylib -I/usr/include/raylib
SRC_FILES := $(filter-out ss.c, $(wildcard *.[ch]))
WFLAGS := -Wall -Wextra

# Automatically enable X11 when running under X11,
# or explicitly with: make X11=1
ifneq ($(X11),)
    CLIBS += -lX11
else ifneq ($(DISPLAY),)
    CLIBS += -lX11
endif

.PHONY: all release clean

all: ss

ss: ss.c $(SRC_FILES)
	$(CC) -o $@ $< $(CFLAGS) -g -O0 $(WFLAGS) $(CLIBS)

release: ss.c $(SRC_FILES)
	$(CC) -o ss $< $(CFLAGS) -O3 $(WFLAGS) $(CLIBS)

clean:
	rm -f ss
