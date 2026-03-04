#pragma once

#include <optional>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <mutex>
#include <vector>

#include <radray/file.h>
#include <radray/image_data.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>

namespace radray::render {

class DebugException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class OffScreenTestContext {
public:
    struct TextureReadbackResult {
        vector<byte> Data{};
        TextureFormat Format{TextureFormat::UNKNOWN};
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t BytesPerPixel{0};
        uint32_t RowPitchBytes{0};
        uint32_t TightRowBytes{0};
    };

    struct RasterShaders {
        unique_ptr<Shader> VS;
        string VSEntry;
        unique_ptr<Shader> PS;
        string PSEntry;
    };

    OffScreenTestContext(
        std::string_view name,
        DeviceDescriptor deviceDesc,
        bool needDxc,
        Eigen::Vector2i rtSize,
        TextureFormat rtFormat);
    OffScreenTestContext(const OffScreenTestContext&) = delete;
    OffScreenTestContext& operator=(const OffScreenTestContext&) = delete;
    OffScreenTestContext(OffScreenTestContext&&) = delete;
    OffScreenTestContext& operator=(OffScreenTestContext&&) = delete;
    virtual ~OffScreenTestContext() noexcept;

    ImageData Run();

    virtual void Init(CommandBuffer* cmd, Fence* fence) = 0;

    virtual void ExecutePass(CommandBuffer* cmd, Fence* fence) = 0;

    ImageData LoadBaseline(std::string_view name = {}) const;
    TextureReadbackResult ReadbackTexture2D(Texture* target, TextureState before, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);
    ImageData PackReadbackRGBA8(const TextureReadbackResult& readback);
    void WriteImageComparisonArtifacts(const ImageData& actual, const ImageData& expected, std::string_view name);
    RasterShaders CompileRasterShaders(std::string_view src, HlslShaderModel sm = HlslShaderModel::SM60);
    void UploadBuffer(Buffer* dst, std::span<const byte> data);
    void Submit(CommandBuffer* cmd, Fence* fence);

    bool HasCapturedRenderErrors() const;
    vector<string> GetCapturedRenderErrors() const;
    void ClearCapturedRenderErrors();
    static void DeviceLogBridge(LogLevel level, std::string_view message, void* userData);
    void OnDeviceLog(LogLevel level, std::string_view message);

public:
    string _name;
    std::filesystem::path _projectDir;
    std::filesystem::path _testEnvDir;
    std::filesystem::path _assetsDir;
    std::filesystem::path _testArtifactsDir;
    bool _needUpdateBaseline;
    unique_ptr<InstanceVulkan> _vkIns;
    shared_ptr<Device> _device;
    CommandQueue* _queue;
#ifdef RADRAY_ENABLE_DXC
    shared_ptr<Dxc> _dxc;
#endif
    unique_ptr<Texture> _rt;
    unique_ptr<TextureView> _rtv;
    TextureState _rtState{TextureState::Undefined};

    RenderLogCallback _prevLogCallback{nullptr};
    void* _prevLogUserData{nullptr};
    mutable std::mutex _capturedRenderErrorsMutex;
    std::vector<std::string> _capturedRenderErrors;
};

}  // namespace radray::render
