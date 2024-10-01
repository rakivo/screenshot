CC := cc
CFLAGS := -std=c11 -O0 -g
CLIBS := -lm -lX11 -lraylib
WFLAGS := -Wall -Wextra -Wpedantic

ss: ss.c
	$(CC) -o $@ $< $(CFLAGS) $(WFLAGS) $(CLIBS)
