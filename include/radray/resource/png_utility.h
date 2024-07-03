#pragma once

#include <istream>
#include <filesystem>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

class ImageData;

namespace resource {

enum class PngColorType {
    UNKNOWN,
    GRAY,
    RGB,
    RGB_ALPHA,
    GRAY_ALPHA
};

class PngData {
public:
    PngData() noexcept = default;
    bool LoadFromFile(const std::filesystem::path& path, uint64_t fileOffset = 0);
    bool LoadFromStream(std::istream& stream);
    void MoveToImageData(ImageData* o);

public:
    std::unique_ptr<byte[]> data{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t bitDepth{0};
    uint32_t channel{0};
    PngColorType colorType{PngColorType::UNKNOWN};
};

const char* to_string(PngColorType val) noexcept;

}  // namespace resource
}  // namespace radray

template <class CharT>
struct std::formatter<radray::resource::PngColorType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::resource::PngColorType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};
