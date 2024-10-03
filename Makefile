CC := cc
CFLAGS := -std=c99 -O0 -g
CLIBS := -lm -lX11 -lraylib
SRC_FILES := $(filter-out ss.c, $(wildcard *.[ch]))
WFLAGS := -Wall -Wextra -Wpedantic

ss: ss.c $(SRC_FILES)
	$(CC) -o $@ $< $(CFLAGS) $(WFLAGS) $(CLIBS)
