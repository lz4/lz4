/**
 * lz4wrapperbatchcli.cpp - LZ4 Wrapper Batch CLI Tool
 * 
 * Command line tool for testing lz4wrapper streaming compression/decompression
 * with multiple data blocks. Designed to test ring buffer behavior and memory management.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <getopt.h>

#include "lz4wrapper.h"

// ============================================================================
// Constants
// ============================================================================

static const char* PROGRAM_NAME = "lz4wrapperbatch";

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

static int parseShrinkMode(const char* str) {
    if (!str) return -1;
    if (strcmp(str, "manual") == 0) return lz4::SHRINK_MANUAL;
    if (strcmp(str, "immediate") == 0) return lz4::SHRINK_AUTO_IMMEDIATE;
    if (strcmp(str, "threshold") == 0) return lz4::SHRINK_AUTO_THRESHOLD;
    return -1;
}

// ============================================================================
// Binary format utilities
// ============================================================================

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
// File I/O utilities
// ============================================================================

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
// Data block structure
// ============================================================================

struct DataBlock {
    int size;
    char* data;
};

struct CompressedBlock {
    int originalSize;
    int compressedSize;
    char* data;
};

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
    printf("LZ4 Wrapper batch streaming compression/decompression tool.\n");
    printf("\n");
    printf("Input format (uncompressed):\n");
    printf("  - Header: block_count (4 bytes, little-endian)\n");
    printf("  - Per block: org_data_size (4 bytes) + org_data (org_data_size bytes)\n");
    printf("\n");
    printf("Output format (compressed):\n");
    printf("  - Header: block_count (4 bytes, little-endian)\n");
    printf("  - Per block: original_size (4 bytes) + data_size (4 bytes) + data (data_size bytes)\n");
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
    printf("  -v, --verbose          Verbose output with timing and state information\n");
    printf("  -q, --quiet            Quiet mode, suppress non-error output\n");
    printf("  --clean                Clean temporary files (no-op for this tool)\n");
    printf("  -h, --help             Show this help message\n");
    printf("\n");
    printf("SIZE suffixes: K (kilobytes), M (megabytes)\n");
    printf("\n");
    printf("Exit codes: 0=success, 1=error\n");
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
            default:
                return false;
        }
    }

    if (optind < argc) {
        opts.inputFile = argv[optind++];
    }
    if (optind < argc) {
        opts.outputFile = argv[optind++];
    }

    return true;
}

// ============================================================================
// Parse uncompressed batch format
// ============================================================================

static bool parseUncompressedBatch(const char* data, int dataSize, std::vector<DataBlock>& blocks) {
    if (dataSize < 4) {
        fprintf(stderr, "Error: Data too small for header\n");
        return false;
    }

    uint32_t blockCount = readLE32(data);
    int offset = 4;

    for (uint32_t i = 0; i < blockCount; i++) {
        if (offset + 4 > dataSize) {
            fprintf(stderr, "Error: Unexpected end of data at block %u\n", i);
            return false;
        }

        uint32_t blockSize = readLE32(data + offset);
        offset += 4;

        if (offset + static_cast<int>(blockSize) > dataSize) {
            fprintf(stderr, "Error: Block %u data exceeds file size\n", i);
            return false;
        }

        DataBlock block;
        block.size = static_cast<int>(blockSize);
        block.data = new (std::nothrow) char[blockSize];
        if (!block.data) {
            fprintf(stderr, "Error: Memory allocation failed for block %u\n", i);
            return false;
        }
        memcpy(block.data, data + offset, blockSize);
        offset += blockSize;

        blocks.push_back(block);
    }

    return true;
}

// ============================================================================
// Parse compressed batch format
// ============================================================================

static bool parseCompressedBatch(const char* data, int dataSize, std::vector<CompressedBlock>& blocks) {
    if (dataSize < 4) {
        fprintf(stderr, "Error: Data too small for header\n");
        return false;
    }

    uint32_t blockCount = readLE32(data);
    int offset = 4;

    for (uint32_t i = 0; i < blockCount; i++) {
        if (offset + 8 > dataSize) {
            fprintf(stderr, "Error: Unexpected end of data at block %u\n", i);
            return false;
        }

        uint32_t originalSize = readLE32(data + offset);
        uint32_t compressedSize = readLE32(data + offset + 4);
        offset += 8;

        if (offset + static_cast<int>(compressedSize) > dataSize) {
            fprintf(stderr, "Error: Block %u data exceeds file size\n", i);
            return false;
        }

        CompressedBlock block;
        block.originalSize = static_cast<int>(originalSize);
        block.compressedSize = static_cast<int>(compressedSize);
        block.data = new (std::nothrow) char[compressedSize];
        if (!block.data) {
            fprintf(stderr, "Error: Memory allocation failed for block %u\n", i);
            return false;
        }
        memcpy(block.data, data + offset, compressedSize);
        offset += compressedSize;

        blocks.push_back(block);
    }

    return true;
}

// ============================================================================
// Compress operation
// ============================================================================

static int doCompress(const Options& opts) {
    Timer totalTimer, blockTimer;
    totalTimer.start();

    // Read input
    int inputSize = 0;
    char* inputData = readFile(opts.inputFile, &inputSize);
    if (!inputData || inputSize <= 0) {
        fprintf(stderr, "Error: Failed to read input data\n");
        return EXIT_ERROR_CODE;
    }

    // Parse input blocks
    std::vector<DataBlock> inputBlocks;
    if (!parseUncompressedBatch(inputData, inputSize, inputBlocks)) {
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Create encoder
    lz4::Lz4Encoder encoder(opts.minWorkArea, opts.maxWorkArea, opts.shrinkMode, opts.shrinkThreshold);
    if (!encoder.isInitialized()) {
        fprintf(stderr, "Error: Failed to initialize encoder\n");
        delete[] inputData;
        for (auto& block : inputBlocks) delete[] block.data;
        return EXIT_ERROR_CODE;
    }

    // Compress blocks
    std::vector<CompressedBlock> compressedBlocks;
    long long totalOriginalSize = 0;
    long long totalCompressedSize = 0;
    double totalCompressTime = 0;

    for (size_t i = 0; i < inputBlocks.size(); i++) {
        const DataBlock& inBlock = inputBlocks[i];
        int maxCompressedSize = LZ4_compressBound(inBlock.size);

        CompressedBlock outBlock;
        outBlock.originalSize = inBlock.size;
        outBlock.data = new (std::nothrow) char[maxCompressedSize];
        if (!outBlock.data) {
            fprintf(stderr, "Error: Memory allocation failed for compressed block %zu\n", i);
            delete[] inputData;
            for (auto& block : inputBlocks) delete[] block.data;
            for (auto& block : compressedBlocks) delete[] block.data;
            return EXIT_ERROR_CODE;
        }

        blockTimer.start();
        outBlock.compressedSize = encoder.compress(outBlock.data, maxCompressedSize, inBlock.data, inBlock.size);
        blockTimer.stop();

        if (outBlock.compressedSize <= 0) {
            fprintf(stderr, "Error: Compression failed for block %zu (error code: %d)\n", i, outBlock.compressedSize);
            delete[] outBlock.data;
            delete[] inputData;
            for (auto& block : inputBlocks) delete[] block.data;
            for (auto& block : compressedBlocks) delete[] block.data;
            return EXIT_ERROR_CODE;
        }

        totalOriginalSize += inBlock.size;
        totalCompressedSize += outBlock.compressedSize;
        totalCompressTime += blockTimer.elapsedMs();

        if (opts.verbose && !opts.quiet) {
            fprintf(stderr, "Block %zu: %d -> %d bytes (%.2f%%) in %.3f ms, workArea=%d, ringBufSize=%d\n",
                    i, inBlock.size, outBlock.compressedSize,
                    100.0 * outBlock.compressedSize / inBlock.size,
                    blockTimer.elapsedMs(),
                    encoder.getWorkArea(), encoder.getRingBufferSize());
        }

        compressedBlocks.push_back(outBlock);
    }

    // Build output
    int outputSize = 4; // block_count
    for (const auto& block : compressedBlocks) {
        outputSize += 8 + block.compressedSize; // original_size + compressed_size + data
    }

    char* outputData = new (std::nothrow) char[outputSize];
    if (!outputData) {
        fprintf(stderr, "Error: Memory allocation failed for output\n");
        delete[] inputData;
        for (auto& block : inputBlocks) delete[] block.data;
        for (auto& block : compressedBlocks) delete[] block.data;
        return EXIT_ERROR_CODE;
    }

    int offset = 0;
    writeLE32(outputData + offset, static_cast<uint32_t>(compressedBlocks.size()));
    offset += 4;

    for (const auto& block : compressedBlocks) {
        writeLE32(outputData + offset, static_cast<uint32_t>(block.originalSize));
        writeLE32(outputData + offset + 4, static_cast<uint32_t>(block.compressedSize));
        memcpy(outputData + offset + 8, block.data, block.compressedSize);
        offset += 8 + block.compressedSize;
    }

    // Write output
    if (!writeFile(opts.outputFile, outputData, outputSize)) {
        fprintf(stderr, "Error: Failed to write output\n");
        delete[] inputData;
        delete[] outputData;
        for (auto& block : inputBlocks) delete[] block.data;
        for (auto& block : compressedBlocks) delete[] block.data;
        return EXIT_ERROR_CODE;
    }

    totalTimer.stop();

    // Print summary
    if (opts.verbose && !opts.quiet) {
        double ratio = 100.0 * totalCompressedSize / totalOriginalSize;
        double throughput = (totalOriginalSize / 1024.0 / 1024.0) / (totalCompressTime / 1000.0);

        fprintf(stderr, "\n--- Summary ---\n");
        fprintf(stderr, "Blocks: %zu\n", inputBlocks.size());
        fprintf(stderr, "Compressed %lld bytes -> %lld bytes (%.2f%%) in %.3f ms\n",
                totalOriginalSize, totalCompressedSize, ratio, totalCompressTime);
        fprintf(stderr, "Throughput: %.2f MB/s\n", throughput);
        fprintf(stderr, "Total time (including I/O): %.3f ms\n", totalTimer.elapsedMs());
    }

    // Cleanup
    delete[] inputData;
    delete[] outputData;
    for (auto& block : inputBlocks) delete[] block.data;
    for (auto& block : compressedBlocks) delete[] block.data;

    return EXIT_SUCCESS_CODE;
}

// ============================================================================
// Decompress operation
// ============================================================================

static int doDecompress(const Options& opts) {
    Timer totalTimer, blockTimer;
    totalTimer.start();

    // Read input
    int inputSize = 0;
    char* inputData = readFile(opts.inputFile, &inputSize);
    if (!inputData || inputSize <= 0) {
        fprintf(stderr, "Error: Failed to read input data\n");
        return EXIT_ERROR_CODE;
    }

    // Parse input blocks
    std::vector<CompressedBlock> compressedBlocks;
    if (!parseCompressedBatch(inputData, inputSize, compressedBlocks)) {
        delete[] inputData;
        return EXIT_ERROR_CODE;
    }

    // Create decoder
    lz4::Lz4Decoder decoder(opts.minWorkArea, opts.maxWorkArea, opts.shrinkMode, opts.shrinkThreshold);
    if (!decoder.isInitialized()) {
        fprintf(stderr, "Error: Failed to initialize decoder\n");
        delete[] inputData;
        for (auto& block : compressedBlocks) delete[] block.data;
        return EXIT_ERROR_CODE;
    }

    // Decompress blocks
    std::vector<DataBlock> decompressedBlocks;
    long long totalCompressedSize = 0;
    long long totalDecompressedSize = 0;
    double totalDecompressTime = 0;

    for (size_t i = 0; i < compressedBlocks.size(); i++) {
        const CompressedBlock& inBlock = compressedBlocks[i];

        // Allocate output buffer
        DataBlock outBlock;
        outBlock.size = inBlock.originalSize;
        outBlock.data = new (std::nothrow) char[inBlock.originalSize];
        if (!outBlock.data) {
            fprintf(stderr, "Error: Memory allocation failed for decompressed block %zu\n", i);
            delete[] inputData;
            for (auto& block : compressedBlocks) delete[] block.data;
            for (auto& block : decompressedBlocks) delete[] block.data;
            return EXIT_ERROR_CODE;
        }

        // Decompress directly to output buffer
        blockTimer.start();
        int decompressedSize = decoder.decompress(outBlock.data, inBlock.data, inBlock.compressedSize, inBlock.originalSize);
        blockTimer.stop();

        if (decompressedSize <= 0) {
            fprintf(stderr, "Error: Decompression failed for block %zu (error code: %d)\n", i, decompressedSize);
            delete[] outBlock.data;
            delete[] inputData;
            for (auto& block : compressedBlocks) delete[] block.data;
            for (auto& block : decompressedBlocks) delete[] block.data;
            return EXIT_ERROR_CODE;
        }

        outBlock.size = decompressedSize;
        totalCompressedSize += inBlock.compressedSize;
        totalDecompressedSize += decompressedSize;
        totalDecompressTime += blockTimer.elapsedMs();

        if (opts.verbose && !opts.quiet) {
            fprintf(stderr, "Block %zu: %d -> %d bytes in %.3f ms, workArea=%d, ringBufSize=%d\n",
                    i, inBlock.compressedSize, decompressedSize,
                    blockTimer.elapsedMs(),
                    decoder.getWorkArea(), decoder.getRingBufferSize());
        }

        decompressedBlocks.push_back(outBlock);
    }

    // Build output (uncompressed batch format)
    int outputSize = 4; // block_count
    for (const auto& block : decompressedBlocks) {
        outputSize += 4 + block.size; // org_data_size + org_data
    }

    char* outputData = new (std::nothrow) char[outputSize];
    if (!outputData) {
        fprintf(stderr, "Error: Memory allocation failed for output\n");
        delete[] inputData;
        for (auto& block : compressedBlocks) delete[] block.data;
        for (auto& block : decompressedBlocks) delete[] block.data;
        return EXIT_ERROR_CODE;
    }

    int offset = 0;
    writeLE32(outputData + offset, static_cast<uint32_t>(decompressedBlocks.size()));
    offset += 4;

    for (const auto& block : decompressedBlocks) {
        writeLE32(outputData + offset, static_cast<uint32_t>(block.size));
        memcpy(outputData + offset + 4, block.data, block.size);
        offset += 4 + block.size;
    }

    // Write output
    if (!writeFile(opts.outputFile, outputData, outputSize)) {
        fprintf(stderr, "Error: Failed to write output\n");
        delete[] inputData;
        delete[] outputData;
        for (auto& block : compressedBlocks) delete[] block.data;
        for (auto& block : decompressedBlocks) delete[] block.data;
        return EXIT_ERROR_CODE;
    }

    totalTimer.stop();

    // Print summary
    if (opts.verbose && !opts.quiet) {
        double throughput = (totalDecompressedSize / 1024.0 / 1024.0) / (totalDecompressTime / 1000.0);

        fprintf(stderr, "\n--- Summary ---\n");
        fprintf(stderr, "Blocks: %zu\n", compressedBlocks.size());
        fprintf(stderr, "Decompressed %lld bytes -> %lld bytes in %.3f ms\n",
                totalCompressedSize, totalDecompressedSize, totalDecompressTime);
        fprintf(stderr, "Throughput: %.2f MB/s\n", throughput);
        fprintf(stderr, "Total time (including I/O): %.3f ms\n", totalTimer.elapsedMs());
    }

    // Cleanup
    delete[] inputData;
    delete[] outputData;
    for (auto& block : compressedBlocks) delete[] block.data;
    for (auto& block : decompressedBlocks) delete[] block.data;

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

