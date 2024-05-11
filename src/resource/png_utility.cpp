#include <radray/resource/png_utility.h>

#include <fstream>
#include <stdexcept>
#include <limits>

#include <png.h>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/image_data.h>

namespace radray::resource {

class ReadPngException : public std::runtime_error {
public:
    explicit ReadPngException(const char* msg) : std::runtime_error(msg) {}
    ~ReadPngException() override {}
};

static bool IsPng(std::istream& is) {
    static constexpr std::size_t HeadSize = 8;
    png_byte pngsig[HeadSize]{};
    is.read(reinterpret_cast<char*>(pngsig), HeadSize);
    if (!is.good()) {
        return false;
    }
    return png_sig_cmp(pngsig, 0, HeadSize) == 0;
}

static bool ReadStreamImpl(PngData* that, std::istream& stream) {
    png_structp pPng = png_create_read_struct(
        PNG_LIBPNG_VER_STRING,
        nullptr,
        [](png_structp p, png_const_charp reason) {
            throw ReadPngException{reason};
        },
        [](png_structp p, png_const_charp tips) {
            RADRAY_WARN_LOG("warning with read png: {}", tips);
        });
    png_infop pInfo = png_create_info_struct(pPng);
    auto guard1 = MakeScopeGuard([&]() {
        png_destroy_read_struct(&pPng, &pInfo, nullptr);
    });
    if (!pPng || !pInfo) {
        RADRAY_ERR_LOG("cannot init libpng");
        return false;
    }
    try {
        png_set_read_fn(pPng, &stream, [](png_structp png_ptr, png_bytep png_data, png_size_t data_size) {
            std::ifstream* pIfs = static_cast<std::ifstream*>(png_get_io_ptr(png_ptr));
            pIfs->read(reinterpret_cast<char*>(png_data), data_size);
        });
        png_set_sig_bytes(pPng, 8);
        png_read_info(pPng, pInfo);
        png_uint_32 img_width = png_get_image_width(pPng, pInfo);
        png_uint_32 img_height = png_get_image_height(pPng, pInfo);
        png_uint_32 bit_depth = png_get_bit_depth(pPng, pInfo);
        png_uint_32 img_channels = png_get_channels(pPng, pInfo);
        png_uint_32 color_type = png_get_color_type(pPng, pInfo);
        uint32_t elemSize = bit_depth / 8;
        if (color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_palette_to_rgb(pPng);
        }
        if (png_get_valid(pPng, pInfo, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(pPng);
        }
        png_set_expand_gray_1_2_4_to_8(pPng);
        png_read_update_info(pPng, pInfo);
        bit_depth = png_get_bit_depth(pPng, pInfo);
        img_channels = png_get_channels(pPng, pInfo);
        color_type = png_get_color_type(pPng, pInfo);
        auto row_ptrs = std::make_unique<png_bytep[]>(img_height);
        size_t byteSize = (size_t)img_width * img_height * img_channels * elemSize;
        auto png_data = std::make_unique<byte[]>(byteSize);
        size_t stride = img_width * img_channels * elemSize;
        for (size_t i = 0; i < img_height; i++) {
            size_t offset = i * stride;
            row_ptrs[i] = reinterpret_cast<png_bytep>(png_data.get() + offset);
        }
        png_read_image(pPng, row_ptrs.get());
        that->width = img_width;
        that->height = img_height;
        that->bitDepth = bit_depth;
        that->channel = img_channels;
        that->colorType = ([color_type]() {
            switch (color_type) {
                case PNG_COLOR_TYPE_GRAY: return PngColorType::GRAY;
                case PNG_COLOR_TYPE_RGB: return PngColorType::RGB;
                case PNG_COLOR_TYPE_RGB_ALPHA: return PngColorType::RGB_ALPHA;
                case PNG_COLOR_TYPE_GRAY_ALPHA: return PngColorType::GRAY_ALPHA;
                default: return PngColorType::UNKNOWN;
            }
        })();
        that->data = std::move(png_data);
        switch (that->colorType) {
            case PngColorType::GRAY: RADRAY_ASSERT(that->channel == 1, "what?"); break;
            case PngColorType::RGB: RADRAY_ASSERT(that->channel == 3, "what?"); break;
            case PngColorType::RGB_ALPHA: RADRAY_ASSERT(that->channel == 4, "what?"); break;
            case PngColorType::GRAY_ALPHA: RADRAY_ASSERT(that->channel == 2, "what?"); break;
            default: break;
        }
        return true;
    } catch (ReadPngException& e) {
        RADRAY_ERR_LOG("load png error {}", e.what());
        return false;
    }
}

bool PngData::LoadFromFile(const std::filesystem::path& path, uint64_t fileOffset) {
    std::ifstream ifs{path, std::ios_base::in | std::ios_base::binary};
    {
        auto filename = path.generic_string();
        RADRAY_DEBUG_LOG("read png file {}", filename);
    }
    if (!ifs.is_open()) {
        auto filename = path.generic_string();
        RADRAY_ERR_LOG("cannot open file {}", filename);
        return false;
    }
    ifs.seekg(fileOffset);
    if (!IsPng(ifs)) {
        auto filename = path.generic_string();
        RADRAY_ERR_LOG("file is not png {}", filename);
        return false;
    }
    return ReadStreamImpl(this, ifs);
}

bool PngData::LoadFromStream(std::istream& stream) {
    if (!IsPng(stream)) {
        RADRAY_ERR_LOG("istream is not png");
        return false;
    }
    return ReadStreamImpl(this, stream);
}

void PngData::MoveToImageData(ImageData* o) {
    o->width = width;
    o->height = height;
    o->format = ([&]() {
        switch (colorType) {
            case PngColorType::GRAY: {
                switch (bitDepth) {
                    case 8: return ImageFormat::R8_BYTE;
                    case 16: return ImageFormat::R16_USHORT;
                    default: RADRAY_ABORT("unreachable"); return (ImageFormat)-1;
                }
            }
            case PngColorType::RGB: {
                switch (bitDepth) {
                    case 8: return ImageFormat::RGBA8_BYTE;
                    case 16: return ImageFormat::RGBA16_USHORT;
                    default: RADRAY_ABORT("unreachable"); return (ImageFormat)-1;
                }
            }
            case PngColorType::RGB_ALPHA: {
                switch (bitDepth) {
                    case 8: return ImageFormat::RGBA8_BYTE;
                    case 16: return ImageFormat::RGBA16_USHORT;
                    default: RADRAY_ABORT("unreachable"); return (ImageFormat)-1;
                }
            }
            case PngColorType::GRAY_ALPHA: {
                switch (bitDepth) {
                    case 8: return ImageFormat::RG8_BYTE;
                    case 16: return ImageFormat::RG16_USHORT;
                    default: RADRAY_ABORT("unreachable"); return (ImageFormat)-1;
                }
            }
            default: RADRAY_ABORT("unreachable"); return (ImageFormat)-1;
        }
    })();
    if (colorType == PngColorType::RGB) {
        RADRAY_ASSERT(channel == 3, "what?");
        size_t elemSize = (size_t)bitDepth / 8;
        size_t srcElem = elemSize * 3;
        size_t srcStride = (size_t)width * srcElem;
        size_t dstElem = elemSize * 4;
        size_t dstStride = (size_t)width * dstElem;
        o->data = std::make_unique<byte[]>(dstStride * height);
        for (size_t j = 0; j < height; j++) {
            const byte* srcStart = data.get() + srcStride * j;
            byte* dstStart = o->data.get() + dstStride * j;
            for (size_t i = 0; i < width; i++) {
                const byte* src = srcStart + i * srcElem;
                byte* dst = dstStart + i * dstElem;
                for (size_t k = 0; k < srcElem; k++) {
                    dst[k] = src[k];
                }
                for (size_t k = 0; k < elemSize; k++) {
                    dst[k] = std::numeric_limits<byte>::max();
                }
            }
        }
    } else {
        o->data = std::move(data);
    }
}

}  // namespace radray::resource

auto std::formatter<radray::resource::PngColorType>::format(radray::resource::PngColorType const& val, format_context& ctx) const -> decltype(ctx.out()) {
    auto str = ([&]() {
        switch (val) {
            case radray::resource::PngColorType::UNKNOWN: return "UNKNOWN";
            case radray::resource::PngColorType::GRAY: return "GRAY";
            case radray::resource::PngColorType::RGB: return "RGB";
            case radray::resource::PngColorType::RGB_ALPHA: return "RGB_ALPHA";
            case radray::resource::PngColorType::GRAY_ALPHA: return "GRAY_ALPHA";
        }
    })();
    return std::formatter<const char*>::format(str, ctx);
}
