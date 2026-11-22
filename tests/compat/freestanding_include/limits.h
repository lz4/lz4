/* <limits.h> of a freestanding environment without libc, see compat/freestanding_frame.c */
#ifndef FREESTANDING_LIMITS_H
#define FREESTANDING_LIMITS_H
#define CHAR_BIT   __CHAR_BIT__
#define SCHAR_MAX  __SCHAR_MAX__
#define UCHAR_MAX  (SCHAR_MAX * 2 + 1)
#define SHRT_MAX   __SHRT_MAX__
#define USHRT_MAX  (SHRT_MAX * 2 + 1)
#define INT_MAX    __INT_MAX__
#define INT_MIN    (-INT_MAX - 1)
#define UINT_MAX   (INT_MAX * 2U + 1U)
#define LONG_MAX   __LONG_MAX__
#define ULONG_MAX  (LONG_MAX * 2UL + 1UL)
#endif
