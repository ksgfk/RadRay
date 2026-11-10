#ifdef __cplusplus
#error jpeg_codec.c must be compiled as C
#endif

#include "jpeg_codec.h"

#ifdef RADRAY_ENABLE_JPEG

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

#if BITS_IN_JSAMPLE != 8
#error jpeg_codec.c requires 8-bit libjpeg samples
#endif

_Static_assert(sizeof(((RadRayJpegState*)0)->Error) >= JMSG_LENGTH_MAX, "JPEG error buffer is too small");
_Static_assert(sizeof(((RadRayJpegState*)0)->Warning) >= JMSG_LENGTH_MAX, "JPEG warning buffer is too small");

struct RadRayJpegDecoder {
    struct jpeg_decompress_struct Info;
    struct jpeg_error_mgr ErrorManager;
    jmp_buf Jump;
    unsigned char Alpha;
};

static int fail(RadRayJpegState* state, const char* message) {
    size_t length = strlen(message);
    if (length >= sizeof(state->Error)) {
        length = sizeof(state->Error) - 1;
    }
    memcpy(state->Error, message, length);
    state->Error[length] = '\0';
    return 0;
}

static void on_error(j_common_ptr info) {
    RadRayJpegState* state = info->client_data;
    info->err->format_message(info, state->Error);
    longjmp(state->Decoder->Jump, 1);
}

static void on_warning(j_common_ptr info) {
    RadRayJpegState* state = info->client_data;
    if (state->Warning[0] == '\0') {
        info->err->format_message(info, state->Warning);
    }
}

int radray_jpeg_read_header(RadRayJpegState* state, int64_t add_alpha) {
    struct RadRayJpegDecoder* decoder;
    if (state->Input == NULL || state->InputSize == 0) {
        return fail(state, "JPEG input is empty");
    }
#ifdef RADRAY_ENABLE_MIMALLOC
    decoder = mi_zalloc(sizeof(*decoder));
#else
    decoder = calloc(1, sizeof(*decoder));
#endif
    if (decoder == NULL) {
        return fail(state, "JPEG decoder allocation failed");
    }
    state->Decoder = decoder;
    decoder->Info.err = jpeg_std_error(&decoder->ErrorManager);
    decoder->ErrorManager.error_exit = on_error;
    decoder->ErrorManager.output_message = on_warning;
    decoder->Info.client_data = state;
    if (setjmp(decoder->Jump) != 0) {
        return 0;
    }

    jpeg_create_decompress(&decoder->Info);
    jpeg_mem_src(&decoder->Info, state->Input, state->InputSize);
    if (jpeg_read_header(&decoder->Info, TRUE) != JPEG_HEADER_OK) {
        return fail(state, "JPEG image header is incomplete");
    }
    decoder->Info.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&decoder->Info)) {
        return fail(state, "JPEG decompression did not start");
    }
    if (decoder->Info.output_width == 0 || decoder->Info.output_height == 0 ||
        decoder->Info.output_components != 3) {
        return fail(state, "unsupported JPEG dimensions or pixel format");
    }

    state->Width = decoder->Info.output_width;
    state->Height = decoder->Info.output_height;
    state->Channels = add_alpha >= 0 ? 4 : 3;
    decoder->Alpha = add_alpha > 255 ? 255 : (unsigned char)add_alpha;
    if (state->Width > SIZE_MAX / (size_t)state->Channels) {
        return fail(state, "JPEG row size exceeds supported storage");
    }
    state->RowBytes = (size_t)state->Width * (size_t)state->Channels;
    if (state->Height > SIZE_MAX / state->RowBytes) {
        return fail(state, "JPEG image size exceeds supported storage");
    }
    state->ByteSize = state->RowBytes * state->Height;
    return 1;
}

int radray_jpeg_read_pixels(RadRayJpegState* state, void* pixels, int flip_y) {
    struct RadRayJpegDecoder* decoder = state->Decoder;
    if (setjmp(decoder->Jump) != 0) {
        return 0;
    }
    while (decoder->Info.output_scanline < decoder->Info.output_height) {
        const size_t y = decoder->Info.output_scanline;
        const size_t row_index = flip_y ? state->Height - 1u - y : y;
        JSAMPROW row = (JSAMPROW)((unsigned char*)pixels + row_index * state->RowBytes);
        if (jpeg_read_scanlines(&decoder->Info, &row, 1) != 1) {
            return fail(state, "JPEG scanline is incomplete");
        }
        if (state->Channels == 4) {
            size_t x;
            /* Expand backwards to preserve RGB bytes that have not been read. */
            for (x = state->Width; x != 0; --x) {
                const size_t source = (x - 1u) * 3u;
                const size_t destination = (x - 1u) * 4u;
                const JSAMPLE red = row[source];
                const JSAMPLE green = row[source + 1];
                const JSAMPLE blue = row[source + 2];
                row[destination] = red;
                row[destination + 1] = green;
                row[destination + 2] = blue;
                row[destination + 3] = decoder->Alpha;
            }
        }
    }
    if (!jpeg_finish_decompress(&decoder->Info)) {
        return fail(state, "JPEG image is incomplete");
    }
    return 1;
}

void radray_jpeg_destroy(RadRayJpegState* state) {
    if (state->Decoder != NULL) {
        jpeg_destroy_decompress(&state->Decoder->Info);
#ifdef RADRAY_ENABLE_MIMALLOC
        mi_free(state->Decoder);
#else
        free(state->Decoder);
#endif
        state->Decoder = NULL;
    }
}

#endif
