#include <radray/image_data.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <cstdio>
#include <utility>

#ifdef RADRAY_ENABLE_PNG
#include "png_codec.h"
#endif

#ifdef RADRAY_ENABLE_JPEG
#include "jpeg_codec.h"
#endif

#include <radray/enum_flags.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/nullable.h>
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

#if defined(RADRAY_ENABLE_PNG) || defined(RADRAY_ENABLE_JPEG)

namespace {

bool ReadImageStream(std::istream& stream, vector<byte>& encoded) {
    const Nullable<std::streambuf*> source{stream.rdbuf()};
    if (!stream.good() || !source) {
        return false;
    }
    array<char, 4096> chunk{};
    for (;;) {
        const std::streamsize count = source->sgetn(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (count == 0) {
            return !encoded.empty();
        }
        if (count < 0 || static_cast<size_t>(count) > encoded.max_size() - encoded.size()) {
            return false;
        }
        const auto* first = reinterpret_cast<const byte*>(chunk.data());
        encoded.insert(encoded.end(), first, first + static_cast<size_t>(count));
    }
}

}  // namespace

#endif

#ifdef RADRAY_ENABLE_PNG

namespace {

bool PngImageFormat(const RadRayPngState& state, ImageFormat& format) noexcept {
    if (state.BitDepth != 8 && state.BitDepth != 16) {
        return false;
    }
    switch (state.Channels) {
        case 1: format = state.BitDepth == 8 ? ImageFormat::R8_BYTE : ImageFormat::R16_USHORT; return true;
        case 2: format = state.BitDepth == 8 ? ImageFormat::RG8_BYTE : ImageFormat::RG16_USHORT; return true;
        case 3: format = state.BitDepth == 8 ? ImageFormat::RGB8_BYTE : ImageFormat::RGB16_USHORT; return true;
        case 4: format = state.BitDepth == 8 ? ImageFormat::RGBA8_BYTE : ImageFormat::RGBA16_USHORT; return true;
        default: return false;
    }
}

void LogPngWarning(const RadRayPngState& state) {
    if (state.Warning[0] != '\0') {
        RADRAY_WARN_LOG("libpng warning: {}", state.Warning);
    }
}

}  // namespace

bool ImageData::IsPNG(std::istream& stream) {
    constexpr size_t PNG_SIG_SIZE = 8;
    if (!stream.good()) {
        return false;
    }
    const auto original_state = stream.rdstate();
    const std::istream::pos_type original_pos = stream.tellg();
    const bool can_seek = original_pos != std::istream::pos_type(-1);
    stream.clear();
    array<unsigned char, PNG_SIG_SIZE> pngsig{};
    constexpr array<unsigned char, PNG_SIG_SIZE> expected{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    stream.read(reinterpret_cast<char*>(pngsig.data()), static_cast<std::streamsize>(pngsig.size()));
    const std::streamsize read_bytes = stream.gcount();
    const bool is_png = (read_bytes == static_cast<std::streamsize>(pngsig.size())) &&
                        (std::memcmp(pngsig.data(), expected.data(), PNG_SIG_SIZE) == 0);
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
    vector<byte> encoded;
    if (!ReadImageStream(stream, encoded)) {
        RADRAY_ERR_LOG("failed to read PNG input");
        return std::nullopt;
    }

    ImageData image;
    RadRayPngState state{};
    state.Input = reinterpret_cast<const unsigned char*>(encoded.data());
    state.InputRemaining = encoded.size();
    auto cleanup = MakeScopeGuard([&]() { radray_png_destroy(&state); });
    const int64_t alpha = settings.AddAlphaIfRGB.has_value() ? static_cast<int64_t>(*settings.AddAlphaIfRGB) : -1;
    if (!radray_png_read_header(&state, alpha)) {
        RADRAY_ERR_LOG("libpng read header failed: {}", state.Error);
        return std::nullopt;
    }
    if (!PngImageFormat(state, image.Format)) {
        RADRAY_ERR_LOG("unsupported PNG pixel format");
        return std::nullopt;
    }
    image.Width = state.Width;
    image.Height = state.Height;
    image.Data = make_unique<byte[]>(state.ByteSize);
    if (!radray_png_read_pixels(&state, image.Data.get(), settings.IsFlipY)) {
        RADRAY_ERR_LOG("libpng read pixels failed: {}", state.Error);
        return std::nullopt;
    }
    LogPngWarning(state);
    return std::make_optional(std::move(image));
}

bool ImageData::WritePNG(PNGWriteSettings settings) const {
    if (settings.FilePath.empty() || Data == nullptr || Width == 0 || Height == 0) {
        RADRAY_ERR_LOG("WritePNG requires a destination and nonempty image data");
        return false;
    }
    if (Format != ImageFormat::RGB8_BYTE && Format != ImageFormat::RGBA8_BYTE) {
        RADRAY_ERR_LOG("WritePNG unsupported image format {}, only RGB8/RGBA8 are supported", Format);
        return false;
    }
    const size_t pixelBytes = ImageData::FormatSize(Format);
    const size_t maxSize = std::numeric_limits<size_t>::max();
    if (Width > maxSize / pixelBytes) {
        RADRAY_ERR_LOG("WritePNG row size exceeds supported storage");
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(Width) * pixelBytes;
    if (Height > maxSize / rowBytes) {
        RADRAY_ERR_LOG("WritePNG image size exceeds supported storage");
        return false;
    }

    const string path{settings.FilePath};
    Nullable<std::FILE*> destination{std::fopen(path.c_str(), "wb")};
    if (!destination) {
        RADRAY_ERR_LOG("WritePNG cannot open file '{}'", settings.FilePath);
        return false;
    }
    auto closeFile = MakeScopeGuard([&]() {
        if (destination) {
            std::fclose(destination.Get());
        }
    });
    {
        RadRayPngState state{};
        state.Width = Width;
        state.Height = Height;
        state.Channels = Format == ImageFormat::RGB8_BYTE ? 3 : 4;
        auto cleanup = MakeScopeGuard([&]() { radray_png_destroy(&state); });
        if (!radray_png_write(&state, destination.Get(), Data.get(), settings.IsFlipY)) {
            RADRAY_ERR_LOG("libpng write failed: {}", state.Error);
            return false;
        }
        LogPngWarning(state);
    }
    if (std::fclose(destination.Release()) != 0) {
        RADRAY_ERR_LOG("WritePNG failed to close file '{}'", settings.FilePath);
        return false;
    }
    return true;
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

bool ImageData::WritePNG(PNGWriteSettings settings) const {
    RADRAY_UNUSED(settings);
    RADRAY_ERR_LOG("libpng support is not enabled");
    return false;
}
#endif

#ifdef RADRAY_ENABLE_JPEG

namespace {

bool ReadJpegStream(std::istream& stream, vector<byte>& encoded) {
    const Nullable<std::streambuf*> source{stream.rdbuf()};
    if (!stream.good() || !source) {
        return false;
    }
    const auto originalPos = source->pubseekoff(0, std::ios::cur, std::ios::in);
    if (originalPos == std::streampos(-1)) {
        return ReadImageStream(stream, encoded);
    }
    const auto endPos = source->pubseekoff(0, std::ios::end, std::ios::in);
    if (source->pubseekpos(originalPos, std::ios::in) == std::streampos(-1) ||
        endPos == std::streampos(-1) || endPos < originalPos) {
        return false;
    }
    const auto length = endPos - originalPos;
    if (!std::in_range<size_t>(length) || !std::in_range<std::streamsize>(length)) {
        return false;
    }
    const size_t size = static_cast<size_t>(length);
    if (size == 0 || size > encoded.max_size()) {
        return false;
    }
    encoded.resize(size);
    const auto count = source->sgetn(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(length));
    if (source->pubseekpos(originalPos, std::ios::in) == std::streampos(-1)) {
        return false;
    }
    return count == length;
}

}  // namespace

bool ImageData::IsJPEG(std::istream& stream) {
    if (!stream.good()) {
        return false;
    }
    const auto original_state = stream.rdstate();
    const std::istream::pos_type original_pos = stream.tellg();
    const bool can_seek = original_pos != std::istream::pos_type(-1);
    stream.clear();
    array<unsigned char, 3> sig{};
    stream.read(reinterpret_cast<char*>(sig.data()), static_cast<std::streamsize>(sig.size()));
    const std::streamsize read_bytes = stream.gcount();
    const bool is_jpeg = read_bytes >= 2 &&
                         sig[0] == 0xFF &&
                         sig[1] == 0xD8;
    stream.clear();
    if (can_seek) {
        stream.seekg(original_pos);
    } else {
        stream.seekg(0, std::ios::beg);
    }
    stream.clear(original_state);
    return is_jpeg;
}

std::optional<ImageData> ImageData::LoadJPEG(std::istream& stream, JPEGLoadSettings settings) {
    vector<byte> encoded;
    if (!ReadJpegStream(stream, encoded)) {
        RADRAY_ERR_LOG("failed to read JPEG stream");
        return std::nullopt;
    }

    ImageData image;
    RadRayJpegState state{};
    state.Input = reinterpret_cast<const unsigned char*>(encoded.data());
    state.InputSize = encoded.size();
    auto cleanup = MakeScopeGuard([&]() { radray_jpeg_destroy(&state); });
    const int64_t alpha = settings.AddAlphaIfRGB.has_value() ? static_cast<int64_t>(*settings.AddAlphaIfRGB) : -1;
    if (!radray_jpeg_read_header(&state, alpha)) {
        RADRAY_ERR_LOG("libjpeg read header failed: {}", state.Error);
        return std::nullopt;
    }
    image.Width = state.Width;
    image.Height = state.Height;
    image.Format = state.Channels == 4 ? ImageFormat::RGBA8_BYTE : ImageFormat::RGB8_BYTE;
    image.Data = make_unique<byte[]>(state.ByteSize);
    if (!radray_jpeg_read_pixels(&state, image.Data.get(), settings.IsFlipY)) {
        RADRAY_ERR_LOG("libjpeg read pixels failed: {}", state.Error);
        return std::nullopt;
    }
    if (state.Warning[0] != '\0') {
        RADRAY_WARN_LOG("libjpeg warning: {}", state.Warning);
    }
    return std::make_optional(std::move(image));
}

#else
bool ImageData::IsJPEG(std::istream& stream) {
    RADRAY_UNUSED(stream);
    RADRAY_ERR_LOG("libjpeg support is not enabled");
    return false;
}

std::optional<ImageData> ImageData::LoadJPEG(std::istream& stream, JPEGLoadSettings settings) {
    RADRAY_UNUSED(stream);
    RADRAY_UNUSED(settings);
    RADRAY_ERR_LOG("libjpeg support is not enabled");
    return std::nullopt;
}
#endif

PixelCompareResult ImageData::CompareImageRGBA8(const ImageData& actual, const ImageData& expected, uint8_t tolerance) noexcept {
    if (actual.Format != ImageFormat::RGBA8_BYTE || expected.Format != ImageFormat::RGBA8_BYTE) {
        return PixelCompareResult{"only RGBA8_BYTE is supported"};
    }
    if (actual.Width != expected.Width || actual.Height != expected.Height) {
        return PixelCompareResult{"image size mismatch"};
    }
    if (actual.Data == nullptr || expected.Data == nullptr) {
        return PixelCompareResult{"image data is null"};
    }
    PixelCompareResult out{};
    const size_t pxCount = static_cast<size_t>(actual.Width) * actual.Height;
    for (size_t i = 0; i < pxCount; ++i) {
        const size_t p = i * 4;
        for (uint32_t c = 0; c < 4; ++c) {
            const uint8_t a = std::to_integer<uint8_t>(actual.Data[p + c]);
            const uint8_t e = std::to_integer<uint8_t>(expected.Data[p + c]);
            const int delta = static_cast<int>(a) - static_cast<int>(e);
            const uint8_t absDelta = static_cast<uint8_t>(delta < 0 ? -delta : delta);
            if (absDelta > tolerance) {
                ++out.MismatchCount;
                if (out.FirstMismatchPixel == static_cast<size_t>(-1)) {
                    out.FirstMismatchPixel = i;
                    out.FirstMismatchChannel = c;
                    out.ActualValue = a;
                    out.ExpectedValue = e;
                }
                break;
            }
        }
    }
    return out;
}

ImageData ImageData::ImageDiffRGBA8(const ImageData& actual, const ImageData& expected) noexcept {
    if (actual.Format != ImageFormat::RGBA8_BYTE || expected.Format != ImageFormat::RGBA8_BYTE) {
        return {};
    }
    if (actual.Width != expected.Width || actual.Height != expected.Height) {
        return {};
    }
    if (actual.Data == nullptr || expected.Data == nullptr) {
        return {};
    }
    ImageData diff{};
    diff.Width = actual.Width;
    diff.Height = actual.Height;
    diff.Format = ImageFormat::RGBA8_BYTE;
    diff.Data = std::make_unique<byte[]>(diff.GetSize());
    const size_t pxCount = static_cast<size_t>(actual.Width) * actual.Height;
    for (size_t i = 0; i < pxCount; ++i) {
        const size_t p = i * 4;
        for (uint32_t c = 0; c < 3; ++c) {
            const int a = std::to_integer<int>(actual.Data[p + c]);
            const int e = std::to_integer<int>(expected.Data[p + c]);
            const int delta = a - e;
            diff.Data[p + c] = static_cast<byte>(delta < 0 ? -delta : delta);
        }
        diff.Data[p + 3] = static_cast<byte>(0xFF);
    }
    return diff;
}

std::string_view format_as(ImageFormat val) noexcept {
    return EnumNameOr(val);
}

}  // namespace radray
