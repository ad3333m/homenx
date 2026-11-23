// SPDX-License-Identifier: MIT
// Eliminate unused FFmpeg tx_double.o and tx_int32.o codelet lists to save ~16 MB of BSS
// in the background sysmodule while keeping real av_tx_init (float MDCT/FFT + ARM64 NEON)
// intact for Opus, AAC, and Vorbis audio decoding.

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

const void * const ff_tx_codelet_list_double_c[] = { NULL };
const void * const ff_tx_codelet_list_int32_c[] = { NULL };

// FFmpeg calls newlib memalign, but this process links Stratosphère’s free
// and realloc. Use its aligned_alloc so all three use the same heap.
void *__wrap_memalign(size_t alignment, size_t size) {
    if (!alignment || (alignment & (alignment - 1))) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (size > SIZE_MAX - (alignment - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    const size_t rounded = (size + alignment - 1) & ~(alignment - 1);
    return aligned_alloc(alignment, rounded ? rounded : alignment);
}
