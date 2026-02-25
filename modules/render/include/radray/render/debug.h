#pragma once

#include <optional>
#include <string_view>
#include <stdexcept>
#include <utility>

#include <radray/image_data.h>
#include <radray/render/common.h>

namespace radray::render {

class Dxc;

struct TextureReadbackLayout {
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BytesPerPixel{0};
    uint32_t RowPitchBytes{0};
    uint32_t TightRowBytes{0};
};

struct TextureReadbackResult {
    TextureFormat Format{TextureFormat::UNKNOWN};
    TextureReadbackLayout Layout{};
    vector<byte> Data{};
};

std::optional<TextureReadbackResult> ReadbackTexture2D(
    Device* device,
    CommandQueue* queue,
    Texture* src,
    TextureState srcStateBeforeCopy,
    uint32_t mipLevel = 0,
    uint32_t arrayLayer = 0) noexcept;

std::optional<ImageData> PackReadbackToTightRGBA8(const TextureReadbackResult& in) noexcept;

class DebugException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DebugContextDescriptor {
    DeviceDescriptor DeviceDesc{};
    QueueType Queue{QueueType::Direct};
    uint32_t QueueIndex{0};
    bool CreateDxc{false};
};

struct DebugOffscreenTargetDescriptor {
    uint32_t Width{0};
    uint32_t Height{0};
    TextureFormat Format{TextureFormat::RGBA8_UNORM};
    string Name{"debug_offscreen_rt"};
};

struct DebugOffscreenTarget {
    unique_ptr<Texture> Texture{};
    unique_ptr<TextureView> RTV{};
    TextureFormat Format{TextureFormat::UNKNOWN};
    uint32_t Width{0};
    uint32_t Height{0};
};

class DebugContext;

class DebugPass {
public:
    virtual ~DebugPass() noexcept = default;
    virtual void Record(DebugContext& ctx, CommandBuffer* cmd, DebugOffscreenTarget& target) = 0;
};

class DebugContext {
public:
    static DebugContext Create(const DebugContextDescriptor& desc);

    Device* GetDevice() const noexcept { return _device.get(); }
    CommandQueue* GetQueue() const noexcept { return _queue; }

#ifdef RADRAY_ENABLE_DXC
    Dxc* GetDxc() const noexcept { return _dxc.get(); }
#endif

    DebugOffscreenTarget CreateOffscreenTarget(const DebugOffscreenTargetDescriptor& desc);

    void ExecutePass(
        DebugOffscreenTarget& target,
        DebugPass& pass,
        TextureState before = TextureState::Undefined,
        TextureState after = TextureState::CopySource);

    ImageData ReadbackRGBA8(
        const DebugOffscreenTarget& target,
        TextureState srcStateBeforeCopy = TextureState::CopySource,
        uint32_t mipLevel = 0,
        uint32_t arrayLayer = 0);

    void WritePNG(const ImageData& img, const PNGWriteSettings& settings) const;

private:
    explicit DebugContext(shared_ptr<Device> device, CommandQueue* queue) noexcept
        : _device(std::move(device)), _queue(queue) {}

private:
    shared_ptr<Device> _device{};
    CommandQueue* _queue{};
#ifdef RADRAY_ENABLE_DXC
    shared_ptr<Dxc> _dxc{};
#endif
};

struct PixelCompareResult {
    size_t MismatchCount{0};
    size_t FirstMismatchPixel{static_cast<size_t>(-1)};
    uint32_t FirstMismatchChannel{0};
    uint8_t ActualValue{0};
    uint8_t ExpectedValue{0};

    bool IsMatch() const noexcept { return MismatchCount == 0; }
};

PixelCompareResult CompareImageRGBA8(const ImageData& actual, const ImageData& expected, uint8_t tolerance = 0);

ImageData BuildDiffImageRGBA8(const ImageData& actual, const ImageData& expected);

void WriteImageComparisonArtifacts(
    const DebugContext& ctx,
    const ImageData& actual,
    const ImageData& expected,
    std::string_view outputDir);

}  // namespace radray::render

