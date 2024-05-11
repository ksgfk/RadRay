#include <radray/resource/jpeg_utility.h>

#include <fstream>
#include <stdexcept>
#include <limits>

#include <jpeglib.h>
#include <jerror.h>

#include <radray/utility.h>
#include <radray/image_data.h>

namespace radray::resource {

class ReadJpgException : public std::runtime_error {
public:
    explicit ReadJpgException() : std::runtime_error("") {}
    ~ReadJpgException() override {}
};

constexpr size_t STREAM_BUFFER_SIZE = 4096;

struct RadrayJpegSourceMgr {
    jpeg_source_mgr pub;
    std::istream* stream;
    uint64_t offset;
    bool startOfFile;
    byte buf[STREAM_BUFFER_SIZE];
};

static_assert(std::is_trivial_v<RadrayJpegSourceMgr>, "RadrayJpegSourceMgr must be trivial");
static_assert(std::is_standard_layout_v<RadrayJpegSourceMgr>, "RadrayJpegSourceMgr must be standard layout");

static void InitSource(j_decompress_ptr cinfo) {
    RadrayJpegSourceMgr* src = reinterpret_cast<RadrayJpegSourceMgr*>(cinfo->src);
    std::istream& stream = *src->stream;
    stream.seekg(src->offset);
    if (stream.bad()) {
        RADRAY_ERR_LOG("bad stream");
        ERREXIT(cinfo, JERR_FILE_READ);
    }
    src->startOfFile = true;
}

static boolean FillBuffer(j_decompress_ptr cinfo) {
    RadrayJpegSourceMgr* src = reinterpret_cast<RadrayJpegSourceMgr*>(cinfo->src);
    std::istream& stream = *src->stream;
    stream.read((char*)src->buf, STREAM_BUFFER_SIZE);
    size_t gcnt = stream.gcount();
    if (gcnt <= 0) {
        if (src->startOfFile) {
            ERREXIT(cinfo, JERR_INPUT_EMPTY);
        }
        WARNMS(cinfo, JWRN_JPEG_EOF);
        src->buf[0] = (byte)(JOCTET)0xFF;
        src->buf[1] = (byte)(JOCTET)JPEG_EOI;
        gcnt = 2;
    }
    if (stream.bad()) {
        RADRAY_ERR_LOG("bad stream");
        ERREXIT(cinfo, JERR_FILE_READ);
    }
    src->pub.next_input_byte = (const JOCTET*)src->buf;
    src->pub.bytes_in_buffer = gcnt;
    return true;
}

static void Skip(j_decompress_ptr cinfo, long count) {
    RadrayJpegSourceMgr* src = reinterpret_cast<RadrayJpegSourceMgr*>(cinfo->src);
    std::istream& stream = *src->stream;
    stream.seekg(count, std::ios_base::cur);
    (*src->pub.fill_input_buffer)(cinfo);
}

static void Term(j_decompress_ptr cinfo) {}

static void JpegReadAsStream(j_decompress_ptr cinfo, std::istream* stream, uint64_t offset) {
    RadrayJpegSourceMgr* src;
    if (cinfo->src == nullptr) {
        cinfo->src = (struct jpeg_source_mgr*)(*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT, sizeof(RadrayJpegSourceMgr));
    }
    src = reinterpret_cast<RadrayJpegSourceMgr*>(cinfo->src);
    src->pub.init_source = InitSource;
    src->pub.fill_input_buffer = FillBuffer;
    src->pub.skip_input_data = Skip;
    src->pub.resync_to_restart = jpeg_resync_to_restart;
    src->pub.term_source = Term;
    src->pub.bytes_in_buffer = 0;
    src->pub.next_input_byte = nullptr;
    src->stream = stream;
    src->offset = offset;
}

static bool ReadStreamImpl(JpgData* that, std::istream& stream) {
    jpeg_decompress_struct cinfo{};
    jpeg_create_decompress(&cinfo);
    auto guard1 = MakeScopeGuard([&]() { jpeg_destroy_decompress(&cinfo); });
    try {
        jpeg_error_mgr errMgr{};
        jpeg_std_error(&errMgr);
        errMgr.error_exit = [](j_common_ptr cinfo) {
            throw ReadJpgException{};
        };
        errMgr.output_message = [](j_common_ptr cinfo) {
            char buffer[JMSG_LENGTH_MAX];
            (*cinfo->err->format_message)(cinfo, buffer);
            RADRAY_WARN_LOG("libjpeg log: {}", buffer);
        };
        cinfo.err = &errMgr;
        JpegReadAsStream(&cinfo, &stream, 0);
        jpeg_read_header(&cinfo, TRUE);
        switch (cinfo.out_color_space) {
            case JCS_GRAYSCALE:
            case JCS_RGB:
                break;
            case JCS_YCbCr:
            case JCS_CMYK:
            case JCS_YCCK:
            case JCS_BG_RGB:
            case JCS_BG_YCC:
            // case JCS_EXT_RGB:
            // case JCS_EXT_RGBX:
            // case JCS_EXT_BGR:
            // case JCS_EXT_BGRX:
            // case JCS_EXT_XBGR:
            // case JCS_EXT_XRGB:
            // case JCS_EXT_RGBA:
            // case JCS_EXT_BGRA:
            // case JCS_EXT_ABGR:
            // case JCS_EXT_ARGB:
            // case JCS_RGB565:
            default:
                cinfo.out_color_space = JCS_RGB;
                break;
        }
        jpeg_start_decompress(&cinfo);
        {
            auto guard2 = MakeScopeGuard([&]() { jpeg_finish_decompress(&cinfo); });
            auto row_stride = cinfo.output_width * cinfo.output_components;
            auto buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);
            size_t allSize = (size_t)row_stride * cinfo.output_height;
            auto result = std::make_unique<byte[]>(allSize);
            size_t oft = 0;
            while (cinfo.output_scanline < cinfo.output_height) {
                jpeg_read_scanlines(&cinfo, buffer, 1);
                std::memcpy(result.get() + oft, buffer[0], row_stride);
                oft += row_stride;
            }
            that->data = std::move(result);
            that->width = cinfo.output_width;
            that->height = cinfo.output_height;
            that->component = cinfo.output_components;
            switch (cinfo.out_color_space) {
                case JCS_GRAYSCALE: that->colorType = JpgColorType::GRAYSCALE; break;
                case JCS_RGB: that->colorType = JpgColorType::RGB; break;
                default: that->colorType = JpgColorType::UNKNOWN; break;
            }
        }
        return true;
    } catch (ReadJpgException& e) {
        RADRAY_ERR_LOG("read jpg fail");
        return false;
    }
}

bool JpgData::LoadFromFile(const std::filesystem::path& path, uint64_t fileOffset) {
    std::ifstream ifs{path, std::ios_base::in | std::ios_base::binary};
    {
        auto filename = path.generic_string();
        RADRAY_DEBUG_LOG("read jpg file {}", filename);
    }
    if (!ifs.is_open() || ifs.bad()) {
        auto filename = path.generic_string();
        RADRAY_ERR_LOG("cannot open file {}", filename);
        return false;
    }
    return ReadStreamImpl(this, ifs);
}

bool JpgData::LoadFromStream(std::istream& stream) {
    return ReadStreamImpl(this, stream);
}

void JpgData::MoveToImageData(ImageData* o) {
    o->width = width;
    o->height = height;
    switch (component) {
        case 1: o->format = ImageFormat::R8_BYTE; break;
        case 3: o->format = ImageFormat::RGBA8_BYTE; break;
        default: RADRAY_ABORT("unreachable"); break;
    }
    if (component == 3) {
        size_t srcStride = (size_t)width * 3;
        size_t dstStride = (size_t)width * 4;
        o->data = std::make_unique<byte[]>(dstStride * height);
        for (size_t j = 0; j < height; j++) {
            const byte* srcStart = data.get() + srcStride * j;
            byte* dstStart = o->data.get() + dstStride * j;
            for (size_t i = 0; i < width; i++) {
                const byte* src = srcStart + i * 3;
                byte* dst = dstStart + i * 4;
                for (size_t k = 0; k < 3; k++) {
                    dst[k] = src[k];
                }
                dst[3] = std::numeric_limits<byte>::max();
            }
        }
    } else {
        o->data = std::move(data);
    }
}

}  // namespace radray::resource

auto std::formatter<radray::resource::JpgColorType>::format(radray::resource::JpgColorType const& val, format_context& ctx) const -> decltype(ctx.out()) {
    auto str = ([&]() {
        switch (val) {
            case radray::resource::JpgColorType::UNKNOWN: return "UNKNOWN";
            case radray::resource::JpgColorType::GRAYSCALE: return "GRAYSCALE";
            case radray::resource::JpgColorType::RGB: return "RGB";
        }
    })();
    return std::formatter<const char*>::format(str, ctx);
}
