/*
 * lz4_bounds_safety.h - portability macros for optional -fbounds-safety
 *
 * Copyright (c) 2026 Jeff Bindel <jeff@incrediblybased.co>
 *
 * BSD 2-Clause License (same as the LZ4 library; see LICENSE in this directory).
 *
 * When LZ4_SUPPORT_FBOUNDS_SAFETY is defined (typically via
 * -DLZ4_SUPPORT_FBOUNDS_SAFETY and a Clang toolchain that implements
 * -fbounds-safety), these macros expand to Clang bounds annotations.
 * Otherwise they expand to nothing so default builds are unchanged.
 *
 * Pattern matches libwebp / libpng / giflib inert-macro -fbounds-safety
 * adoption: annotations are inert unless explicitly enabled.
 */
#ifndef LZ4_BOUNDS_SAFETY_H_2983827168210
#define LZ4_BOUNDS_SAFETY_H_2983827168210

#ifdef LZ4_SUPPORT_FBOUNDS_SAFETY

#  include <ptrcheck.h>
/* Non-ABI-breaking sized-by annotations for buffer pointer parameters.
 * Prefer LZ4_SIZED_BY for byte buffers whose companion argument is a
 * capacity (compressedSize / dstCapacity). Use *_OR_NULL when the
 * pointer may be NULL while the companion size is zero.
 */
#  define LZ4_SIZED_BY(n) __sized_by(n)
#  define LZ4_SIZED_BY_OR_NULL(n) __sized_by_or_null(n)
#  define LZ4_COUNTED_BY(n) __counted_by(n)
#  define LZ4_COUNTED_BY_OR_NULL(n) __counted_by_or_null(n)

#else /* !LZ4_SUPPORT_FBOUNDS_SAFETY */

#  define LZ4_SIZED_BY(n)
#  define LZ4_SIZED_BY_OR_NULL(n)
#  define LZ4_COUNTED_BY(n)
#  define LZ4_COUNTED_BY_OR_NULL(n)

#endif /* LZ4_SUPPORT_FBOUNDS_SAFETY */

#endif /* LZ4_BOUNDS_SAFETY_H_2983827168210 */
