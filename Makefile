all: lz4demo64 lz4demo32 

lz4demo64: lz4.c lz4.h bench.c lz4demo.c
	gcc      -O3 -I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration lz4.c bench.c lz4demo.c -o lz4demo64.exe

lz4demo32: lz4.c lz4.h bench.c lz4demo.c
	gcc -m32 -O3 -I. -std=c99 -Wall -W -Wundef -Wno-implicit-function-declaration lz4.c bench.c lz4demo.c -o lz4demo32.exe

clean:
	rm -f core *.o lz4demo32.exe lz4demo64.exe
