/**
 * lz4wrappercli.cpp - LZ4 Wrapper CLI Tool
 * 
 * Command line tool for testing lz4wrapper streaming compression/decompression.
 * Supports single block compression and decompression with configurable parameters.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <getopt.h>
#include <sys/stat.h>

#include "lz4wrapper.h"

// ============================================================================
// Constants
// ============================================================================

static const char* PROGRAM_NAME = "lz4wrapper";
static const char* VERSION = "1.0.0";

// Exit codes
enum {
    EXIT_SUCCESS_CODE = 0,
    EXIT_ERROR_CODE = 1
};

// ============================================================================
// Timer utility
// ============================================================================

class Timer {
public:
    void start() {
        m_start = std::chrono::high_resolution_clock::now();
    }

    void stop() {
        m_end = std::chrono::high_resolution_clock::now();
    }

    double elapsedMs() const {
        return std::chrono::duration<double, std::milli>(m_end - m_start).count();
    }

private:
    std::chrono::high_resolution_clock::time_point m_start;
    std::chrono::high_resolution_clock::time_point m_end;
};

// ============================================================================
// Size parsing utility
// ============================================================================

/**
 * Parse size string with K/M suffix
 * @param str Size string (e.g., "64K", "1M", "65536")
 * @return Parsed size in bytes, or -1 on error
 */
static int parseSize(const char* str) {
    if (!str || !*str) return -1;

    char* endptr;
    long value = strtol(str, &endptr, 10);

    if (value < 0) return -1;

    if (*endptr == 'K' || *endptr == 'k') {
        value *= 1024;
        endptr++;
    } else if (*endptr == 'M' || *endptr == 'm') {
        value *= 1024 * 1024;
        endptr++;
    }

    if (*endptr != '\0') return -1;

    return static_cast<int>(value);
}

/**
 * Parse shrink mode string
 * @param str Mode string (manual|immediate|threshold)
 * @return ShrinkMode enum value, or -1 on error
 */
static int parseShrinkMode(const char* str) {
    if (!str) return -1;
    if (strcmp(str, "manual") == 0) return lz4::SHRINK_MANUAL;
    if (strcmp(str, "immediate") == 0) return lz4::SHRINK_AUTO_IMMEDIATE;
    if (strcmp(str, "threshold") == 0) return lz4::SHRINK_AUTO_THRESHOLD;
    return -1;
}

// ============================================================================
// File I/O utilities
// ============================================================================

/**
 * Read entire file into buffer
 * @param filename File path (NULL for stdin)
 * @param outSize Output: size of data read
 * @return Buffer with file contents, or NULL on error (caller must free)
 */
static char* readFile(const char* filename, int* outSize) {
    FILE* fp = filename ? fopen(filename, "rb") : stdin;
    if (!fp) {
        fprintf(stderr, "Error: Cannot open input file: %s\n", filename ? filename : "stdin");
        return nullptr;
    }

    std::vector<char> buffer;
    char chunk[65536];
    size_t bytesRead;

    while ((bytesRead = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        buffer.insert(buffer.end(), chunk, chunk + bytesRead);
    }

    if (filename) fclose(fp);

    if (buffer.empty()) {
        *outSize = 0;
        return nullptr;
    }

    char* result = new (std::nothrow) char[buffer.size()];
    if (!result) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        return nullptr;
    }

    memcpy(result, buffer.data(), buffer.size());
    *outSize = static_cast<int>(buffer.size());
    return result;
}

/**
 * Write buffer to file
 * @param filename File path (NULL for stdout)
 * @param data Data buffer
 * @param size Data size
 * @return true on success
 */
static bool writeFile(const char* filename, const char* data, int size) {
    FILE* fp = filename ? fopen(filename, "wb") : stdout;
    if (!fp) {
        fprintf(stderr, "Error: Cannot open output file: %s\n", filename ? filename : "stdout");
        return false;
    }

    size_t written = fwrite(data, 1, size, fp);

    if (filename) fclose(fp);

    return written == static_cast<size_t>(size);
}

// ============================================================================
// Compression format
// ============================================================================
// Compressed data format (binary):
//   - original_size: 4 bytes, little-endian
//   - compressed_size: 4 bytes, little-endian
//   - compressed_data: compressed_size bytes

static void writeLE32(char* dst, uint32_t value) {
    dst[0] = static_cast<char>(value & 0xFF);
    dst[1] = static_cast<char>((value >> 8) & 0xFF);
    dst[2] = static_cast<char>((value >> 16) & 0xFF);
    dst[3] = static_cast<char>((value >> 24) & 0xFF);
}

static uint32_t readLE32(const char* src) {
    return static_cast<uint32_t>(static_cast<unsigned char>(src[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(src[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(src[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(src[3])) << 24);
}

// ============================================================================
// Options structure
// ============================================================================

struct Options {
    int minWorkArea = lz4::DEFAULT_WORK_AREA;
    int maxWorkArea = lz4::DEFAULT_MAX_WORK;
    lz4::ShrinkMode shrinkMode = lz4::SHRINK_MANUAL;
    int shrinkThreshold = 0;
    bool compress = false;
    bool decompress = false;
    bool verbose = false;
    bool quiet = false;
    bool clean = false;
    bool help = false;
    const char* inputFile = nullptr;
    const char* outputFile = nullptr;
};

// ============================================================================
// Help and usage
// ============================================================================

static void printUsage() {
    printf("Usage: %s [OPTIONS] [INPUT] [OUTPUT]\n", PROGRAM_NAME);
    printf("\n");
    printf("LZ4 Wrapper streaming compression/decompression tool.\n");
    printf("\n");
    printf("Mode selection:\n");
    printf("  -c, --compress         Compress mode\n");
    printf("  -d, --decompress       Decompress mode\n");
    printf("\n");
    printf("Options:\n");
    printf("  --min-work-area=SIZE   Minimum work area size (default: 64K)\n");
    printf("  --max-work-area=SIZE   Maximum work area size (default: 10M)\n");
    printf("  --shrink-mode=MODE     Shrink mode: manual|immediate|threshold (default: manual)\n");
    printf("  --shrink-threshold=SIZE Shrink threshold for threshold mode\n");
    printf("  -v, --verbose          Verbose output with timing information\n");
    printf("  -q, --quiet            Quiet mode, suppress non-error output\n");
    printf("  --clean                Clean temporary files (no-op for this tool)\n");
    printf("  -h, --help             Show this help message\n");
    printf("\n");
    printf("SIZE suffixes: K (kilobytes), M (megabytes)\n");
    printf("Examples: 64K, 1M, 10485760\n");
    printf("\n");
    printf("Exit codes: 0=success, 1=error\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -c input.bin output.lz4\n", PROGRAM_NAME);
    printf("  %s -d input.lz4 output.bin\n", PROGRAM_NAME);
    printf("  cat data | %s -c > compressed.lz4\n", PROGRAM_NAME);
    printf("  %s -c --min-work-area=128K --max-work-area=5M input output\n", PROGRAM_NAME);
}

static void printVersion() {
    printf("%s version %s\n", PROGRAM_NAME, VERSION);
}

// ============================================================================
// Option parsing
// ============================================================================

static const struct option longOptions[] = {
    {"compress",         no_argument,       nullptr, 'c'},
    {"decompress",       no_argument,       nullptr, 'd'},
    {"min-work-area",    required_argument, nullptr, 'M'},
    {"max-work-area",    required_argument, nullptr, 'X'},
    {"shrink-mode",      required_argument, nullptr, 'S'},
    {"shrink-threshold", required_argument, nullptr, 'T'},
    {"verbose",          no_argument,       nullptr, 'v'},
    {"quiet",            no_argument,       nullptr, 'q'},
    {"clean",            no_argument,       nullptr, 'C'},
    {"help",             no_argument,       nullptr, 'h'},
    {"version",          no_argument,       nullptr, 'V'},
    {nullptr,            0,                 nullptr, 0}
};

static bool parseOptions(int argc, char* argv[], Options& opts) {
    int opt;
    while ((opt = getopt_long(argc, argv, "cdvqh", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                opts.compress = true;
                break;
            case 'd':
                opts.decompress = true;
                break;
            case 'M': {
                int size = parseSize(optarg);
                if (size < 0) {
                    fprintf(stderr, "Error: Invalid min-work-area size: %s\n", optarg);
                    return false;
                }
                opts.minWorkArea = size;
                break;
            }
            case 'X': {
                int size = parseSize(optarg);
                if (size < 0) {
                    fprintf(stderr, "Error: Invalid max-work-area size: %s\n", optarg);
                    return false;
                }
                opts.maxWorkArea = size;
                break;
            }
            case 'S': {
                int mode = parseShrinkMode(optarg);
                if (mode < 0) {
                    fprintf(stderr, "Error: Invalid shrink mode: %s\n", optarg);
                    return false;
                }
                opts.shrinkMode = static_cast<lz4::ShrinkMode>(mode);
                break;
            }
            case 'T': {
                int size = parseSize(optarg);
                if (size < 0) {
                    fprintf(stderr, "Error: Invalid shrink-threshold size: %s\n", optarg);
                    return false;
                }
                opts.shrinkThreshold = size;
                break;
            }
            case 'v':
                opts.verbose = true;
                break;
            case 'q':
                opts.quiet = true;
                break;
            case 'C':
                opts.clean = true;
                break;
            case 'h':
                opts.help = true;
                break;
            case 'V':
                printVersion();
                exit(EXIT_SUCCESS_CODE);
            default:
                return false;
        }
    }

    // Parse positional arguments
    if (optind < argc) {
        opts.inputFile = argv[optind++];
    }
    if (optind < argc) {
        opts.outputFile = argv[optind++];
    }

    return true;
}

// ============================================================================
// Compress operation
// ============================================================================

static int doCompress(const Options& opts) {
    Timer timer;

    // Read input
    int inputSize = 0;
    char* inputData = readFile(opts.inputFile, &inputSize);
    if (!inputData || inputSize <= 0) {
        fprintf(stderr, "Error: Failed to read input data\n");
        return EXIT_ERROR_CODE;
    }

    // Create encoder
    lz4::Lz4Encoder encoder(opts.minWorkArea, opts.maxWorkArea, opts.shrinkMode, opts.shrinkThreshold);
    if (!encoder.isInitialized()) {
        fprintf(stderr, "Error: Failed to initialize encoder\n");
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }
    if (opts.verbose && !opts.quiet) {
        fprintf(stdout, "Encoder parameters: workArea=%d, ringBufferSize=%d, baseBufferSize=%d, maxBufferSize=%d\n",
                encoder.getWorkArea(), encoder.getRingBufferSize(), encoder.getBaseBufferSize(), encoder.getMaxBufferSize());
        fprintf(stdout, "Encoder shrink mode: %s\n", encoder.getShrinkMode() == lz4::SHRINK_MANUAL ? "manual" :
                encoder.getShrinkMode() == lz4::SHRINK_AUTO_IMMEDIATE ? "immediate" : "threshold");
        fprintf(stdout, "Encoder shrink threshold: %d\n", encoder.getShrinkThreshold());
    }

    // Allocate output buffer (worst case: input size + overhead)
    int maxOutputSize = LZ4_compressBound(inputSize);
    char* compressedData = new (std::nothrow) char[maxOutputSize];
    if (!compressedData) {
        fprintf(stderr, "Error: Failed to allocate output buffer\n");
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Compress
    timer.start();
    int compressedSize = encoder.compress(compressedData, maxOutputSize, inputData, inputSize);
    timer.stop();

    if (compressedSize <= 0) {
        fprintf(stderr, "Error: Compression failed (error code: %d)\n", compressedSize);
        delete[] inputData;
        delete[] compressedData;
        return EXIT_ERROR_CODE;
    }

    // Write output with header
    // Format: original_size (4 bytes) + compressed_size (4 bytes) + compressed_data
    int outputSize = 8 + compressedSize;
    char* outputData = new (std::nothrow) char[outputSize];
    if (!outputData) {
        fprintf(stderr, "Error: Failed to allocate output buffer\n");
        delete[] inputData;
        delete[] compressedData;
        return EXIT_ERROR_CODE;
    }

    writeLE32(outputData, static_cast<uint32_t>(inputSize));
    writeLE32(outputData + 4, static_cast<uint32_t>(compressedSize));
    memcpy(outputData + 8, compressedData, compressedSize);

    if (!writeFile(opts.outputFile, outputData, outputSize)) {
        fprintf(stderr, "Error: Failed to write output\n");
        delete[] inputData;
        delete[] compressedData;
        delete[] outputData;
        return EXIT_ERROR_CODE;
    }

    // Print verbose information
    if (opts.verbose && !opts.quiet) {
        double ratio = 100.0 * compressedSize / inputSize;
        double elapsed = timer.elapsedMs();
        double throughput = (inputSize / 1024.0 / 1024.0) / (elapsed / 1000.0);

        fprintf(stderr, "Compressed %d bytes -> %d bytes (%.2f%%) in %.3f ms\n",
                inputSize, compressedSize, ratio, elapsed);
        fprintf(stderr, "Throughput: %.2f MB/s\n", throughput);
        fprintf(stderr, "WorkArea: %d bytes, RingBufferSize: %d bytes\n",
                encoder.getWorkArea(), encoder.getRingBufferSize());
    }

    delete[] inputData;
    delete[] compressedData;
    delete[] outputData;

    return EXIT_SUCCESS_CODE;
}

// ============================================================================
// Decompress operation
// ============================================================================

static int doDecompress(const Options& opts) {
    Timer timer;

    // Read input
    int inputSize = 0;
    char* inputData = readFile(opts.inputFile, &inputSize);
    if (!inputData || inputSize < 8) {
        fprintf(stderr, "Error: Failed to read input data or data too small\n");
        if (inputData) delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Parse header
    uint32_t originalSize = readLE32(inputData);
    uint32_t compressedSize = readLE32(inputData + 4);

    if (static_cast<int>(compressedSize) + 8 != inputSize) {
        fprintf(stderr, "Error: Invalid compressed data format\n");
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Create decoder
    lz4::Lz4Decoder decoder(opts.minWorkArea, opts.maxWorkArea, opts.shrinkMode, opts.shrinkThreshold);
    if (!decoder.isInitialized()) {
        fprintf(stderr, "Error: Failed to initialize decoder\n");
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }
    if (opts.verbose && !opts.quiet) {
        fprintf(stdout, "Decoder parameters: workArea=%d, ringBufferSize=%d, baseBufferSize=%d, maxBufferSize=%d\n",
                decoder.getWorkArea(), decoder.getRingBufferSize(), decoder.getBaseBufferSize(), decoder.getMaxBufferSize());
        fprintf(stdout, "Decoder shrink mode: %s\n", decoder.getShrinkMode() == lz4::SHRINK_MANUAL ? "manual" :
                decoder.getShrinkMode() == lz4::SHRINK_AUTO_IMMEDIATE ? "immediate" : "threshold");
        fprintf(stdout, "Decoder shrink threshold: %d\n", decoder.getShrinkThreshold());
    }

    // Allocate output buffer
    char* decompressedData = new (std::nothrow) char[originalSize];
    if (!decompressedData) {
        fprintf(stderr, "Error: Memory allocation failed for decompressed data\n");
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Decompress
    timer.start();
    int decompressedSize = decoder.decompress(decompressedData, inputData + 8, compressedSize, originalSize);
    timer.stop();

    if (decompressedSize <= 0) {
        fprintf(stderr, "Error: Decompression failed (error code: %d)\n", decompressedSize);
        delete[] decompressedData;
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Write output
    if (!writeFile(opts.outputFile, decompressedData, decompressedSize)) {
        fprintf(stderr, "Error: Failed to write output\n");
        delete[] decompressedData;
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Print verbose information
    if (opts.verbose && !opts.quiet) {
        double elapsed = timer.elapsedMs();
        double throughput = (decompressedSize / 1024.0 / 1024.0) / (elapsed / 1000.0);

        fprintf(stderr, "Decompressed %d bytes -> %d bytes in %.3f ms\n",
                static_cast<int>(compressedSize), decompressedSize, elapsed);
        fprintf(stderr, "Throughput: %.2f MB/s\n", throughput);
        fprintf(stderr, "WorkArea: %d bytes, RingBufferSize: %d bytes\n",
                decoder.getWorkArea(), decoder.getRingBufferSize());
    }

    delete[] decompressedData;
    delete[] inputData;

    return EXIT_SUCCESS_CODE;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    Options opts;

    if (!parseOptions(argc, argv, opts)) {
        printUsage();
        return EXIT_ERROR_CODE;
    }

    if (opts.help) {
        printUsage();
        return EXIT_SUCCESS_CODE;
    }

    if (opts.clean) {
        // No-op for this tool
        return EXIT_SUCCESS_CODE;
    }

    if (opts.compress && opts.decompress) {
        fprintf(stderr, "Error: Cannot specify both -c and -d\n");
        return EXIT_ERROR_CODE;
    }

    if (!opts.compress && !opts.decompress) {
        fprintf(stderr, "Error: Must specify either -c (compress) or -d (decompress)\n");
        printUsage();
        return EXIT_ERROR_CODE;
    }

    if (opts.compress) {
        return doCompress(opts);
    } else {
        return doDecompress(opts);
    }
}

