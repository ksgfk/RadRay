#ifdef __cplusplus
#error png_codec.c must be compiled as C
#endif

#include "png_codec.h"

#ifdef RADRAY_ENABLE_PNG

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

static void store_message(char* destination, size_t capacity, const char* message) {
    const char* source = message != NULL ? message : "libpng error";
    size_t index = 0;
    while (index + 1 < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static void PNGCBAPI on_error(png_structp png, png_const_charp message) {
    RadRayPngState* state = png_get_error_ptr(png);
    store_message(state->Error, sizeof(state->Error), message);
    png_longjmp(png, 1);
}

static void PNGCBAPI on_warning(png_structp png, png_const_charp message) {
    RadRayPngState* state = png_get_error_ptr(png);
    if (state->Warning[0] == '\0') {
        store_message(state->Warning, sizeof(state->Warning), message);
    }
}

static png_voidp PNGCBAPI allocate(png_structp png, png_alloc_size_t size) {
    (void)png;
#ifdef RADRAY_ENABLE_MIMALLOC
    return mi_malloc(size);
#else
    return malloc(size);
#endif
}

static void PNGCBAPI deallocate(png_structp png, png_voidp allocation) {
    (void)png;
#ifdef RADRAY_ENABLE_MIMALLOC
    mi_free(allocation);
#else
    free(allocation);
#endif
}

static void PNGCBAPI read_bytes(png_structp png, png_bytep destination, png_size_t length) {
    RadRayPngState* state = png_get_io_ptr(png);
    if (length > state->InputRemaining) {
        png_error(png, "PNG input is truncated");
    }
    if (length != 0) {
        memcpy(destination, state->Input, length);
        state->Input += length;
        state->InputRemaining -= length;
    }
}

static void PNGCBAPI write_bytes(png_structp png, png_bytep source, png_size_t length) {
    FILE* destination = png_get_io_ptr(png);
    if (fwrite(source, 1, length, destination) != length) {
        png_error(png, "PNG output write failed");
    }
}

static void PNGCBAPI flush_bytes(png_structp png) {
    FILE* destination = png_get_io_ptr(png);
    if (fflush(destination) != 0) {
        png_error(png, "PNG output flush failed");
    }
}

static int set_layout(RadRayPngState* state, uint32_t width, uint32_t height, int channels, int bit_depth) {
    size_t pixel_bytes;
    size_t row_bytes;
    if (width == 0 || height == 0 || channels < 1 || channels > 4 ||
        (bit_depth != 8 && bit_depth != 16)) {
        store_message(state->Error, sizeof(state->Error), "unsupported PNG dimensions or pixel format");
        return 0;
    }
    pixel_bytes = (size_t)channels * ((size_t)bit_depth / 8);
    if (width > SIZE_MAX / pixel_bytes) {
        store_message(state->Error, sizeof(state->Error), "PNG row size exceeds supported storage");
        return 0;
    }
    row_bytes = (size_t)width * pixel_bytes;
    if (height > SIZE_MAX / row_bytes) {
        store_message(state->Error, sizeof(state->Error), "PNG image size exceeds supported storage");
        return 0;
    }
    state->Width = width;
    state->Height = height;
    state->Channels = channels;
    state->BitDepth = bit_depth;
    state->RowBytes = row_bytes;
    state->ByteSize = row_bytes * height;
    return 1;
}

static int little_endian(void) {
    const uint16_t value = 1;
    return *(const unsigned char*)&value != 0;
}

int radray_png_read_header(RadRayPngState* state, int64_t add_alpha) {
    int bit_depth;
    int color_type;
    if (state->Input == NULL || state->InputRemaining == 0) {
        store_message(state->Error, sizeof(state->Error), "PNG input is empty");
        return 0;
    }
    state->Png = png_create_read_struct_2(
        PNG_LIBPNG_VER_STRING, state, on_error, on_warning, NULL, allocate, deallocate);
    if (state->Png == NULL) {
        store_message(state->Error, sizeof(state->Error), "png_create_read_struct_2 failed");
        return 0;
    }
    if (setjmp(png_jmpbuf(state->Png)) != 0) {
        return 0;
    }

    state->Info = png_create_info_struct(state->Png);
    if (state->Info == NULL) {
        store_message(state->Error, sizeof(state->Error), "png_create_info_struct failed");
        return 0;
    }
    png_set_read_fn(state->Png, state, read_bytes);
#ifdef PNG_MAXIMUM_INFLATE_WINDOW
    png_set_option(state->Png, PNG_MAXIMUM_INFLATE_WINDOW, PNG_OPTION_ON);
#endif
    png_set_check_for_invalid_index(state->Png, 1);
    png_set_crc_action(state->Png, PNG_CRC_DEFAULT, PNG_CRC_WARN_USE);
    png_read_info(state->Png, state->Info);

    bit_depth = png_get_bit_depth(state->Png, state->Info);
    color_type = png_get_color_type(state->Png, state->Info);
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(state->Png);
    }
    if (png_get_valid(state->Png, state->Info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(state->Png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(state->Png);
    }
    if (bit_depth == 16 && little_endian()) {
        png_set_swap(state->Png);
    }
    if (add_alpha >= 0 &&
        (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_PALETTE)) {
        const png_uint_32 limit = bit_depth == 16 ? 0xffffu : 0xffu;
        png_set_add_alpha(state->Png, add_alpha > limit ? limit : (png_uint_32)add_alpha, PNG_FILLER_AFTER);
    }
    state->PassCount = png_set_interlace_handling(state->Png);
    png_read_update_info(state->Png, state->Info);

    if (!set_layout(state, png_get_image_width(state->Png, state->Info),
                    png_get_image_height(state->Png, state->Info),
                    png_get_channels(state->Png, state->Info),
                    png_get_bit_depth(state->Png, state->Info))) {
        return 0;
    }
    if (state->RowBytes != png_get_rowbytes(state->Png, state->Info)) {
        store_message(state->Error, sizeof(state->Error), "unsupported PNG row layout");
        return 0;
    }
    return 1;
}

int radray_png_read_pixels(RadRayPngState* state, void* pixels, int flip_y) {
    int pass;
    uint32_t y;
    if (setjmp(png_jmpbuf(state->Png)) != 0) {
        return 0;
    }
    for (pass = 0; pass < state->PassCount; ++pass) {
        for (y = 0; y < state->Height; ++y) {
            const size_t row = flip_y ? state->Height - 1u - y : y;
            png_read_row(state->Png, (unsigned char*)pixels + row * state->RowBytes, NULL);
        }
    }
    png_read_end(state->Png, NULL);
    return 1;
}

int radray_png_write(RadRayPngState* state, FILE* destination, const void* pixels, int flip_y) {
    int color_type;
    uint32_t y;
    if (state->Channels != 3 && state->Channels != 4) {
        store_message(state->Error, sizeof(state->Error), "unsupported PNG output format");
        return 0;
    }
    if (!set_layout(state, state->Width, state->Height, state->Channels, 8)) {
        return 0;
    }
    color_type = state->Channels == 3 ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGB_ALPHA;
    state->Writing = 1;
    state->Png = png_create_write_struct_2(
        PNG_LIBPNG_VER_STRING, state, on_error, on_warning, NULL, allocate, deallocate);
    if (state->Png == NULL) {
        store_message(state->Error, sizeof(state->Error), "png_create_write_struct_2 failed");
        return 0;
    }
    if (setjmp(png_jmpbuf(state->Png)) != 0) {
        return 0;
    }
    state->Info = png_create_info_struct(state->Png);
    if (state->Info == NULL) {
        store_message(state->Error, sizeof(state->Error), "png_create_info_struct failed");
        return 0;
    }
    png_set_write_fn(state->Png, destination, write_bytes, flush_bytes);
    png_set_IHDR(state->Png, state->Info, state->Width, state->Height,
                 8, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(state->Png, state->Info);
    for (y = 0; y < state->Height; ++y) {
        const size_t row = flip_y ? state->Height - 1u - y : y;
        png_write_row(state->Png, (const unsigned char*)pixels + row * state->RowBytes);
    }
    png_write_end(state->Png, state->Info);
    return 1;
}

void radray_png_destroy(RadRayPngState* state) {
    if (state->Png != NULL) {
        if (state->Writing) {
            png_destroy_write_struct(&state->Png, &state->Info);
        } else {
            png_destroy_read_struct(&state->Png, &state->Info, NULL);
        }
    }
}

#endif
