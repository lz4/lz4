/*
  LZ4iomt.cpp - LZ4 File/Stream Interface with multi thread support
  Copyright (C) yumeyao 2020
  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - LZ4 source repository : https://github.com/lz4/lz4
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user code of the LZ4 library.
  - The license of LZ4 library is BSD.
  - The license of xxHash library is BSD.
  - The license of this source file is GPLv2.
*/

#include <thread>
#include <condition_variable>
#include <vector>
#include <deque>
#include <utility>
extern "C" {
#include "lz4io.c"
}
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

static unsigned parallel = 0;

void LZ4IO_setParallel(unsigned n) {
    parallel = n;
}

static unsigned LZ4IO_getAvailableCores() {
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
}

int LZ4IO_multiThreadAvailable() {
    if (parallel == 0)
    {
        parallel = LZ4IO_getAvailableCores();
    }
    return parallel - 1;
}

namespace LZ4IOMT {

struct LZ4IOHandler {
    virtual size_t write(const void* buffer, size_t size) = 0;
    virtual size_t read(void* buffer, size_t size) = 0;
    virtual const char* readError() = 0;
    virtual ~LZ4IOHandler() {}
};

struct LZ4IOFileInfo {
    FILE* srcFile;
    const char* srcFileName;
    FILE* dstFile;
};

struct LZ4IOFileHandler : LZ4IOHandler, LZ4IOFileInfo {
    LZ4IOFileHandler(const LZ4IOFileInfo& info) : LZ4IOFileInfo(info) {}
    size_t write(const void* buffer, size_t size) override { return fwrite(buffer, 1, size, dstFile); }
    size_t read(void* buffer, size_t size) override { return fread(buffer, 1, size, srcFile); }
    const char* readError() override { return ferror(srcFile) ? srcFileName : nullptr; }
};

struct WorkCtx {
    WorkCtx(LZ4IOHandler& info, const cRess_t* pRess, const LZ4F_preferences_t* pPrefs, unsigned nThreads) 
        : io(info), compressedFileSize(0), contentSize(pPrefs->frameInfo.contentSize), blockSize(pRess->srcBufferSize)
        , dstBufferSize(pRess->dstBufferSize), prefs(*pPrefs), contentChecksumFlag(pPrefs->frameInfo.contentChecksumFlag)
    {
        void* const dstBuffer = pRess->dstBuffer;

        /* Write Archive Header */
        size_t headerSize = LZ4F_compressBegin_usingCDict(pRess->ctx, pRess->dstBuffer, dstBufferSize, pRess->cdict, pPrefs);
        if (LZ4F_isError(headerSize)) EXM_THROW(33, "File header generation failed : %s", LZ4F_getErrorName(headerSize));
        { size_t const sizeCheck = io.write(dstBuffer, headerSize);
            if (sizeCheck!=headerSize) EXM_THROW(34, "Write error : cannot write header"); }

        compressedFileSize += headerSize;
        prefs.frameInfo.contentSize = 0;
        prefs.frameInfo.contentChecksumFlag = LZ4F_noContentChecksum;

        contentSize = doWork(pRess->srcBuffer, pRess->dstBuffer, nThreads);
    }
    unsigned long long getCompressedFileSize() const { return compressedFileSize; }
    unsigned long long getContentSize() const { return contentSize; }
private:
    LZ4IOHandler& io;
    unsigned long long compressedFileSize;
    unsigned long long contentSize;
    size_t blockSize;
    size_t dstBufferSize;
    LZ4F_preferences_t prefs;
    LZ4F_contentChecksum_t contentChecksumFlag;

    struct Block {
        void* src;
        void* dst;
        unsigned srcSize;
        unsigned dstSize;
    };

    std::deque<Block> blocks;
    std::vector<Block*> pendingBlocks;
    std::condition_variable cv_compress;
    std::condition_variable cv_writeback;
    std::mutex mtx;
    // std::vector<void*> freeSrcBuffers;
    // std::vector<void*> freeDstBuffers;

    unsigned state;
    enum { SOURCE_EOF = 1 };
    bool isEOF() const { return state & SOURCE_EOF; }
    void setEOF() {
        state |= SOURCE_EOF;
        std::unique_lock<std::mutex> lock(mtx);
        cv_compress.notify_all();
    }
    Block* getPendingBlock() { //blocking call
        std::unique_lock<std::mutex> lock(mtx);
        cv_writeback.notify_one(); // notify some compression is done
        if (pendingBlocks.empty() && isEOF()) return nullptr;
        cv_compress.wait(lock, [this]() {
            return !pendingBlocks.empty() || isEOF();
        });
        if (!pendingBlocks.empty()) {
            Block* p = pendingBlocks[0];
            pendingBlocks.erase(pendingBlocks.begin());
            return p;
        }
        // is EOF
        return 0;
    }
    void pushPendingBlock(Block& block) { std::lock_guard<std::mutex> _(mtx); pendingBlocks.push_back(&block); cv_compress.notify_one(); }
    void* getDstBuffer() { return malloc(dstBufferSize); }
    void* getSrcBuffer() { return malloc(blockSize); }

    struct ThreadArg {
        WorkCtx* workCtx;
        Block* initialBlock;
    };

    void worker(Block* initialBlock);
    static void workerThread(void* arg);
    unsigned long long doWork(void* firstSrc, void* firstDst, unsigned nThreads);

    void freeBuffer(void* p) { free(p); }
    void freeBuffer(void* p, void* nofreeAddress) { if (p != nofreeAddress) free(p); }
};

static unsigned getFrameHeaderSize(const LZ4F_preferences_t& prefs) {
    return 7
        //+ (prefs.frameInfo.contentSize ? 8 : 0) // always 0
        + (prefs.frameInfo.dictID ? 4 : 0);
}

inline void WorkCtx::worker(Block* block) {
    do {
        block->dst = block->dst ? block->dst : getDstBuffer();
        size_t outSize = LZ4F_compressFrame(block->dst, dstBufferSize, block->src, block->srcSize, &prefs);
        if (LZ4F_isError(outSize)) EXM_THROW(35, "Compression failed : %s", LZ4F_getErrorName(outSize));
        block->dstSize = (unsigned)outSize;
    } while ((block = getPendingBlock()));
}

void WorkCtx::workerThread(void* arg_) {
    ThreadArg& arg = *static_cast<ThreadArg*>(arg_);
    arg.workCtx->worker(arg.initialBlock);
}

unsigned long long WorkCtx::doWork(void* firstSrc, void* firstDst, unsigned nThreads) {
    std::vector<std::pair<std::thread, ThreadArg>> threads;
    threads.reserve(nThreads);

    unsigned long long blockIndex = 0;
    unsigned long long finishedBlock = 0;
    unsigned long long hashedBlock = 0;

    void* src = firstSrc;
    void* dst = firstDst;
    unsigned srcSize = blockSize;

    state = 0;
    XXH32_state_t xxh;
    if (contentChecksumFlag) XXH32_reset(&xxh, 0);

    auto needMoreBlocks = [&]() { return !isEOF() && blocks.size() < 2 * nThreads; };

    while (true) {
        if (needMoreBlocks()) {
            if (blockIndex > 0) {
                src = getSrcBuffer();
                size_t size = io.read(src, blockSize);
                if (size < blockSize) {
                    const char *srcFileName = io.readError();
                    if (srcFileName) EXM_THROW(37, "Error reading %s ", srcFileName);
                    setEOF();
                }
                srcSize = size;
                if (size == 0) {
                    freeBuffer(src);
                    continue;
                }
                dst = nullptr;
            }

            blocks.emplace_back(Block{src, dst, srcSize, (unsigned)0});
            ++blockIndex;

            if (blockIndex <= nThreads) {
                threads.emplace_back();
                threads.back().second = { this, &blocks.back() };
                threads.back().first = std::thread(workerThread, &threads.back().second);
                if (blocks.front().dstSize == 0) continue; // feed more source blocks
            }
            else {
                pushPendingBlock(blocks.back());
            }
        }
        else {
            if (isEOF() && finishedBlock == blockIndex) break;
        }

        while (!blocks.empty()) {
            if (blocks.front().dstSize != 0) {
                Block& block = blocks.front();
                unsigned headerSize = getFrameHeaderSize(prefs);
                unsigned writeSize = block.dstSize - headerSize - 4;
                size_t result = io.write(((char*)block.dst) + headerSize, writeSize);
                if (result < writeSize) EXM_THROW(36, "Write error : cannot write compressed block");
                compressedFileSize += writeSize;
                if (contentChecksumFlag && finishedBlock == hashedBlock) {
                    (void)XXH32_update(&xxh, block.src, block.srcSize);
                    ++hashedBlock;
                }
                ++finishedBlock;
                freeBuffer(block.src, firstSrc);
                freeBuffer(block.dst, firstDst);
                unsigned lastchunkSize = block.srcSize;
                blocks.pop_front();
                if (blocks.empty() || blocks.front().dstSize == 0) {
                    unsigned long long filesize = (finishedBlock - 1) * blockSize + lastchunkSize;
                    DISPLAYUPDATE(2, "\rRead : %u MB   ==> %.2f%%   ", (unsigned)(filesize>>20), (double)compressedFileSize/filesize*100);
                }
                continue;
            }
            if (needMoreBlocks()) {
                break;
            }
            if (contentChecksumFlag) {
                for (auto iter = blocks.begin() + (hashedBlock - finishedBlock); iter != blocks.end(); ++iter) {
                    auto& block = *iter;
                    (void)XXH32_update(&xxh, block.src, block.srcSize);
                    ++hashedBlock;
                    if (blocks.front().dstSize != 0) break;
                }
                if (blocks.front().dstSize != 0) continue;
            }
            std::unique_lock<std::mutex> lock(mtx);
            cv_writeback.wait(lock, [this]() {
                return blocks.front().dstSize != 0;
            });
        }
    };

    unsigned long long readSize = (finishedBlock - 1) * blockSize + srcSize;
    {
        if (contentSize != 0 && contentSize != readSize) EXM_THROW(38, "End of file generation failed : %s", LZ4F_getErrorName(LZ4F_errorCode_t(0) - LZ4F_ERROR_frameSize_wrong));

        char frameend[8] = {0};
        size_t writeSize = 4;
        if (contentChecksumFlag) {
            U32 const checksum = XXH32_digest(&xxh);
            LZ4IO_writeLE32(&frameend[4], checksum);
            writeSize = 8;
        }
        size_t result = io.write(frameend, writeSize);
        if (result < writeSize) EXM_THROW(39, "Write error : cannot write end of stream");
        compressedFileSize += writeSize;
    }
    for (auto&& thread : threads) thread.first.join();
    return readSize;
}



}


unsigned long long LZ4IO_compressFilename_extRess_multithread(const cRess_t* pRess, FILE* srcFile, const char* srcFileName, FILE* dstFile,
                                                              const LZ4F_preferences_t* pPrefs, unsigned long long* pCompressedfilesize)
{
    DISPLAYLEVEL(4, "Using Multithread mode\n");

    LZ4IOMT::LZ4IOFileHandler ioInfo = LZ4IOMT::LZ4IOFileInfo{srcFile, srcFileName, dstFile};
    LZ4IOMT::WorkCtx workers(ioInfo, pRess, pPrefs, parallel);

    *pCompressedfilesize = workers.getCompressedFileSize();
    return workers.getContentSize();
}
