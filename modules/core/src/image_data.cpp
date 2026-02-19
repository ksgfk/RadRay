#include <radray/image_data.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <bit>
#include <stdexcept>

#ifdef RADRAY_ENABLE_PNG
#include <exception>
#include <png.h>
#endif

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/platform.h>
#include <radray/scope_guard.h>

namespace radray {

ImageData::ImageData(const ImageData& other) noexcept : Width(other.Width), Height(other.Height), Format(other.Format) {
    const size_t size = other.GetSize();
    if (size > 0 && other.Data) {
        Data = make_unique<byte[]>(size);
        std::memcpy(Data.get(), other.Data.get(), size);
    }
}

ImageData::ImageData(ImageData&& other) noexcept
    : Data(std::move(other.Data)), Width(other.Width), Height(other.Height), Format(other.Format) {
    other.Width = 0;
    other.Height = 0;
    other.Format = ImageFormat::R8_BYTE;
}

ImageData& ImageData::operator=(const ImageData& other) noexcept {
    if (this != &other) {
        ImageData tmp{other};
        swap(*this, tmp);
    }
    return *this;
}

ImageData& ImageData::operator=(ImageData&& other) noexcept {
    if (this != &other) {
        ImageData tmp{std::move(other)};
        swap(*this, tmp);
    }
    return *this;
}

size_t ImageData::GetSize() const noexcept {
    if (Width == 0 || Height == 0) {
        return 0;
    }
    const size_t bs = this->FormatSize(Format);
    const size_t width = static_cast<size_t>(Width);
    const size_t height = static_cast<size_t>(Height);
    const size_t pixelCount = width * height;
    return pixelCount * bs;
}

std::span<const byte> ImageData::GetSpan() const noexcept {
    const size_t size = GetSize();
    if (!Data || size == 0) {
        return {};
    }
    return std::span<const byte>{Data.get(), size};
}

ImageData ImageData::RGB8ToRGBA8(uint8_t alpha_) const noexcept {
    if (Width == 0 || Height == 0) {
        return ImageData{};
    }
    RADRAY_ASSERT(Format == ImageFormat::RGB8_BYTE);
    RADRAY_ASSERT(Data);

    ImageData dstImg;
    dstImg.Width = Width;
    dstImg.Height = Height;
    dstImg.Format = ImageFormat::RGBA8_BYTE;
    dstImg.Data = make_unique<byte[]>(dstImg.GetSize());

    const size_t row = static_cast<size_t>(Width);
    const size_t srcStride = row * 3;
    const size_t dstStride = row * 4;
    byte a_ = static_cast<byte>(alpha_);
    const byte* src_ = Data.get();
    byte* dst_ = dstImg.Data.get();
    for (size_t j = 0; j < Height; j++, src_ += srcStride, dst_ += dstStride) {
        const byte* src = src_;
        byte* dst = dst_;
        for (size_t i = 0; i < row; i++, src += 3, dst += 4) {
            byte r = src[0], g = src[1], b = src[2], a = a_;
            dst[0] = r, dst[1] = g, dst[2] = b, dst[3] = a;
        }
    }
    return dstImg;
}

void ImageData::FlipY() noexcept {
    if (!Data || Width == 0 || Height < 2) {
        return;
    }
    const size_t rowBytes = this->FormatSize(Format) * static_cast<size_t>(Width);
    if (rowBytes == 0) {
        return;
    }
    byte* dataPtr = Data.get();
    for (size_t y = 0; y < Height / 2; ++y) {
        byte* rowTop = dataPtr + y * rowBytes;
        byte* rowBottom = dataPtr + (Height - 1 - y) * rowBytes;
        std::swap_ranges(rowTop, rowTop + rowBytes, rowBottom);
    }
}

void swap(ImageData& a, ImageData& b) noexcept {
    using std::swap;
    swap(a.Data, b.Data);
    swap(a.Width, b.Width);
    swap(a.Height, b.Height);
    swap(a.Format, b.Format);
}

size_t ImageData::FormatSize(ImageFormat format) noexcept {
    switch (format) {
        case ImageFormat::R8_BYTE: return 1;
        case ImageFormat::R16_USHORT: return 2;
        case ImageFormat::R16_HALF: return 2;
        case ImageFormat::R32_FLOAT: return 4;
        case ImageFormat::RG8_BYTE: return 2;
        case ImageFormat::RG16_USHORT: return 4;
        case ImageFormat::RG16_HALF: return 4;
        case ImageFormat::RG32_FLOAT: return 8;
        case ImageFormat::RGB32_FLOAT: return 12;
        case ImageFormat::RGBA8_BYTE: return 4;
        case ImageFormat::RGBA16_USHORT: return 8;
        case ImageFormat::RGBA16_HALF: return 8;
        case ImageFormat::RGBA32_FLOAT: return 16;
        case ImageFormat::RGB8_BYTE: return 3;
        case ImageFormat::RGB16_USHORT: return 6;
    }
    Unreachable();
}

#ifdef RADRAY_ENABLE_PNG

class LibpngException : public std::runtime_error {
public:
    LibpngException() : std::runtime_error("LibpngException") {}
    explicit LibpngException(const char* msg) : std::runtime_error(msg) {}
    ~LibpngException() noexcept override = default;
};
static void radray_libpng_user_error_fn(png_structp png_ptr, png_const_charp msg) {
    RADRAY_UNUSED(png_ptr);
    RADRAY_ERR_LOG("libpng error msg: {}", msg);
    throw LibpngException(msg);
}
static void radray_libpng_user_warn_fn(png_structp png_ptr, png_const_charp msg) {
    RADRAY_UNUSED(png_ptr);
    RADRAY_WARN_LOG("libpng warning msg: {}", msg);
}
static void* radray_libpng_malloc_fn(png_structp png_ptr, png_size_t size) {
    RADRAY_UNUSED(png_ptr);
    return radray::Malloc(size);
}
static void radray_libpng_free_fn(png_structp png_ptr, void* ptr) {
    RADRAY_UNUSED(png_ptr);
    radray::Free(ptr);
}
static void radray_libpng_read_fn(png_structp png_ptr, png_bytep data, png_size_t length) {
    auto* stream = static_cast<std::istream*>(png_get_io_ptr(png_ptr));
    stream->read(reinterpret_cast<char*>(data), length);
    if ((png_size_t)stream->gcount() != length) {
        png_error(png_ptr, "gcount not equal length");
    }
}

static size_t _SafeMulSize(size_t a, size_t b) {
    if (b != 0 && a > std::numeric_limits<size_t>::max() / b) {
        throw LibpngException("image buffer too large");
    }
    return a * b;
}

static constexpr size_t PNG_SIG_SIZE = 8;
bool ImageData::IsPNG(std::istream& stream) {
    if (!stream.good()) {
        return false;
    }
    const auto original_state = stream.rdstate();
    const std::istream::pos_type original_pos = stream.tellg();
    const bool can_seek = original_pos != std::istream::pos_type(-1);
    stream.clear();
    std::array<png_byte, PNG_SIG_SIZE> pngsig{};
    stream.read(reinterpret_cast<char*>(pngsig.data()), static_cast<std::streamsize>(pngsig.size()));
    const std::streamsize read_bytes = stream.gcount();
    const bool is_png = (read_bytes == static_cast<std::streamsize>(pngsig.size())) &&
                        (png_sig_cmp(pngsig.data(), 0, PNG_SIG_SIZE) == 0);
    stream.clear();
    if (can_seek) {
        stream.seekg(original_pos);
    } else {
        stream.seekg(0, std::ios::beg);
    }
    stream.clear(original_state);
    return is_png;
}
std::optional<ImageData> ImageData::LoadPNG(std::istream& stream, PNGLoadSettings settings) {
    if (!stream.good()) {
        RADRAY_ERR_LOG("stream is not good");
        return std::nullopt;
    }
    png_structp png_ptr = nullptr;
    png_infop info_ptr = nullptr;
    auto guard_png_ptr = MakeScopeGuard([&]() {
        if (png_ptr) {
            png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : nullptr, nullptr);
            png_ptr = nullptr;
            info_ptr = nullptr;
        }
    });
    try {
        png_ptr = png_create_read_struct_2(
            PNG_LIBPNG_VER_STRING,
            nullptr, radray_libpng_user_error_fn, radray_libpng_user_warn_fn,
            nullptr, radray_libpng_malloc_fn, radray_libpng_free_fn);
        if (!png_ptr) {
            throw LibpngException("png_create_read_struct_2 failed");
        }
        info_ptr = png_create_info_struct(png_ptr);
        if (!info_ptr) {
            throw LibpngException("png_create_info_struct failed");
        }
        png_set_read_fn(png_ptr, &stream, radray_libpng_read_fn);
#if PNG_LIBPNG_VER >= 10600
#ifdef PNG_MAXIMUM_INFLATE_WINDOW
        png_set_option(png_ptr, PNG_MAXIMUM_INFLATE_WINDOW, PNG_OPTION_ON);
#endif
        png_set_check_for_invalid_index(png_ptr, 1);
#endif
        png_set_crc_action(png_ptr, PNG_CRC_DEFAULT, PNG_CRC_WARN_USE);
        png_read_info(png_ptr, info_ptr);
        png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
        png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
        if (width == 0 || height == 0) {
            throw LibpngException("png image has zero dimension");
        }
        if (width > std::numeric_limits<uint32_t>::max() || height > std::numeric_limits<uint32_t>::max()) {
            throw LibpngException("png image exceeds supported dimensions");
        }
        png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);
        png_byte color_type = png_get_color_type(png_ptr, info_ptr);
        if (color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_palette_to_rgb(png_ptr);
        }
        if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png_ptr);
        }
        if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
            png_set_expand_gray_1_2_4_to_8(png_ptr);
            bit_depth = 8;
        }
        if (bit_depth == 16 && std::endian::native == std::endian::little) {
            png_set_swap(png_ptr);
        }
        if (settings.AddAlphaIfRGB.has_value() &&
            (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_PALETTE)) {
            const uint32_t alpha_limit = (bit_depth == 16) ? 0xFFFFu : 0xFFu;
            const png_uint_32 alpha_value = static_cast<png_uint_32>(
                std::min(*settings.AddAlphaIfRGB, alpha_limit));
            png_set_add_alpha(png_ptr, alpha_value, PNG_FILLER_AFTER);
        }
        const int passes = png_set_interlace_handling(png_ptr);
        RADRAY_UNUSED(passes);
        png_read_update_info(png_ptr, info_ptr);
        width = png_get_image_width(png_ptr, info_ptr);
        height = png_get_image_height(png_ptr, info_ptr);
        bit_depth = png_get_bit_depth(png_ptr, info_ptr);
        color_type = png_get_color_type(png_ptr, info_ptr);
        const size_t height_sz = static_cast<size_t>(height);
        const size_t rowbytes = static_cast<size_t>(png_get_rowbytes(png_ptr, info_ptr));
        if (rowbytes == 0 || height_sz == 0) {
            throw LibpngException("png rowbytes invalid");
        }
        const size_t image_byte_size = _SafeMulSize(rowbytes, height_sz);
        auto image_data = make_unique<radray::byte[]>(image_byte_size);
        static_assert(sizeof(radray::byte) == sizeof(png_byte), "what");
        static_assert(std::is_trivial_v<radray::byte> && std::is_trivial_v<png_byte>, "what");
        static_assert(std::is_standard_layout_v<radray::byte> && std::is_standard_layout_v<png_byte>, "what");
        vector<png_bytep> row_pointers(height_sz);
        png_bytep base_ptr = reinterpret_cast<png_bytep>(image_data.get());
        if (settings.IsFlipY) {
            png_bytep row = base_ptr + (height_sz - 1) * rowbytes;
            for (size_t i = 0; i < height_sz; ++i, row -= rowbytes) {
                row_pointers[i] = row;
            }
        } else {
            png_bytep row = base_ptr;
            for (size_t i = 0; i < height_sz; ++i, row += rowbytes) {
                row_pointers[i] = row;
            }
        }
        png_read_image(png_ptr, row_pointers.data());
        png_read_end(png_ptr, nullptr);
        ImageData imgData;
        imgData.Data = std::move(image_data);
        imgData.Width = static_cast<uint32_t>(width);
        imgData.Height = static_cast<uint32_t>(height);
        switch (color_type) {
            case PNG_COLOR_TYPE_GRAY: {
                switch (bit_depth) {
                    case 8: imgData.Format = ImageFormat::R8_BYTE; break;
                    case 16: imgData.Format = ImageFormat::R16_USHORT; break;
                    default: throw LibpngException("unsupported PNG bit depth");
                }
                break;
            }
            case PNG_COLOR_TYPE_GRAY_ALPHA: {
                switch (bit_depth) {
                    case 8: imgData.Format = ImageFormat::RG8_BYTE; break;
                    case 16: imgData.Format = ImageFormat::RG16_USHORT; break;
                    default: throw LibpngException("unsupported PNG bit depth");
                }
                break;
            }
            case PNG_COLOR_TYPE_RGB: {
                switch (bit_depth) {
                    case 8: imgData.Format = ImageFormat::RGB8_BYTE; break;
                    case 16: imgData.Format = ImageFormat::RGB16_USHORT; break;
                    default: throw LibpngException("unsupported PNG bit depth");
                }
                break;
            }
            case PNG_COLOR_TYPE_RGB_ALPHA: {
                switch (bit_depth) {
                    case 8: imgData.Format = ImageFormat::RGBA8_BYTE; break;
                    case 16: imgData.Format = ImageFormat::RGBA16_USHORT; break;
                    default: throw LibpngException("unsupported PNG bit depth");
                }
                break;
            }
            default: throw LibpngException("unsupported PNG color type");
        }
        RADRAY_ASSERT(image_byte_size == imgData.GetSize());
        return std::make_optional(std::move(imgData));
    } catch (LibpngException& e) {
        RADRAY_ERR_LOG("LibpngException: {}", e.what());
        return std::nullopt;
    } catch (...) {
        RADRAY_ERR_LOG("LibpngException: {}", "unknown error");
        return std::nullopt;
    }
}
#else
bool ImageData::IsPNG(std::istream& stream) {
    RADRAY_UNUSED(stream);
    RADRAY_ERR_LOG("libpng support is not enabled");
    return false;
}
std::optional<ImageData> ImageData::LoadPNG(std::istream& stream, PNGLoadSettings settings) {
    RADRAY_UNUSED(stream);
    RADRAY_UNUSED(settings);
    RADRAY_ERR_LOG("libpng support is not enabled");
    return std::nullopt;
}
#endif

std::string_view format_as(ImageFormat val) noexcept {
    switch (val) {
        case radray::ImageFormat::R8_BYTE: return "R8_BYTE";
        case radray::ImageFormat::R16_USHORT: return "R16_USHORT";
        case radray::ImageFormat::R16_HALF: return "R16_HALF";
        case radray::ImageFormat::R32_FLOAT: return "R32_FLOAT";
        case radray::ImageFormat::RG8_BYTE: return "RG8_BYTE";
        case radray::ImageFormat::RG16_USHORT: return "RG16_USHORT";
        case radray::ImageFormat::RG16_HALF: return "RG16_HALF";
        case radray::ImageFormat::RG32_FLOAT: return "RG32_FLOAT";
        case radray::ImageFormat::RGB32_FLOAT: return "RGB32_FLOAT";
        case radray::ImageFormat::RGBA8_BYTE: return "RGBA8_BYTE";
        case radray::ImageFormat::RGBA16_USHORT: return "RGBA16_USHORT";
        case radray::ImageFormat::RGBA16_HALF: return "RGBA16_HALF";
        case radray::ImageFormat::RGBA32_FLOAT: return "RGBA32_FLOAT";
        case radray::ImageFormat::RGB8_BYTE: return "RGB8_BYTE";
        case radray::ImageFormat::RGB16_USHORT: return "RGB16_USHORT";
    }
    Unreachable();
}

}  // namespace radray
