#include <radray/image_data.h>

#ifdef RADRAY_ENABLE_PNG
#include <exception>
#include <png.h>
#endif

#include <radray/utility.h>

namespace radray {

size_t ImageData::GetSize() const noexcept {
    auto bs = GetImageFormatSize(Format);
    return bs * Width * Height;
}

std::span<const byte> ImageData::GetSpan() const noexcept {
    size_t size = GetSize();
    return std::span<const byte>{Data.get(), size};
}

size_t GetImageFormatSize(ImageFormat format) noexcept {
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
    }
    Unreachable();
}

#ifdef RADRAY_ENABLE_PNG
class LibpngException : public std::exception {
public:
    LibpngException() : std::exception() {}
    explicit LibpngException(const char* msg) : std::exception(msg) {}
    ~LibpngException() noexcept override = default;
};
static void radray_libpng_user_error_fn(png_structp png_ptr, png_const_charp msg) {
    RADRAY_UNUSED(png_ptr);
    RADRAY_ERR_LOG("libpng error: {}", msg);
    throw LibpngException(msg);
}
static void radray_libpng_user_warn_fn(png_structp png_ptr, png_const_charp msg) noexcept {
    RADRAY_UNUSED(png_ptr);
    RADRAY_WARN_LOG("libpng warn: {}", msg);
}
static void* radray_libpng_malloc_fn(png_structp png_ptr, png_size_t size) noexcept {
    RADRAY_UNUSED(png_ptr);
    return radray::Malloc(size);
}
static void radray_libpng_free_fn(png_structp png_ptr, void* ptr) noexcept {
    RADRAY_UNUSED(png_ptr);
    radray::Free(ptr);
}
static void radray_libpng_read_fn(png_structp png_ptr, png_bytep data, png_size_t length) noexcept {
    auto* stream = static_cast<std::istream*>(png_get_io_ptr(png_ptr));
    stream->read(reinterpret_cast<char*>(data), length);
    if ((png_size_t)stream->gcount() != length) {
        png_error(png_ptr, "gcount not equal length");
    }
}
static constexpr size_t PNG_SIG_SIZE = 8;
bool IsPNG(std::istream& stream) noexcept {
    png_byte pngsig[PNG_SIG_SIZE];
    int isPng = 0;
    stream.read((char*)pngsig, PNG_SIG_SIZE);
    if (!stream.good()) {
        return false;
    }
    isPng = png_sig_cmp(pngsig, 0, PNG_SIG_SIZE);
    stream.seekg(0, std::ios::beg);
    return (isPng == 0);
}
std::optional<ImageData> LoadPNG(std::istream& stream) {
    png_structp png_ptr = nullptr;
    png_infop info_ptr = nullptr;
    auto guard_png_ptr = MakeScopeGuard([&]() {if (png_ptr) png_destroy_read_struct(&png_ptr, nullptr, nullptr); });
    auto guard_info_ptr = MakeScopeGuard([&]() {if (info_ptr) png_destroy_info_struct(png_ptr, &info_ptr); });
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
        png_read_info(png_ptr, info_ptr);
        // png_get_IHDR();
        // if (color_type == PNG_COLOR_TYPE_PALETTE)
        //     png_set_palette_to_rgb(png_ptr);
        // if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        //     png_set_gray_1_2_4_to_8(png_ptr);
        // if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        //     png_set_tRNS_to_alpha(png_ptr);
        return std::nullopt;
    } catch (LibpngException& e) {
        RADRAY_ERR_LOG("libpng error: {}", e.what());
        return std::nullopt;
    } catch (...) {
        RADRAY_ERR_LOG("libpng error: unknown error");
        return std::nullopt;
    }
}
#endif

std::string_view to_string(ImageFormat val) noexcept {
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
    }
    Unreachable();
}

}  // namespace radray
