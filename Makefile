CC=gcc
CFLAGS=-I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration

OS := $(shell uname)
ifeq ($(OS),Linux)
EXT =
else
EXT =.exe
endif

default: lz4demo

all: lz4demo lz4demo32 fuzzer

lz4demo: lz4.c lz4hc.c bench.c lz4demo.c
	$(CC)      -O3 $(CFLAGS) $^ -o $@$(EXT)

lz4demo32: lz4.c lz4hc.c bench.c lz4demo.c
	$(CC) -m32 -Os -march=native $(CFLAGS) $^ -o $@$(EXT)

fuzzer : lz4.c fuzzer.c
	$(CC)      -O3 $(CFLAGS) $^ -o $@$(EXT)
	
clean:
	rm -f core *.o lz4demo$(EXT) lz4demo32$(EXT) fuzzer$(EXT)
