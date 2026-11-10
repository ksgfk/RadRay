#pragma once

#ifdef RADRAY_ENABLE_JPEG

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct RadRayJpegDecoder;

typedef struct RadRayJpegState {
    /* Nullable handle; initialize the entire state to zero before use. */
    struct RadRayJpegDecoder* Decoder;
    const unsigned char* Input;
    size_t InputSize;
    uint32_t Width;
    uint32_t Height;
    size_t RowBytes;
    size_t ByteSize;
    int Channels;
    char Error[256];
    char Warning[256];
} RadRayJpegState;

/* State, input and pixel storage belong to the caller. Input must remain valid
 * until destroy. Do not copy live state or reuse it before destroying it and
 * resetting it to zero. Destroy after either success or failure.
 * Each read establishes its own jump point and returns nonzero on success. */

/* A negative add_alpha selects RGB8; nonnegative values select RGBA8 with
 * alpha clamped to 255. Starts decompression and returns checked layout in state. */
int radray_jpeg_read_header(RadRayJpegState* state, int64_t add_alpha);

/* Requires a successful header read and writable storage of state->ByteSize
 * bytes. Rows are written directly; flip_y reverses their destination order. */
int radray_jpeg_read_pixels(RadRayJpegState* state, void* pixels, int flip_y);

void radray_jpeg_destroy(RadRayJpegState* state);

#ifdef __cplusplus
}
#endif

#endif
