#pragma once

#include <istream>
#include <filesystem>
#include <string_view>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

class ImageData;

namespace resource {

enum class JpgColorType {
    UNKNOWN,
    GRAYSCALE,
    RGB
};

class JpgData {
public:
    JpgData() noexcept = default;
    bool LoadFromFile(const std::filesystem::path& path, uint64_t fileOffset = 0);
    bool LoadFromStream(std::istream& stream);
    void MoveToImageData(ImageData* o);

public:
    radray::unique_ptr<byte[]> data{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t component{0};
    JpgColorType colorType{JpgColorType::UNKNOWN};
};

std::string_view to_string(JpgColorType val) noexcept;

}  // namespace resource
}  // namespace radray

template <class CharT>
struct fmt::formatter<radray::resource::JpgColorType, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(radray::resource::JpgColorType val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(to_string(val), ctx);
    }
};
