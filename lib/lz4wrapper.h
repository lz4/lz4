#pragma once

#include "lz4.h"
#include <cstring>
#include <stdexcept>
#include <climits>

namespace lz4 {

// ============================================================================
// Constants
// ============================================================================

enum {
    DICT_SIZE         = 64 * 1024,           // 64KB: LZ4 dictionary size (fixed)
    DEFAULT_WORK_AREA = 5 * 1024,            // 5KB: Default work area size
    DEFAULT_MAX_WORK  = INT_MAX - DICT_SIZE, // INT_MAX - DICT_SIZE: Default maximum work area
};

/**
 * Shrink mode for automatic memory management
 */
enum ShrinkMode {
    SHRINK_MANUAL = 0,       // Manual mode: user must call shrink() explicitly (default)
    SHRINK_AUTO_IMMEDIATE,   // Auto immediate: shrink after each operation if buffer expanded
    SHRINK_AUTO_THRESHOLD    // Auto threshold: shrink when buffer exceeds threshold after operation
};

enum Lz4ErrorCode {
    LZ4_ERROR_SUCCESS              = 0,
    LZ4_ERROR_FAILED               = -1,
    LZ4_ERROR_NOT_INITIALIZED      = -2,
    LZ4_ERROR_INITIALIZED_FAILED   = -3,
    LZ4_ERROR_COMPRESS_FAILED      = -4,
    LZ4_ERROR_DECOMPRESS_FAILED    = -5,
    LZ4_ERROR_PARAM_ERROR          = -6,
    LZ4_ERROR_BUFFER_RESIZE_FAILED = -7,
};

/**
 * Align size to next power of 2 starting from DEFAULT_WORK_AREA (64KB)
 * Example: 100KB -> 128KB, 200KB -> 256KB, 9MB -> 16MB (but capped by max)
 * @param size Required size
 * @return Aligned size (power of 2 * 64KB)
 */
inline int alignToPowerOf2(int size)
{
    if (size <= DEFAULT_WORK_AREA) {
        return DEFAULT_WORK_AREA;
    }

    int aligned = DEFAULT_WORK_AREA;
    while (aligned < size) {
        aligned *= 2;
    }
    return aligned;
}

/**
 * Calculate default buffer size (dict + work area)
 */
inline int calcDefaultBufferSize()
{
    return DICT_SIZE + DEFAULT_WORK_AREA;
}

/**
 * Calculate default max buffer size (dict + max work area)
 */
inline int calcDefaultMaxBufferSize()
{
    return DICT_SIZE + DEFAULT_MAX_WORK;
}

/**
 * LZ4 streaming encoder
 * 
 * Features:
 * - Dynamic buffer sizing with power-of-2 expansion: 64KB -> 128KB -> 256KB -> ...
 * - Automatic expansion when input exceeds current work area
 * - Configurable shrink mode: manual, auto-immediate, or auto-threshold
 * - User-configurable base and max buffer sizes
 * 
 * Correct ring buffer management principles:
 * 1. When offset approaches buffer end, move the last 64KB dictionary data to the beginning
 * 2. Use LZ4_saveDict to update internal dictionary pointer and dictSize
 * 3. offset is reset to 64KB, maintaining dictionary continuity
 */
class Lz4Encoder {
  public:
    /**
     * Constructor
     * @param minWorkArea Minimum allowed work area size (default: 64KB)
     * @param maxWorkArea Maximum allowed work area size (default: 10MB)
     * @param shrinkMode Shrink mode (default: SHRINK_MANUAL)
     * @param shrinkThreshold Custom shrink threshold (default: minWorkArea)
     */
    explicit Lz4Encoder(int        minWorkArea     = DEFAULT_WORK_AREA,
                        int        maxWorkArea     = DEFAULT_MAX_WORK,
                        ShrinkMode shrinkMode      = SHRINK_MANUAL,
                        int        shrinkThreshold = 0)
        : m_lz4Stream(nullptr), m_ringBuffer(nullptr), m_offset(0), m_minBufferSize(0), m_maxBufferSize(0),
          m_ringBufferSize(0), m_shrinkMode(shrinkMode), m_shrinkThreshold(0), m_initialized(false)
    {
        int threshold = (shrinkThreshold > 0) ? shrinkThreshold : minWorkArea;

        if (minWorkArea < DEFAULT_WORK_AREA) {
            minWorkArea = DEFAULT_WORK_AREA;
        }

        if (maxWorkArea < minWorkArea) {
            maxWorkArea = minWorkArea;
        }

        if (threshold < minWorkArea) {
            threshold = minWorkArea;
        }
        if (threshold > maxWorkArea) {
            threshold = maxWorkArea;
        }

        m_minBufferSize   = DICT_SIZE + minWorkArea;
        m_maxBufferSize   = DICT_SIZE + maxWorkArea;
        m_shrinkThreshold = DICT_SIZE + threshold;
        m_ringBufferSize  = m_minBufferSize;

        init();
    }

    ~Lz4Encoder()
    {
        destroy();
    }

    // Disable copy
    Lz4Encoder(const Lz4Encoder&)            = delete;
    Lz4Encoder& operator=(const Lz4Encoder&) = delete;

    // Support move
    Lz4Encoder(Lz4Encoder&& other) noexcept
        : m_lz4Stream(other.m_lz4Stream), m_ringBuffer(other.m_ringBuffer), m_offset(other.m_offset),
          m_minBufferSize(other.m_minBufferSize), m_maxBufferSize(other.m_maxBufferSize),
          m_ringBufferSize(other.m_ringBufferSize), m_shrinkMode(other.m_shrinkMode),
          m_shrinkThreshold(other.m_shrinkThreshold), m_initialized(other.m_initialized)
    {
        other.m_lz4Stream       = nullptr;
        other.m_ringBuffer      = nullptr;
        other.m_offset          = 0;
        other.m_minBufferSize   = calcDefaultBufferSize();
        other.m_maxBufferSize   = calcDefaultMaxBufferSize();
        other.m_ringBufferSize  = other.m_minBufferSize;
        other.m_shrinkMode      = SHRINK_MANUAL;
        other.m_shrinkThreshold = other.m_minBufferSize;
        other.m_initialized     = false;
    }

    Lz4Encoder& operator=(Lz4Encoder&& other) noexcept
    {
        if (this != &other) {
            destroy();
            m_lz4Stream       = other.m_lz4Stream;
            m_ringBuffer      = other.m_ringBuffer;
            m_offset          = other.m_offset;
            m_minBufferSize   = other.m_minBufferSize;
            m_maxBufferSize   = other.m_maxBufferSize;
            m_ringBufferSize  = other.m_ringBufferSize;
            m_shrinkMode      = other.m_shrinkMode;
            m_shrinkThreshold = other.m_shrinkThreshold;
            m_initialized     = other.m_initialized;

            other.m_lz4Stream       = nullptr;
            other.m_ringBuffer      = nullptr;
            other.m_offset          = 0;
            other.m_minBufferSize   = calcDefaultBufferSize();
            other.m_maxBufferSize   = calcDefaultMaxBufferSize();
            other.m_ringBufferSize  = other.m_minBufferSize;
            other.m_shrinkMode      = SHRINK_MANUAL;
            other.m_shrinkThreshold = other.m_minBufferSize;
            other.m_initialized     = false;
        }
        return *this;
    }

    /**
     * Compress a block of data
     * 
     * @param dest Output buffer (allocated by caller)
     * @param destLen Maximum length of output buffer (compressBound(srcLen))
     * @param src Input data
     * @param srcLen Input data length (will auto-expand buffer if needed)
     * @return Number of bytes after compression, returns <0 on failure
     */
    int compress(char* dest, int destLen, const char* src, int srcLen)
    {
        if (!m_initialized) {
            return LZ4_ERROR_NOT_INITIALIZED;
        }

        if (srcLen <= 0) {
            return LZ4_ERROR_PARAM_ERROR;
        }
        if (srcLen > m_maxBufferSize - DICT_SIZE) {
            return LZ4_ERROR_PARAM_ERROR;
        }

        // Check if we need to expand the buffer (input exceeds work area)
        int workArea = m_ringBufferSize - DICT_SIZE;
        if (srcLen > workArea) {
            // Calculate new size: power of 2 from 64KB
            int newWorkArea   = alignToPowerOf2(srcLen);
            int newBufferSize = DICT_SIZE + newWorkArea;

            // If exceeds max, use max
            if (newBufferSize > m_maxBufferSize) {
                newBufferSize = m_maxBufferSize;
            }

            if (!resizeBuffer(newBufferSize)) {
                return LZ4_ERROR_BUFFER_RESIZE_FAILED;
            }
        }

        // Check for ring buffer wraparound BEFORE writing
        if (m_offset + srcLen > m_ringBufferSize) {
            // Use LZ4_saveDict to save last 64KB dictionary to buffer beginning
            LZ4_saveDict(m_lz4Stream, m_ringBuffer, DICT_SIZE);
            m_offset = DICT_SIZE;
        }

        // Copy source data to current position in ring buffer
        char* ringPtr = m_ringBuffer + m_offset;
        memcpy(ringPtr, src, srcLen);

        // Use LZ4 streaming compression
        int compressedSize = LZ4_compress_limitedOutput_continue(m_lz4Stream, ringPtr, dest, srcLen, destLen);
        if (compressedSize <= 0) {
            return LZ4_ERROR_COMPRESS_FAILED;
        }

        // Update offset
        m_offset += srcLen;

        // Auto shrink if enabled
        tryAutoShrink();

        return compressedSize;
    }

    /**
     * Reset encoder state (keeps current buffer size)
     */
    void reset()
    {
        if (m_initialized && m_lz4Stream) {
            LZ4_resetStream(m_lz4Stream);
            m_offset = 0;
            if (m_ringBuffer) {
                memset(m_ringBuffer, 0, m_ringBufferSize);
            }
        }
    }

    /**
     * Shrink buffer to base size
     */
    void shrink()
    {
        if (m_initialized && m_ringBufferSize > m_minBufferSize) {
            resizeBuffer(m_minBufferSize);
        }
    }

    /**
     * Get current offset (for debugging)
     */
    int getCurrentOffset() const
    {
        return m_offset;
    }

    /**
     * Get current work area size (ringBufferSize - DICT_SIZE)
     */
    int getWorkArea() const
    {
        return m_ringBufferSize - DICT_SIZE;
    }

    /**
     * Get current ring buffer size
     */
    int getRingBufferSize() const
    {
        return m_ringBufferSize;
    }

    /**
     * Get base buffer size
     */
    int getBaseBufferSize() const
    {
        return m_minBufferSize;
    }

    /**
     * Get max buffer size
     */
    int getMaxBufferSize() const
    {
        return m_maxBufferSize;
    }

    /**
     * Get current shrink mode
     */
    ShrinkMode getShrinkMode() const
    {
        return m_shrinkMode;
    }

    /**
     * Set shrink mode
     * @param mode New shrink mode
     */
    void setShrinkMode(ShrinkMode mode)
    {
        m_shrinkMode = mode;
    }

    /**
     * Get current shrink threshold
     */
    int getShrinkThreshold() const
    {
        return m_shrinkThreshold;
    }

    /**
     * Set shrink threshold
     * @param threshold New threshold (must be between minBufferSize and maxBufferSize)
     */
    void setShrinkThreshold(int threshold)
    {
        int minWork = m_minBufferSize - DICT_SIZE;
        int maxWork = m_maxBufferSize - DICT_SIZE;
        if (threshold < minWork || threshold > maxWork) {
            return;
        }
        m_shrinkThreshold = DICT_SIZE + threshold;
    }

    bool isInitialized() const
    {
        return m_initialized;
    }

  private:
    /**
     * Try to auto shrink based on current shrink mode
     */
    void tryAutoShrink()
    {
        if (m_shrinkMode == SHRINK_MANUAL) {
            return;
        }

        if (m_ringBufferSize <= m_minBufferSize) {
            return;
        }

        if (m_shrinkMode == SHRINK_AUTO_IMMEDIATE) {
            shrink();
        } else if (m_shrinkMode == SHRINK_AUTO_THRESHOLD) {
            if (m_ringBufferSize > m_shrinkThreshold) {
                shrink();
            }
        }
    }

    int init()
    {
        if (m_initialized) {
            return LZ4_ERROR_SUCCESS;
        }

        m_lz4Stream = LZ4_createStream();
        if (!m_lz4Stream) {
            return LZ4_ERROR_INITIALIZED_FAILED;
        }

        m_ringBuffer = new (std::nothrow) char[m_ringBufferSize];
        if (!m_ringBuffer) {
            LZ4_freeStream(m_lz4Stream);
            m_lz4Stream = nullptr;
            return LZ4_ERROR_INITIALIZED_FAILED;
        }

        memset(m_ringBuffer, 0, m_ringBufferSize);
        m_offset      = 0;
        m_initialized = true;

        return LZ4_ERROR_SUCCESS;
    }

    /**
     * Resize ring buffer to new size
     * Uses LZ4_saveDict to preserve dictionary continuity
     * @param newBufferSize New buffer size (must include DICT_SIZE)
     * @return true on success, false on failure
     */
    bool resizeBuffer(int newBufferSize)
    {
        char* newBuffer = new (std::nothrow) char[newBufferSize];
        if (!newBuffer) {
            return false;
        }

        memset(newBuffer, 0, newBufferSize);

        int newOffset = 0;

        // Preserve dictionary using LZ4_saveDict
        if (m_ringBuffer && m_offset > 0 && m_lz4Stream) {
            int dictSize  = (m_offset < DICT_SIZE) ? m_offset : DICT_SIZE;
            int dictStart = (m_offset > DICT_SIZE) ? (m_offset - DICT_SIZE) : 0;
            memcpy(newBuffer, m_ringBuffer + dictStart, dictSize);
            LZ4_saveDict(m_lz4Stream, newBuffer, dictSize);
            newOffset = dictSize;
        }

        if (m_ringBuffer) {
            delete[] m_ringBuffer;
        }

        m_ringBuffer     = newBuffer;
        m_ringBufferSize = newBufferSize;
        m_offset         = newOffset;

        return true;
    }

    void destroy()
    {
        if (m_initialized) {
            if (m_lz4Stream) {
                LZ4_freeStream(m_lz4Stream);
                m_lz4Stream = nullptr;
            }
            if (m_ringBuffer) {
                delete[] m_ringBuffer;
                m_ringBuffer = nullptr;
            }
            m_offset          = 0;
            m_minBufferSize   = calcDefaultBufferSize();
            m_maxBufferSize   = calcDefaultMaxBufferSize();
            m_ringBufferSize  = m_minBufferSize;
            m_shrinkMode      = SHRINK_MANUAL;
            m_shrinkThreshold = m_minBufferSize;
            m_initialized     = false;
        }
    }

  private:
    LZ4_stream_t* m_lz4Stream;
    char*         m_ringBuffer;
    int           m_offset;
    int           m_minBufferSize;    // Minimum buffer size (dict + min work area)
    int           m_maxBufferSize;    // Maximum allowed buffer size
    int           m_ringBufferSize;   // Current buffer size
    ShrinkMode    m_shrinkMode;
    int           m_shrinkThreshold;  // Threshold for SHRINK_AUTO_THRESHOLD mode
    bool          m_initialized;
};

/**
 * LZ4 streaming decoder
 * 
 * Features:
 * - Dynamic buffer sizing with power-of-2 expansion: 64KB -> 128KB -> 256KB -> ...
 * - Automatic expansion when originalSize exceeds current work area
 * - Configurable shrink mode: manual, auto-immediate, or auto-threshold
 * - User-configurable base and max buffer sizes
 */
class Lz4Decoder {
  public:
    /**
     * Constructor
     * @param minWorkArea Minimum allowed work area size (default: 64KB)
     * @param maxWorkArea Maximum allowed work area size (default: 10MB)
     * @param shrinkMode Shrink mode (default: SHRINK_MANUAL)
     * @param shrinkThreshold Custom shrink threshold (default: minWorkArea)
     */
    explicit Lz4Decoder(int        minWorkArea     = DEFAULT_WORK_AREA,
                        int        maxWorkArea     = DEFAULT_MAX_WORK,
                        ShrinkMode shrinkMode      = SHRINK_MANUAL,
                        int        shrinkThreshold = 0)
        : m_lz4Stream(nullptr), m_ringBuffer(nullptr), m_offset(0), m_minBufferSize(0), m_maxBufferSize(0),
          m_ringBufferSize(0), m_shrinkMode(shrinkMode), m_shrinkThreshold(0), m_initialized(false)
    {
        int threshold = (shrinkThreshold > 0) ? shrinkThreshold : minWorkArea;

        if (minWorkArea < DEFAULT_WORK_AREA) {
            minWorkArea = DEFAULT_WORK_AREA;
        }

        if (maxWorkArea < minWorkArea) {
            maxWorkArea = minWorkArea;
        }

        if (threshold < minWorkArea) {
            threshold = minWorkArea;
        }
        if (threshold > maxWorkArea) {
            threshold = maxWorkArea;
        }

        m_minBufferSize   = DICT_SIZE + minWorkArea;
        m_maxBufferSize   = DICT_SIZE + maxWorkArea;
        m_shrinkThreshold = DICT_SIZE + threshold;
        m_ringBufferSize  = m_minBufferSize;

        init();
    }

    ~Lz4Decoder()
    {
        destroy();
    }

    // Disable copy
    Lz4Decoder(const Lz4Decoder&)            = delete;
    Lz4Decoder& operator=(const Lz4Decoder&) = delete;

    // Support move
    Lz4Decoder(Lz4Decoder&& other) noexcept
        : m_lz4Stream(other.m_lz4Stream), m_ringBuffer(other.m_ringBuffer), m_offset(other.m_offset),
          m_minBufferSize(other.m_minBufferSize), m_maxBufferSize(other.m_maxBufferSize),
          m_ringBufferSize(other.m_ringBufferSize), m_shrinkMode(other.m_shrinkMode),
          m_shrinkThreshold(other.m_shrinkThreshold), m_initialized(other.m_initialized)
    {
        other.m_lz4Stream       = nullptr;
        other.m_ringBuffer      = nullptr;
        other.m_offset          = 0;
        other.m_minBufferSize   = calcDefaultBufferSize();
        other.m_maxBufferSize   = calcDefaultMaxBufferSize();
        other.m_ringBufferSize  = other.m_minBufferSize;
        other.m_shrinkMode      = SHRINK_MANUAL;
        other.m_shrinkThreshold = other.m_minBufferSize;
        other.m_initialized     = false;
    }

    Lz4Decoder& operator=(Lz4Decoder&& other) noexcept
    {
        if (this != &other) {
            destroy();
            m_lz4Stream       = other.m_lz4Stream;
            m_ringBuffer      = other.m_ringBuffer;
            m_offset          = other.m_offset;
            m_minBufferSize   = other.m_minBufferSize;
            m_maxBufferSize   = other.m_maxBufferSize;
            m_ringBufferSize  = other.m_ringBufferSize;
            m_shrinkMode      = other.m_shrinkMode;
            m_shrinkThreshold = other.m_shrinkThreshold;
            m_initialized     = other.m_initialized;

            other.m_lz4Stream       = nullptr;
            other.m_ringBuffer      = nullptr;
            other.m_offset          = 0;
            other.m_minBufferSize   = calcDefaultBufferSize();
            other.m_maxBufferSize   = calcDefaultMaxBufferSize();
            other.m_ringBufferSize  = other.m_minBufferSize;
            other.m_shrinkMode      = SHRINK_MANUAL;
            other.m_shrinkThreshold = other.m_minBufferSize;
            other.m_initialized     = false;
        }
        return *this;
    }

    /**
     * Decompress a block of data
     * 
     * @param dest Output buffer (allocated by caller, size must be >= originalSize)
     * @param src Compressed data
     * @param srcLen Compressed data length
     * @param originalSize Expected original size (also the required dest buffer size)
     * @return Number of bytes after decompression, returns <0 on failure
     */
    int decompress(char* dest, const char* src, int srcLen, int originalSize)
    {
        if (!m_initialized) {
            return LZ4_ERROR_NOT_INITIALIZED;
        }

        if (dest == nullptr || srcLen <= 0 || originalSize <= 0) {
            return LZ4_ERROR_PARAM_ERROR;
        }
        if (originalSize > m_maxBufferSize - DICT_SIZE) {
            return LZ4_ERROR_PARAM_ERROR;
        }

        // Check if we need to expand the buffer (originalSize exceeds work area)
        int workArea = m_ringBufferSize - DICT_SIZE;
        if (originalSize > workArea) {
            // Calculate new size: power of 2 from 64KB
            int newWorkArea   = alignToPowerOf2(originalSize);
            int newBufferSize = DICT_SIZE + newWorkArea;

            // If exceeds max, use max
            if (newBufferSize > m_maxBufferSize) {
                newBufferSize = m_maxBufferSize;
            }

            if (!resizeBuffer(newBufferSize)) {
                return LZ4_ERROR_BUFFER_RESIZE_FAILED;
            }
        }

        // Check for ring buffer wraparound BEFORE decompression
        if (m_offset + originalSize > m_ringBufferSize) {
            const int dictStart = m_offset - DICT_SIZE;
            if (dictStart > 0) {
                memmove(m_ringBuffer, m_ringBuffer + dictStart, DICT_SIZE);
            }
            LZ4_setStreamDecode(m_lz4Stream, m_ringBuffer, DICT_SIZE);
            m_offset = DICT_SIZE;
        }

        // Decompress to ring buffer (maintains dictionary continuity)
        char* ringPtr        = m_ringBuffer + m_offset;
        int decompressedSize = LZ4_decompress_safe_continue(m_lz4Stream, src, ringPtr, srcLen, originalSize);
        if (decompressedSize <= 0) {
            return LZ4_ERROR_DECOMPRESS_FAILED;
        }

        // Copy to user's buffer
        memcpy(dest, ringPtr, decompressedSize);

        // Update offset
        m_offset += decompressedSize;

        // Auto shrink if enabled (safe now since data is copied to user's buffer)
        tryAutoShrink();

        return decompressedSize;
    }

    /**
     * Reset decoder state (keeps current buffer size)
     */
    void reset()
    {
        if (m_initialized) {
            m_offset = 0;
            if (m_lz4Stream) {
                LZ4_freeStreamDecode(m_lz4Stream);
                m_lz4Stream = LZ4_createStreamDecode();
            }
            if (m_ringBuffer) {
                memset(m_ringBuffer, 0, m_ringBufferSize);
            }
        }
    }

    /**
     * Shrink buffer to base size
     */
    void shrink()
    {
        if (m_initialized && m_ringBufferSize > m_minBufferSize) {
            resizeBuffer(m_minBufferSize);
        }
    }

    /**
     * Get current offset (for debugging)
     */
    int getCurrentOffset() const
    {
        return m_offset;
    }

    /**
     * Get current work area size (ringBufferSize - DICT_SIZE)
     */
    int getWorkArea() const
    {
        return m_ringBufferSize - DICT_SIZE;
    }

    /**
     * Get current ring buffer size
     */
    int getRingBufferSize() const
    {
        return m_ringBufferSize;
    }

    /**
     * Get base buffer size
     */
    int getBaseBufferSize() const
    {
        return m_minBufferSize;
    }

    /**
     * Get max buffer size
     */
    int getMaxBufferSize() const
    {
        return m_maxBufferSize;
    }

    /**
     * Get current shrink mode
     */
    ShrinkMode getShrinkMode() const
    {
        return m_shrinkMode;
    }

    /**
     * Set shrink mode
     * @param mode New shrink mode
     */
    void setShrinkMode(ShrinkMode mode)
    {
        m_shrinkMode = mode;
    }

    /**
     * Get current shrink threshold
     */
    int getShrinkThreshold() const
    {
        return m_shrinkThreshold;
    }

    /**
     * Set shrink threshold
     * @param threshold New threshold (must be between minBufferSize and maxBufferSize)
     */
    void setShrinkThreshold(int threshold)
    {
        int minWork = m_minBufferSize - DICT_SIZE;
        int maxWork = m_maxBufferSize - DICT_SIZE;
        if (threshold < minWork || threshold > maxWork) {
            return;
        }
        m_shrinkThreshold = DICT_SIZE + threshold;
    }

    bool isInitialized() const
    {
        return m_initialized;
    }

  private:
    /**
     * Try to auto shrink based on current shrink mode
     */
    void tryAutoShrink()
    {
        if (m_shrinkMode == SHRINK_MANUAL) {
            return;
        }

        if (m_ringBufferSize <= m_minBufferSize) {
            return;
        }

        if (m_shrinkMode == SHRINK_AUTO_IMMEDIATE) {
            shrink();
        } else if (m_shrinkMode == SHRINK_AUTO_THRESHOLD) {
            if (m_ringBufferSize > m_shrinkThreshold) {
                shrink();
            }
        }
    }

    int init()
    {
        if (m_initialized) {
            return LZ4_ERROR_SUCCESS;
        }

        m_lz4Stream = LZ4_createStreamDecode();
        if (!m_lz4Stream) {
            return LZ4_ERROR_INITIALIZED_FAILED;
        }

        m_ringBuffer = new (std::nothrow) char[m_ringBufferSize];
        if (!m_ringBuffer) {
            LZ4_freeStreamDecode(m_lz4Stream);
            m_lz4Stream = nullptr;
            return LZ4_ERROR_INITIALIZED_FAILED;
        }

        memset(m_ringBuffer, 0, m_ringBufferSize);
        m_offset      = 0;
        m_initialized = true;

        return LZ4_ERROR_SUCCESS;
    }

    /**
     * Resize ring buffer to new size
     * Uses LZ4_setStreamDecode to preserve dictionary continuity
     * @param newBufferSize New buffer size (must include DICT_SIZE)
     * @return true on success, false on failure
     */
    bool resizeBuffer(int newBufferSize)
    {
        char* newBuffer = new (std::nothrow) char[newBufferSize];
        if (!newBuffer) {
            return false;
        }

        memset(newBuffer, 0, newBufferSize);

        int newOffset = 0;

        // Preserve dictionary
        if (m_ringBuffer && m_offset > 0) {
            int dictSize  = (m_offset < DICT_SIZE) ? m_offset : DICT_SIZE;
            int dictStart = (m_offset > DICT_SIZE) ? (m_offset - DICT_SIZE) : 0;

            memcpy(newBuffer, m_ringBuffer + dictStart, dictSize);
            newOffset = dictSize;

            if (m_lz4Stream) {
                LZ4_setStreamDecode(m_lz4Stream, newBuffer, dictSize);
            }
        }

        if (m_ringBuffer) {
            delete[] m_ringBuffer;
        }

        m_ringBuffer     = newBuffer;
        m_ringBufferSize = newBufferSize;
        m_offset         = newOffset;

        return true;
    }

    void destroy()
    {
        if (m_initialized) {
            if (m_lz4Stream) {
                LZ4_freeStreamDecode(m_lz4Stream);
                m_lz4Stream = nullptr;
            }
            if (m_ringBuffer) {
                delete[] m_ringBuffer;
                m_ringBuffer = nullptr;
            }
            m_offset          = 0;
            m_minBufferSize   = calcDefaultBufferSize();
            m_maxBufferSize   = calcDefaultMaxBufferSize();
            m_ringBufferSize  = m_minBufferSize;
            m_shrinkMode      = SHRINK_MANUAL;
            m_shrinkThreshold = m_minBufferSize;
            m_initialized     = false;
        }
    }

  private:
    LZ4_streamDecode_t* m_lz4Stream;
    char*               m_ringBuffer;
    int                 m_offset;
    int                 m_minBufferSize;    // Minimum buffer size (dict + min work area)
    int                 m_maxBufferSize;    // Maximum allowed buffer size
    int                 m_ringBufferSize;   // Current buffer size
    ShrinkMode          m_shrinkMode;
    int                 m_shrinkThreshold;  // Threshold for SHRINK_AUTO_THRESHOLD mode
    bool                m_initialized;
};

} // namespace lz4
