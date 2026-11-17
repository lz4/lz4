/*
 * throttle : copies standard input to standard output at a limited speed,
 * to simulate a storage device or a network link within a pipeline.
 * usage : throttle [-b MB] MB/s      (1 MB = 1000000 bytes)
 *
 * Data is delivered once the link has had the time to carry it.
 * The link carries one chunk at a time, and earns no credit while idle,
 * beyond a margin which absorbs late wake-ups.
 *
 * -b MB : network link, with a buffer of MB on each side, like the socket buffers of TCP :
 *         the sender can write bursts while the link is busy, and the receiver can read them later.
 *         Without -b : storage device, which delivers data as soon as it is read.
 */
#if defined(__linux__)
#  define _GNU_SOURCE   /* splice() */
#  include <fcntl.h>
#else
#  define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* end of the link time of a chunk : starts when the link and the data are both ready */
static double carry(double linkFree, size_t size, double rate)
{
    double const t = now();
    if (linkFree < t - IDLE_MARGIN) linkFree = t - IDLE_MARGIN;
    return linkFree + (double)size / rate;
}


/* ===== storage device : no buffer ===== */

static int throttleDevice(double rate)
{
    double linkFree = now();
    size_t chunkSize;
    char* buffer;

    /* chunks of ~2 ms of transfer, from 4 KB to 1 MB */
    chunkSize = (size_t)(rate / 500);
    if (chunkSize < 4096) chunkSize = 4096;
    if (chunkSize > BUFFER_SIZE) chunkSize = BUFFER_SIZE;

#ifdef SPLICE_F_MOVE
    /* Linux : moves data without copying it, when standard input or output is a pipe.
     * Data is delivered first, then the link waits for the time to carry it. */
    for (;;) {
        ssize_t const n = splice(STDIN_FILENO, NULL, STDOUT_FILENO, NULL, chunkSize, SPLICE_F_MOVE | SPLICE_F_MORE);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EINVAL) break;   /* neither is a pipe : read() and write() */
            perror("throttle: splice");
            return 1;
        }
        linkFree = carry(linkFree, (size_t)n, rate);
        waitUntil(linkFree);
    }
#endif
    buffer = (char*)malloc(BUFFER_SIZE);
    if (buffer == NULL) {
        perror("throttle");
        return 1;
    }
    for (;;) {
        size_t filled = 0;
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
        linkFree = carry(linkFree, filled, rate);
        waitUntil(linkFree);
        if (writeAll(buffer, filled)) {
            perror("throttle: write");
            return 1;
        }
    }
    free(buffer);
    return 0;
}


/* ===== network link : a buffer on each side =====
 * One ring buffer, with 3 positions counted from the start of the stream :
 * read (from stdin) >= carried (by the link) >= written (to stdout).
 * The send buffer holds read - carried, the receive buffer holds carried - written. */

typedef struct {
    char* ring;
    size_t sideSize;   /* size of each buffer : the ring holds both */
    unsigned long long read, carried, written;
    int inputEnded, error;
    pthread_mutex_t lock;
    pthread_cond_t changed;
} Link;

static void setError(Link* l, const char* msg)
{
    perror(msg);
    pthread_mutex_lock(&l->lock);
    l->error = 1;
    pthread_cond_broadcast(&l->changed);
    pthread_mutex_unlock(&l->lock);
}

static void* readInput(void* arg)
{
    Link* const l = (Link*)arg;
    size_t const ringSize = 2 * l->sideSize;
    for (;;) {
        size_t pos, room;
        ssize_t n;
        pthread_mutex_lock(&l->lock);
        while (!l->error && l->read - l->carried >= l->sideSize) pthread_cond_wait(&l->changed, &l->lock);
        if (l->error) {
            pthread_mutex_unlock(&l->lock);
            return NULL;
        }
        pos = (size_t)(l->read % ringSize);
        room = l->sideSize - (size_t)(l->read - l->carried);
        if (room > ringSize - pos) room = ringSize - pos;   /* contiguous */
        pthread_mutex_unlock(&l->lock);

        n = read(STDIN_FILENO, l->ring + pos, room);
        if (n < 0) {
            if (errno == EINTR) continue;
            setError(l, "throttle: read");
            return NULL;
        }
        pthread_mutex_lock(&l->lock);
        if (n == 0) l->inputEnded = 1;
        l->read += (unsigned long long)n;
        pthread_cond_broadcast(&l->changed);
        pthread_mutex_unlock(&l->lock);
        if (n == 0) return NULL;
    }
}

static void* writeOutput(void* arg)
{
    Link* const l = (Link*)arg;
    size_t const ringSize = 2 * l->sideSize;
    for (;;) {
        size_t pos, size;
        pthread_mutex_lock(&l->lock);
        while (!l->error && l->carried == l->written && !(l->inputEnded && l->written == l->read))
            pthread_cond_wait(&l->changed, &l->lock);
        if (l->error || (l->inputEnded && l->written == l->read)) {   /* everything carried and written */
            pthread_mutex_unlock(&l->lock);
            return NULL;
        }
        pos = (size_t)(l->written % ringSize);
        size = (size_t)(l->carried - l->written);
        if (size > ringSize - pos) size = ringSize - pos;
        pthread_mutex_unlock(&l->lock);

        if (writeAll(l->ring + pos, size)) {
            setError(l, "throttle: write");
            return NULL;
        }
        pthread_mutex_lock(&l->lock);
        l->written += size;
        pthread_cond_broadcast(&l->changed);
        pthread_mutex_unlock(&l->lock);
    }
}

static int throttleNetwork(double rate, size_t sideSize)
{
    Link l;
    pthread_t reader, writer;
    double linkFree = now();
    size_t chunkMax = (size_t)(rate / 500);   /* ~2 ms of transfer */
    if (chunkMax < 4096) chunkMax = 4096;

    memset(&l, 0, sizeof(l));
    l.sideSize = sideSize;
    l.ring = (char*)malloc(2 * sideSize);
    if (l.ring == NULL) {
        perror("throttle");
        return 1;
    }
    pthread_mutex_init(&l.lock, NULL);
    pthread_cond_init(&l.changed, NULL);
    if (pthread_create(&reader, NULL, readInput, &l) || pthread_create(&writer, NULL, writeOutput, &l)) {
        perror("throttle: pthread_create");
        return 1;
    }

    /* the link : carries data from the send buffer to the receive buffer, at its speed */
    for (;;) {
        size_t size, recvRoom;
        pthread_mutex_lock(&l.lock);
        while (!l.error && !(l.inputEnded && l.carried == l.read)
               && (l.carried == l.read || l.carried - l.written >= sideSize))
            pthread_cond_wait(&l.changed, &l.lock);
        if (l.error || (l.inputEnded && l.carried == l.read)) {
            pthread_mutex_unlock(&l.lock);
            break;
        }
        size = (size_t)(l.read - l.carried);
        recvRoom = sideSize - (size_t)(l.carried - l.written);
        if (size > recvRoom) size = recvRoom;
        if (size > chunkMax) size = chunkMax;
        pthread_mutex_unlock(&l.lock);

        linkFree = carry(linkFree, size, rate);
        waitUntil(linkFree);

        pthread_mutex_lock(&l.lock);
        l.carried += size;
        pthread_cond_broadcast(&l.changed);
        pthread_mutex_unlock(&l.lock);
    }
    pthread_join(reader, NULL);
    pthread_join(writer, NULL);
    free(l.ring);
    return l.error;
}

int main(int argc, char** argv)
{
    double rate, bufferMB = 0;
    int arg = 1;

    if (argc == 4 && !strcmp(argv[1], "-b")) {
        bufferMB = atof(argv[2]);
        arg = 3;
    }
    if (argc != arg + 1 || (rate = atof(argv[arg]) * 1e6) <= 0 || bufferMB < 0) {
        fprintf(stderr, "usage : %s [-b MB] MB/s \n", argv[0]);
        return 1;
    }
    if (bufferMB > 0) return throttleNetwork(rate, (size_t)(bufferMB * 1e6));
    return throttleDevice(rate);
}
