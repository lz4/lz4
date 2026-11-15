/*
 * throttle : copies standard input to standard output at a limited speed,
 * to simulate a storage device or a network link within a pipeline.
 * usage : throttle MB/s      (1 MB = 1000000 bytes)
 *
 * Data is delivered once the link has had the time to carry it.
 * The link carries one chunk at a time, and earns no credit while idle,
 * beyond a margin which absorbs late wake-ups.
 */
#if defined(__linux__)
#  define _GNU_SOURCE   /* splice() */
#  include <fcntl.h>
#else
#  define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE (1 << 20)
#define IDLE_MARGIN 0.001   /* seconds */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void waitUntil(double t)
{
    double delay;
    while ((delay = t - now()) > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)delay;
        ts.tv_nsec = (long)((delay - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
}

static int writeAll(const char* buffer, size_t size)
{
    while (size > 0) {
        ssize_t const n = write(STDOUT_FILENO, buffer, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buffer += n;
        size -= (size_t)n;
    }
    return 0;
}

int main(int argc, char** argv)
{
    double rate;      /* bytes per second */
    double linkFree;  /* time when the link has carried everything so far */
    size_t chunkSize;
    char* buffer;

    if (argc != 2 || (rate = atof(argv[1]) * 1e6) <= 0) {
        fprintf(stderr, "usage : %s MB/s \n", argv[0]);
        return 1;
    }
    /* chunks of ~2 ms of transfer, from 4 KB to 1 MB */
    chunkSize = (size_t)(rate / 500);
    if (chunkSize < 4096) chunkSize = 4096;
    if (chunkSize > BUFFER_SIZE) chunkSize = BUFFER_SIZE;
    buffer = (char*)malloc(BUFFER_SIZE);
    if (buffer == NULL) {
        perror("throttle");
        return 1;
    }

    linkFree = now();
#ifdef SPLICE_F_MOVE
    /* Linux : moves data without copying it, when standard input or output is a pipe.
     * Data is delivered first, then the link waits for the time to carry it. */
    for (;;) {
        ssize_t const n = splice(STDIN_FILENO, NULL, STDOUT_FILENO, NULL, chunkSize, SPLICE_F_MOVE | SPLICE_F_MORE);
        double t;
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EINVAL) break;   /* neither is a pipe : read() and write() */
            perror("throttle: splice");
            return 1;
        }
        t = now();
        if (linkFree < t - IDLE_MARGIN) linkFree = t - IDLE_MARGIN;
        linkFree += (double)n / rate;
        waitUntil(linkFree);
    }
#endif
    for (;;) {
        size_t filled = 0;
        double t;
        /* a full chunk, unless the input ends : a pipe delivers 64 KB at most per read */
        while (filled < chunkSize) {
            ssize_t const n = read(STDIN_FILENO, buffer + filled, chunkSize - filled);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("throttle: read");
                return 1;
            }
            filled += (size_t)n;
        }
        if (filled == 0) break;
        t = now();
        if (linkFree < t - IDLE_MARGIN) linkFree = t - IDLE_MARGIN;
        linkFree += (double)filled / rate;
        waitUntil(linkFree);
        if (writeAll(buffer, filled)) {
            perror("throttle: write");
            return 1;
        }
    }
    free(buffer);
    return 0;
}
