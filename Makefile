OS := $(shell uname)

ifeq ($(OS),Linux)
  OUTPUT32  = lz4demo32
  OUTPUT    = lz4demo
else
  OUTPUT32  = LZ4Demo32.exe
  OUTPUT    = LZ4Demo.exe
endif

all: lz4demo

lz4demo: lz4.c lz4.h lz4hc.c lz4hc.h bench.c lz4demo.c
	gcc      -O3 -I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration lz4hc.c lz4.c bench.c lz4demo.c -o $(OUTPUT)

lz4demo32: lz4.c lz4.h lz4hc.c lz4hc.h bench.c lz4demo.c
	gcc -m32 -Os -march=native -I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration lz4hc.c lz4.c bench.c lz4demo.c -o $(OUTPUT32)

clean:
	rm -f core *.o $(OUTPUT32) $(OUTPUT)
