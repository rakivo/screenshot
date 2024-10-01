CC=cc
CFLAGS=std=c++11 -O0 -g
CLIBS=-lX11 -lraylib
WFLAGS := -Wall -Wextra -Wpedantic

export $(TESSDATA_PREFIX)

ss: ss.c
	$(CC) -o $@ $< $(CFLAGS) $(WFLAGS) $(CLIBS)
