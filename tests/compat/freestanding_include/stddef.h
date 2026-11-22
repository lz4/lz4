/* <stddef.h> of a freestanding environment without libc, see compat/freestanding_frame.c */
#ifndef FREESTANDING_STDDEF_H
#define FREESTANDING_STDDEF_H
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#define NULL ((void*)0)
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
