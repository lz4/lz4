#ifndef TESTINPUT_H_INCLUDED
#define TESTINPUT_H_INCLUDED

#include <inttypes.h>

#if defined (__cplusplus)
extern "C" {
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#if defined(__cplusplus)
}
#endif
#endif
