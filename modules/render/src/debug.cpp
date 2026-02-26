#include <radray/render/debug.h>

#include <filesystem>
#include <cstring>
#include <limits>
#include <string>
#include <fstream>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/platform.h>
#include <radray/render/render_utility.h>
#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

OffScreenTestContext::OffScreenTestContext(
    std::string_view name,
    DeviceDescriptor deviceDesc,
    bool needDxc,
    Eigen::Vector2i rtSize,
    TextureFormat rtFormat)
    : _name(name) {
    {
        auto v = radray::GetEnv("RADRAY_PROJECT_DIR");
        if (v.empty()) {
            _projectDir = std::filesystem::current_path();
        } else {
            _projectDir = std::filesystem::path{v};
        }
    }
    {
        auto v = radray::GetEnv("RADRAY_TEST_ENV_DIR");
        if (v.empty()) {
            _testEnvDir = std::filesystem::current_path();
        } else {
            _testEnvDir = std::filesystem::path{v};
        }
    }
    {
        auto v = radray::GetEnv("RADRAY_ASSETS_DIR");
        if (v.empty()) {
            _assetsDir = std::filesystem::current_path();
        } else {
            _assetsDir = std::filesystem::path{v};
        }
    }
    {
        auto v = radray::GetEnv("RADRAY_TEST_ARTIFACTS_DIR");
        if (v.empty()) {
            _testArtifactsDir = _testEnvDir / "test_artifacts";
        } else {
            _testArtifactsDir = std::filesystem::path{v};
        }
    }
    {
        auto v = radray::GetEnv("RADRAY_TEST_UPDATE_BASELINE");
        _needUpdateBaseline = (v == "1");
    }
    std::visit(
        [this](auto& desc) {
            using DescT = std::decay_t<decltype(desc)>;
            if constexpr (std::is_same_v<DescT, D3D12DeviceDescriptor>) {
                _prevLogCallback = desc.LogCallback;
                _prevLogUserData = desc.LogUserData;
                desc.LogCallback = &OffScreenTestContext::DeviceLogBridge;
                desc.LogUserData = this;
            }
        },
        deviceDesc);
    if (std::holds_alternative<VulkanDeviceDescriptor>(deviceDesc)) {
        auto appName = fmt::format("{}_vk_instance", _name);
        render::VulkanInstanceDescriptor insDesc{
            appName,
            1,
            "RadRay",
            1,
            true,
            false,
            &OffScreenTestContext::DeviceLogBridge,
            this};
        _vkIns = CreateVulkanInstance(insDesc).Unwrap();
    }
    _device = CreateDevice(deviceDesc).Unwrap();
    _queue = _device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
#ifdef RADRAY_ENABLE_DXC
    if (needDxc) {
        _dxc = CreateDxc().Unwrap();
    }
#else
    if (needDxc) {
        throw DebugException("dxc is required but RADRAY_ENABLE_DXC is off");
    }
#endif
    if (rtSize.x() <= 0 || rtSize.y() <= 0) {
        throw DebugException(fmt::format("invalid render target size: {}x{}", rtSize.x(), rtSize.y()));
    }
    TextureDescriptor rtDesc{
        TextureDimension::Dim2D,
        static_cast<uint32_t>(rtSize.x()),
        static_cast<uint32_t>(rtSize.y()),
        1,
        1,
        1,
        rtFormat,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        ResourceHint::None,
        "test_offscreen_rt"};
    _rt = _device->CreateTexture(rtDesc).Unwrap();
    TextureViewDescriptor rtvDesc{
        _rt.get(),
        TextureDimension::Dim2D,
        rtFormat,
        SubresourceRange{0, 1, 0, 1},
        TextureViewUsage::RenderTarget};
    _rtv = _device->CreateTextureView(rtvDesc).Unwrap();
}

OffScreenTestContext::~OffScreenTestContext() noexcept {
    _rtv.reset();
    _rt.reset();
    _queue = nullptr;
    _device.reset();
    _dxc.reset();
    if (_vkIns) {
        DestroyVulkanInstance(std::move(_vkIns));
    }
}

OffScreenTestContext::TextureReadbackResult OffScreenTestContext::ReadbackTexture2D(
    Texture* src,
    TextureState before,
    uint32_t mipLevel,
    uint32_t arrayLayer) {
    const TextureDescriptor srcDesc = src->GetDesc();
    if (srcDesc.Dim != TextureDimension::Dim2D && srcDesc.Dim != TextureDimension::Dim2DArray) {
        throw DebugException(fmt::format("ReadbackTexture2D only supports 2D textures, got {}", srcDesc.Dim));
    }
    if (srcDesc.MipLevels == 0 || mipLevel >= srcDesc.MipLevels) {
        throw DebugException(fmt::format("ReadbackTexture2D invalid mip level {}, total {}", mipLevel, srcDesc.MipLevels));
    }
    if (srcDesc.DepthOrArraySize == 0 || arrayLayer >= srcDesc.DepthOrArraySize) {
        throw DebugException(fmt::format("ReadbackTexture2D invalid array layer {}, total {}", arrayLayer, srcDesc.DepthOrArraySize));
    }
    const uint32_t bpp = GetTextureFormatBytesPerPixel(srcDesc.Format);
    if (bpp == 0) {
        throw DebugException(fmt::format("ReadbackTexture2D invalid texture format {}", srcDesc.Format));
    }
    const uint32_t width = std::max(srcDesc.Width >> mipLevel, 1u);
    const uint32_t height = std::max(srcDesc.Height >> mipLevel, 1u);
    const uint64_t tightRowBytes = static_cast<uint64_t>(width) * bpp;
    const uint64_t rowPitchAlign = std::max<uint64_t>(1, _device->GetDetail().TextureDataPitchAlignment);
    const uint64_t rowPitchBytes = Align(tightRowBytes, rowPitchAlign);
    const uint64_t totalBytes = rowPitchBytes * height;
    if (rowPitchBytes > std::numeric_limits<uint32_t>::max() ||
        tightRowBytes > std::numeric_limits<uint32_t>::max() ||
        totalBytes > std::numeric_limits<uint32_t>::max()) {
        throw DebugException("ReadbackTexture2D readback size overflow");
    }
    BufferDescriptor readbackDesc{
        totalBytes,
        MemoryType::ReadBack,
        BufferUse::MapRead | BufferUse::CopyDestination,
        ResourceHint::Dedicated,
        "debug_readback_buffer"};
    auto readbackBuf = _device->CreateBuffer(readbackDesc).Unwrap();
    auto cmd = _device->CreateCommandBuffer(_queue).Unwrap();
    auto fence = _device->CreateFence().Unwrap();
    cmd->Begin();
    {
        ResourceBarrierDescriptor texBarrier = BarrierTextureDescriptor{
            src,
            before,
            TextureState::CopySource,
            nullptr,
            false,
            true,
            SubresourceRange{arrayLayer, 1, mipLevel, 1}};
        cmd->ResourceBarrier(std::span{&texBarrier, 1});
    }
    cmd->CopyTextureToBuffer(readbackBuf.get(), 0, src, SubresourceRange{arrayLayer, 1, mipLevel, 1});
    cmd->End();
    CommandBuffer* cmdRaw[] = {cmd.get()};
    CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = cmdRaw;
    submitDesc.SignalFence = fence.get();
    _queue->Submit(submitDesc);
    fence->Wait();
    void* mapped = readbackBuf->Map(0, totalBytes);
    if (mapped == nullptr) {
        throw DebugException("ReadbackTexture2D map readback buffer failed");
    }
    vector<byte> data{totalBytes};
    std::memcpy(data.data(), mapped, totalBytes);
    readbackBuf->Unmap(0, totalBytes);
    return {
        std::move(data),
        srcDesc.Format,
        width,
        height,
        bpp,
        static_cast<uint32_t>(rowPitchBytes),
        static_cast<uint32_t>(tightRowBytes)};
}

ImageData OffScreenTestContext::PackReadbackRGBA8(const TextureReadbackResult& readback) {
    if (readback.Width == 0 || readback.Height == 0 || readback.BytesPerPixel != 4) {
        throw DebugException("PackReadbackToTightRGBA8 invalid readback layout");
    }
    const uint64_t expectBytes = static_cast<uint64_t>(readback.RowPitchBytes) * readback.Height;
    if (readback.Data.size() < expectBytes) {
        throw DebugException(fmt::format("PackReadbackToTightRGBA8 input data too small, need {}, got {}", expectBytes, readback.Data.size()));
    }

    ImageData out{};
    out.Width = readback.Width;
    out.Height = readback.Height;
    out.Format = ImageFormat::RGBA8_BYTE;
    out.Data = make_unique<byte[]>(static_cast<size_t>(out.GetSize()));
    const bool isBGRA = (readback.Format == TextureFormat::BGRA8_UNORM || readback.Format == TextureFormat::BGRA8_UNORM_SRGB);
    for (uint32_t y = 0; y < readback.Height; ++y) {
        const byte* srcRow = readback.Data.data() + static_cast<size_t>(readback.RowPitchBytes) * y;
        byte* dstRow = out.Data.get() + static_cast<size_t>(readback.TightRowBytes) * y;
        if (isBGRA) {
            for (uint32_t x = 0; x < readback.Width; ++x) {
                const size_t p = static_cast<size_t>(x) * 4;
                dstRow[p + 0] = srcRow[p + 2];
                dstRow[p + 1] = srcRow[p + 1];
                dstRow[p + 2] = srcRow[p + 0];
                dstRow[p + 3] = srcRow[p + 3];
            }
        } else {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(readback.TightRowBytes));
        }
    }
    return out;
}

ImageData OffScreenTestContext::Run() {
    auto cmd = _device->CreateCommandBuffer(_queue).Unwrap();
    auto fence = _device->CreateFence().Unwrap();
    cmd->Begin();
    Init(cmd.get(), fence.get());
    {
        const ResourceBarrierDescriptor b = BarrierTextureDescriptor{_rt.get(), TextureState::Undefined, TextureState::RenderTarget};
        cmd->ResourceBarrier(std::span{&b, 1});
        _rtState = TextureState::RenderTarget;
    }
    ExecutePass(cmd.get(), fence.get());
    {
        const ResourceBarrierDescriptor b = BarrierTextureDescriptor{_rt.get(), _rtState, TextureState::CopySource};
        cmd->ResourceBarrier(std::span{&b, 1});
        _rtState = TextureState::CopySource;
    }

    const TextureDescriptor srcDesc = _rt->GetDesc();
    const uint32_t bpp = GetTextureFormatBytesPerPixel(srcDesc.Format);
    if (bpp == 0) {
        throw DebugException(fmt::format("ReadbackTexture2D invalid texture format {}", srcDesc.Format));
    }
    const uint32_t width = srcDesc.Width;
    const uint32_t height = srcDesc.Height;
    const uint64_t tightRowBytes = static_cast<uint64_t>(width) * bpp;
    const uint64_t rowPitchAlign = std::max<uint64_t>(1, _device->GetDetail().TextureDataPitchAlignment);
    const uint64_t rowPitchBytes = Align(tightRowBytes, rowPitchAlign);
    const uint64_t totalBytes = rowPitchBytes * height;
    if (rowPitchBytes > std::numeric_limits<uint32_t>::max() ||
        tightRowBytes > std::numeric_limits<uint32_t>::max() ||
        totalBytes > std::numeric_limits<uint32_t>::max()) {
        throw DebugException("ReadbackTexture2D readback size overflow");
    }
    BufferDescriptor readbackDesc{
        totalBytes,
        MemoryType::ReadBack,
        BufferUse::MapRead | BufferUse::CopyDestination,
        ResourceHint::Dedicated,
        "test_rt_readback_buffer"};
    auto readbackBuf = _device->CreateBuffer(readbackDesc).Unwrap();
    cmd->CopyTextureToBuffer(readbackBuf.get(), 0, _rt.get(), SubresourceRange{0, 1, 0, 1});

    cmd->End();
    {
        CommandBuffer* submitBuffers[] = {cmd.get()};
        CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = submitBuffers;
        submitDesc.SignalFence = fence.get();
        _queue->Submit(submitDesc);
        fence->Wait();
    }
    void* mapped = readbackBuf->Map(0, totalBytes);
    if (mapped == nullptr) {
        throw DebugException("ReadbackTexture2D map readback buffer failed");
    }
    vector<byte> data{totalBytes};
    std::memcpy(data.data(), mapped, totalBytes);
    readbackBuf->Unmap(0, totalBytes);
    TextureReadbackResult result{
        std::move(data),
        srcDesc.Format,
        width,
        height,
        bpp,
        static_cast<uint32_t>(rowPitchBytes),
        static_cast<uint32_t>(tightRowBytes)};
    auto packed = PackReadbackRGBA8(result);
    if (_needUpdateBaseline) {
        auto outPath = _testEnvDir / fmt::format("{}_baseline.png", _name);
        packed.WritePNG({outPath.string(), false});
    }
    return packed;
}

ImageData OffScreenTestContext::LoadBaseline(std::string_view name) const {
    auto inPath = _testEnvDir / (name.empty() ? fmt::format("{}_baseline.png", _name) : string{name});
    std::ifstream file{inPath, std::ios::binary};
    return ImageData::LoadPNG(file, {.AddAlphaIfRGB = 0xFF, .IsFlipY = false}).value();
}

void OffScreenTestContext::WriteImageComparisonArtifacts(const ImageData& actual, const ImageData& expected, std::string_view name) {
    const std::filesystem::path outPath{_testArtifactsDir};
    std::error_code ec;
    std::filesystem::create_directories(outPath, ec);

    const ImageData diff = ImageData::ImageDiffRGBA8(actual, expected);
    actual.WritePNG({(outPath / fmt::format("{}_actual.png", name)).string(), false});
    expected.WritePNG({(outPath / fmt::format("{}_expected.png", name)).string(), false});
    diff.WritePNG({(outPath / fmt::format("{}_diff.png", name)).string(), false});
}

OffScreenTestContext::RasterShaders OffScreenTestContext::CompileRasterShaders(std::string_view src, HlslShaderModel sm) {
    vector<string> defines;
    auto backend = _device->GetBackend();
    if (backend == RenderBackend::Vulkan) {
        defines.emplace_back("VULKAN");
    } else if (backend == RenderBackend::D3D12) {
        defines.emplace_back("D3D12");
    } else if (backend == RenderBackend::Metal) {
        defines.emplace_back("METAL");
    } else {
        throw DebugException(fmt::format("unsupported backend for shader compilation {}", backend));
    }
    vector<std::string_view> defineViews;
    for (const auto& d : defines) {
        defineViews.emplace_back(d);
    }
    vector<string> includes;
    includes.emplace_back((_projectDir / "shaderlib").generic_string());
    vector<std::string_view> includeViews;
    for (const auto& i : includes) {
        includeViews.emplace_back(i);
    }
    auto entryVS = "VSMain";
    auto entryPS = "PSMain";
    auto vsBin = _dxc->Compile(
                         src,
                         entryVS,
                         ShaderStage::Vertex,
                         sm,
                         false,
                         defineViews,
                         includeViews,
                         backend != RenderBackend::D3D12)
                     .value();
    auto psBin = _dxc->Compile(
                         src,
                         entryPS,
                         ShaderStage::Pixel,
                         sm,
                         false,
                         defineViews,
                         includeViews,
                         backend != RenderBackend::D3D12)
                     .value();
    unique_ptr<Shader> vsShader, psShader;
    if (backend == RenderBackend::Metal) {
        SpirvToMslOption mslOption{
            3,
            0,
            0,
#ifdef RADRAY_PLATFORM_MACOS
            MslPlatform::MacOS,
#elif defined(RADRAY_PLATFORM_IOS)
            MslPlatform::IOS,
#else
            MslPlatform::MacOS,
#endif
            true,
            false};
        auto vsMsl = ConvertSpirvToMsl(vsBin.Data, entryVS, ShaderStage::Vertex, mslOption).value();
        vsShader = _device->CreateShader({vsMsl.GetBlob(), ShaderBlobCategory::MSL}).Unwrap();
        auto psMsl = ConvertSpirvToMsl(psBin.Data, entryPS, ShaderStage::Pixel, mslOption).value();
        psShader = _device->CreateShader({psMsl.GetBlob(), ShaderBlobCategory::MSL}).Unwrap();
    } else {
        vsShader = _device->CreateShader({vsBin.Data, vsBin.Category}).Unwrap();
        psShader = _device->CreateShader({psBin.Data, psBin.Category}).Unwrap();
    }
    return {std::move(vsShader), entryVS, std::move(psShader), entryPS};
}

void OffScreenTestContext::UploadBuffer(Buffer* dst, std::span<const byte> data) {
    auto upload = _device->CreateBuffer({data.size(), MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, "vb_upload"}).Unwrap();
    void* mapped = upload->Map(0, data.size());
    if (mapped == nullptr) {
        throw DebugException("UploadBuffer map failed");
    }
    std::memcpy(mapped, data.data(), data.size());
    upload->Unmap(0, data.size());
    auto cmd = _device->CreateCommandBuffer(_queue).Unwrap();
    auto fence = _device->CreateFence().Unwrap();
    cmd->Begin();
    {
        ResourceBarrierDescriptor b[] = {
            BarrierBufferDescriptor{dst, BufferState::Common, BufferState::CopyDestination}};
        cmd->ResourceBarrier(b);
    }
    cmd->CopyBufferToBuffer(dst, 0, upload.get(), 0, data.size());
    {
        ResourceBarrierDescriptor b[] = {
            BarrierBufferDescriptor{dst, BufferState::CopyDestination, BufferState::Common}};
        cmd->ResourceBarrier(b);
    }
    cmd->End();
    Submit(cmd.get(), fence.get());
    fence->Wait();
}

void OffScreenTestContext::Submit(CommandBuffer* cmd, Fence* fence) {
    CommandBuffer* submitBuffers[] = {cmd};
    CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitBuffers;
    submitDesc.SignalFence = fence;
    _queue->Submit(submitDesc);
}

bool OffScreenTestContext::HasCapturedRenderErrors() const {
    std::lock_guard<std::mutex> lock(_capturedRenderErrorsMutex);
    return !_capturedRenderErrors.empty();
}

vector<string> OffScreenTestContext::GetCapturedRenderErrors() const {
    std::lock_guard<std::mutex> lock(_capturedRenderErrorsMutex);
    return _capturedRenderErrors;
}

void OffScreenTestContext::ClearCapturedRenderErrors() {
    std::lock_guard<std::mutex> lock(_capturedRenderErrorsMutex);
    _capturedRenderErrors.clear();
}

void OffScreenTestContext::DeviceLogBridge(LogLevel level, std::string_view message, void* userData) {
    auto* self = static_cast<OffScreenTestContext*>(userData);
    if (self == nullptr) {
        return;
    }
    self->OnDeviceLog(level, message);
}

void OffScreenTestContext::OnDeviceLog(LogLevel level, std::string_view message) {
    if (level == LogLevel::Err || level == LogLevel::Critical) {
        std::lock_guard<std::mutex> lock(_capturedRenderErrorsMutex);
        _capturedRenderErrors.emplace_back(message);
    }
    if (_prevLogCallback != nullptr) {
        _prevLogCallback(level, message, _prevLogUserData);
    }
}

}  // namespace radray::render
