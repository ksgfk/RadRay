#pragma once

#ifdef RADRAY_ENABLE_PNG

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct png_struct_def;
struct png_info_def;

typedef struct RadRayPngState {
    /* Nullable handles; initialize the entire state to zero before use. */
    struct png_struct_def* Png;
    struct png_info_def* Info;
    const unsigned char* Input;
    size_t InputRemaining;
    uint32_t Width;
    uint32_t Height;
    size_t RowBytes;
    size_t ByteSize;
    int Channels;
    int BitDepth;
    int PassCount;
    int Writing;
    char Error[256];
    char Warning[256];
} RadRayPngState;

/* State and pixel storage belong to the caller. Each operation establishes
 * its own jump point. Do not copy live state or reuse it before destroying it
 * and resetting it to zero. Destroy after either success or failure.
 * Functions return nonzero on success and never propagate C++ exceptions. */

/* Input is borrowed. A negative add_alpha disables expansion; nonnegative
 * values are clamped to the source bit depth. Image layout is returned in state. */
int radray_png_read_header(RadRayPngState* state, int64_t add_alpha);

/* Requires a successful header read and writable storage of state->ByteSize
 * bytes, zero-initialized for interlaced input. */
int radray_png_read_pixels(RadRayPngState* state, void* pixels, int flip_y);

/* Set Width/Height/Channels (3 or 4) before calling. Pixels must contain a
 * tightly packed 8-bit image. The destination file is borrowed and nonnull. */
int radray_png_write(RadRayPngState* state, FILE* destination, const void* pixels, int flip_y);

void radray_png_destroy(RadRayPngState* state);

#ifdef __cplusplus
}
#endif

#endif
