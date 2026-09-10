CC := cc
CFLAGS := -std=gnu99
CLIBS := -lm -lX11 -lraylib -I/usr/include/raylib
SRC_FILES := $(filter-out, $(wildcard *.[ch]))
WFLAGS := -Wall -Wextra

ss: ss.c $(SRC_FILES)
	$(CC) -o $@ $< $(CFLAGS) -g -O0 $(WFLAGS) $(CLIBS)

release: ss.c $(SRC_FILES)
	$(CC) -o ss $< $(CFLAGS) -O3 $(WFLAGS) $(CLIBS)
