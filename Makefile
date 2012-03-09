OS := $(shell uname)

ifeq ($(OS),Linux)
  OUTPUT32  = lz4demo32
  OUTPUT64  = lz4demo64
else
  OUTPUT32  = LZ4Demo32.exe
  OUTPUT64  = LZ4Demo64.exe
endif

all: lz4demo64 lz4demo32 

lz4demo64: lz4.c lz4.h bench.c lz4demo.c
	gcc      -O3 -I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration lz4.c bench.c lz4demo.c -o $(OUTPUT64)

lz4demo32: lz4.c lz4.h bench.c lz4demo.c
	gcc -m32 -O3 -I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration lz4.c bench.c lz4demo.c -o $(OUTPUT32)

clean:
	rm -f core *.o $(OUTPUT32) $(OUTPUT64)
