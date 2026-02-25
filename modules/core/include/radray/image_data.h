#pragma once

#include <span>
#include <string_view>
#include <optional>
#include <istream>

#include <radray/types.h>

namespace radray {

enum class ImageFormat {
    R8_BYTE,     // 1
    R16_USHORT,  // 2
    R16_HALF,    // 2
    R32_FLOAT,   // 4

    RG8_BYTE,     // 2
    RG16_USHORT,  // 4
    RG16_HALF,    // 4
    RG32_FLOAT,   // 8

    RGB32_FLOAT,  // 12

    RGBA8_BYTE,     // 4
    RGBA16_USHORT,  // 8
    RGBA16_HALF,    // 8
    RGBA32_FLOAT,   // 16

    RGB8_BYTE,    // 3
    RGB16_USHORT  // 6
};

class PNGLoadSettings {
public:
    std::optional<uint32_t> AddAlphaIfRGB;
    bool IsFlipY{false};
};

class PNGWriteSettings {
public:
    std::string_view FilePath{};
    bool IsFlipY{false};
};

struct PixelCompareResult {
    string ErrorReason{};
    size_t MismatchCount{0};
    size_t FirstMismatchPixel{static_cast<size_t>(-1)};
    uint32_t FirstMismatchChannel{0};
    uint8_t ActualValue{0};
    uint8_t ExpectedValue{0};

    bool IsMatch() const noexcept { return MismatchCount == 0; }
};

class ImageData {
public:
    ImageData() noexcept = default;
    ImageData(const ImageData&) noexcept;
    ImageData(ImageData&&) noexcept;
    ImageData& operator=(const ImageData&) noexcept;
    ImageData& operator=(ImageData&&) noexcept;
    ~ImageData() noexcept = default;

    size_t GetSize() const noexcept;
    std::span<const byte> GetSpan() const noexcept;

    ImageData RGB8ToRGBA8(uint8_t alpha) const noexcept;
    void FlipY() noexcept;

    friend void swap(ImageData& a, ImageData& b) noexcept;

    static size_t FormatSize(ImageFormat format) noexcept;
    static bool IsPNG(std::istream& stream);
    static std::optional<ImageData> LoadPNG(std::istream& stream, PNGLoadSettings settings = PNGLoadSettings{});
    bool WritePNG(PNGWriteSettings settings = PNGWriteSettings{}) const;

    static PixelCompareResult CompareImageRGBA8(const ImageData& actual, const ImageData& expected, uint8_t tolerance) noexcept;
    static ImageData ImageDiffRGBA8(const ImageData& actual, const ImageData& expected) noexcept;

    unique_ptr<byte[]> Data{};
    uint32_t Width{0};
    uint32_t Height{0};
    ImageFormat Format{ImageFormat::R8_BYTE};
};

std::string_view format_as(ImageFormat val) noexcept;

}  // namespace radray
