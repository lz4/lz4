all: lz4demo64 lz4demo32 

lz4demo64: lz4.c lz4.h lz4demo.c
	gcc -g -O3 -I. -Wall -W lz4.c lz4demo.c -o lz4demo64.exe

lz4demo32: lz4.c lz4.h lz4demo.c
	gcc -m32 -g -O3 -I. -Wall -W lz4.c lz4demo.c -o lz4demo32.exe

clean:
	rm -f core *.o lz4demo32.exe lz4demo64.exe
