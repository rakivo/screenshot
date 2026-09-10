CC := cc
CFLAGS := -std=gnu99
CLIBS := -lm -lX11 -lraylib -I/usr/include/raylib
SRC_FILES := $(filter-out ss.c, $(wildcard *.[ch]))
WFLAGS := -Wall -Wextra

.PHONY: all release clean

all: ss

ss: ss.c $(SRC_FILES)
	$(CC) -o $@ $< $(CFLAGS) -g -O0 $(WFLAGS) $(CLIBS)

release: ss.c $(SRC_FILES)
	$(CC) -o ss $< $(CFLAGS) -O3 $(WFLAGS) $(CLIBS)

clean:
	rm -f ss
