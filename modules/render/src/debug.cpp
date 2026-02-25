#include <radray/render/debug.h>

#include <filesystem>
#include <cstring>
#include <limits>
#include <string>

#include <radray/logger.h>
#include <radray/utility.h>
#ifdef RADRAY_ENABLE_DXC
#include <radray/render/dxc.h>
#endif

namespace radray::render {

namespace {

[[noreturn]] void ThrowDebug(const std::string& where, const std::string& msg) {
    throw DebugException(where + ": " + msg);
}

}  // namespace

std::optional<TextureReadbackResult> ReadbackTexture2D(
    Device* device,
    CommandQueue* queue,
    Texture* src,
    TextureState srcStateBeforeCopy,
    uint32_t mipLevel,
    uint32_t arrayLayer) noexcept {
    if (device == nullptr || queue == nullptr || src == nullptr) {
        RADRAY_ERR_LOG("ReadbackTexture2D got null input");
        return std::nullopt;
    }

    const TextureDescriptor srcDesc = src->GetDesc();
    if (srcDesc.Dim != TextureDimension::Dim2D && srcDesc.Dim != TextureDimension::Dim2DArray) {
        RADRAY_ERR_LOG("ReadbackTexture2D only supports 2D textures, got {}", srcDesc.Dim);
        return std::nullopt;
    }
    if (srcDesc.MipLevels == 0 || mipLevel >= srcDesc.MipLevels) {
        RADRAY_ERR_LOG("ReadbackTexture2D invalid mip level {}, total {}", mipLevel, srcDesc.MipLevels);
        return std::nullopt;
    }
    if (srcDesc.DepthOrArraySize == 0 || arrayLayer >= srcDesc.DepthOrArraySize) {
        RADRAY_ERR_LOG("ReadbackTexture2D invalid array layer {}, total {}", arrayLayer, srcDesc.DepthOrArraySize);
        return std::nullopt;
    }

    const uint32_t bpp = GetTextureFormatBytesPerPixel(srcDesc.Format);
    if (bpp == 0) {
        RADRAY_ERR_LOG("ReadbackTexture2D invalid texture format {}", srcDesc.Format);
        return std::nullopt;
    }

    const uint32_t width = std::max(srcDesc.Width >> mipLevel, 1u);
    const uint32_t height = std::max(srcDesc.Height >> mipLevel, 1u);
    const uint64_t tightRowBytes = static_cast<uint64_t>(width) * bpp;
    const uint64_t rowPitchAlign = std::max<uint64_t>(1, device->GetDetail().TextureDataPitchAlignment);
    const uint64_t rowPitchBytes = Align(tightRowBytes, rowPitchAlign);
    const uint64_t totalBytes = rowPitchBytes * height;
    if (rowPitchBytes > std::numeric_limits<uint32_t>::max() ||
        tightRowBytes > std::numeric_limits<uint32_t>::max() ||
        totalBytes > std::numeric_limits<uint32_t>::max()) {
        RADRAY_ERR_LOG("ReadbackTexture2D readback size overflow");
        return std::nullopt;
    }

    BufferDescriptor readbackDesc{};
    readbackDesc.Size = totalBytes;
    readbackDesc.Memory = MemoryType::ReadBack;
    readbackDesc.Usage = BufferUse::MapRead | BufferUse::CopyDestination;
    readbackDesc.Hints = ResourceHint::None;
    readbackDesc.Name = "debug_readback_buffer";
    auto readbackBufOpt = device->CreateBuffer(readbackDesc);
    if (!readbackBufOpt.HasValue()) {
        RADRAY_ERR_LOG("ReadbackTexture2D failed to create readback buffer");
        return std::nullopt;
    }
    auto readbackBuf = readbackBufOpt.Release();

    auto cmdOpt = device->CreateCommandBuffer(queue);
    if (!cmdOpt.HasValue()) {
        RADRAY_ERR_LOG("ReadbackTexture2D failed to create command buffer");
        return std::nullopt;
    }
    auto cmd = cmdOpt.Release();

    auto fenceOpt = device->CreateFence();
    if (!fenceOpt.HasValue()) {
        RADRAY_ERR_LOG("ReadbackTexture2D failed to create fence");
        return std::nullopt;
    }
    auto fence = fenceOpt.Release();

    cmd->Begin();
    {
        BarrierTextureDescriptor texBarrier{};
        texBarrier.Target = src;
        texBarrier.Before = srcStateBeforeCopy;
        texBarrier.After = TextureState::CopySource;
        texBarrier.IsSubresourceBarrier = true;
        texBarrier.Range = SubresourceRange{arrayLayer, 1, mipLevel, 1};
        const ResourceBarrierDescriptor barriers[] = {texBarrier};
        cmd->ResourceBarrier(barriers);
    }
    cmd->CopyTextureToBuffer(readbackBuf.get(), 0, src, SubresourceRange{arrayLayer, 1, mipLevel, 1});
    {
        // Readback buffers are CPU-visible and can be mapped directly after GPU execution completes.
        // Avoid forcing backend-specific state transitions here.
    }
    cmd->End();

    CommandBuffer* cmdRaw[] = {cmd.get()};
    CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = cmdRaw;
    submitDesc.SignalFence = fence.get();
    queue->Submit(submitDesc);
    fence->Wait();

    TextureReadbackResult result{};
    result.Format = srcDesc.Format;
    result.Layout.Width = width;
    result.Layout.Height = height;
    result.Layout.BytesPerPixel = bpp;
    result.Layout.RowPitchBytes = static_cast<uint32_t>(rowPitchBytes);
    result.Layout.TightRowBytes = static_cast<uint32_t>(tightRowBytes);
    result.Data.resize(totalBytes);

    void* mapped = readbackBuf->Map(0, totalBytes);
    if (mapped == nullptr) {
        RADRAY_ERR_LOG("ReadbackTexture2D map readback buffer failed");
        return std::nullopt;
    }
    std::memcpy(result.Data.data(), mapped, totalBytes);
    readbackBuf->Unmap(0, totalBytes);
    return result;
}

std::optional<ImageData> PackReadbackToTightRGBA8(const TextureReadbackResult& in) noexcept {
    if (in.Layout.Width == 0 || in.Layout.Height == 0 || in.Layout.BytesPerPixel != 4) {
        RADRAY_ERR_LOG("PackReadbackToTightRGBA8 invalid readback layout");
        return std::nullopt;
    }
    if (in.Format != TextureFormat::RGBA8_UNORM &&
        in.Format != TextureFormat::BGRA8_UNORM &&
        in.Format != TextureFormat::RGBA8_UNORM_SRGB &&
        in.Format != TextureFormat::BGRA8_UNORM_SRGB) {
        RADRAY_ERR_LOG("PackReadbackToTightRGBA8 unsupported format {}", in.Format);
        return std::nullopt;
    }

    const uint64_t expectBytes = static_cast<uint64_t>(in.Layout.RowPitchBytes) * in.Layout.Height;
    if (in.Data.size() < expectBytes) {
        RADRAY_ERR_LOG("PackReadbackToTightRGBA8 input data too small, need {}, got {}", expectBytes, in.Data.size());
        return std::nullopt;
    }

    ImageData out{};
    out.Width = in.Layout.Width;
    out.Height = in.Layout.Height;
    out.Format = ImageFormat::RGBA8_BYTE;
    out.Data = make_unique<byte[]>(static_cast<size_t>(out.GetSize()));
    const bool isBGRA = (in.Format == TextureFormat::BGRA8_UNORM || in.Format == TextureFormat::BGRA8_UNORM_SRGB);
    for (uint32_t y = 0; y < in.Layout.Height; ++y) {
        const byte* srcRow = in.Data.data() + static_cast<size_t>(in.Layout.RowPitchBytes) * y;
        byte* dstRow = out.Data.get() + static_cast<size_t>(in.Layout.TightRowBytes) * y;
        if (!isBGRA) {
            std::memcpy(dstRow, srcRow, in.Layout.TightRowBytes);
            continue;
        }
        for (uint32_t x = 0; x < in.Layout.Width; ++x) {
            const size_t p = static_cast<size_t>(x) * 4;
            dstRow[p + 0] = srcRow[p + 2];
            dstRow[p + 1] = srcRow[p + 1];
            dstRow[p + 2] = srcRow[p + 0];
            dstRow[p + 3] = srcRow[p + 3];
        }
    }
    return out;
}

DebugContext DebugContext::Create(const DebugContextDescriptor& desc) {
    auto devOpt = CreateDevice(desc.DeviceDesc);
    if (!devOpt.HasValue()) {
        ThrowDebug("DebugContext::Create", "CreateDevice failed");
    }
    auto device = devOpt.Unwrap();

    auto queueOpt = device->GetCommandQueue(desc.Queue, desc.QueueIndex);
    if (!queueOpt.HasValue()) {
        ThrowDebug("DebugContext::Create", "GetCommandQueue failed");
    }
    auto* queue = queueOpt.Unwrap();

    DebugContext ctx{device, queue};
    if (desc.CreateDxc) {
#ifdef RADRAY_ENABLE_DXC
        auto dxcOpt = CreateDxc();
        if (!dxcOpt.HasValue()) {
            ThrowDebug("DebugContext::Create", "CreateDxc failed");
        }
        ctx._dxc = dxcOpt.Unwrap();
#else
        ThrowDebug("DebugContext::Create", "CreateDxc requested but RADRAY_ENABLE_DXC is off");
#endif
    }
    return ctx;
}

DebugOffscreenTarget DebugContext::CreateOffscreenTarget(const DebugOffscreenTargetDescriptor& desc) {
    if (desc.Width == 0 || desc.Height == 0) {
        ThrowDebug("DebugContext::CreateOffscreenTarget", "invalid extent");
    }

    TextureDescriptor rtDesc{};
    rtDesc.Dim = TextureDimension::Dim2D;
    rtDesc.Width = desc.Width;
    rtDesc.Height = desc.Height;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.SampleCount = 1;
    rtDesc.Format = desc.Format;
    rtDesc.Memory = MemoryType::Device;
    rtDesc.Usage = TextureUse::RenderTarget | TextureUse::CopySource;
    rtDesc.Hints = ResourceHint::None;
    rtDesc.Name = desc.Name;
    auto rtOpt = _device->CreateTexture(rtDesc);
    if (!rtOpt.HasValue()) {
        ThrowDebug("DebugContext::CreateOffscreenTarget", "CreateTexture failed");
    }

    auto rt = rtOpt.Release();
    TextureViewDescriptor rtvDesc{};
    rtvDesc.Target = rt.get();
    rtvDesc.Dim = TextureDimension::Dim2D;
    rtvDesc.Format = desc.Format;
    rtvDesc.Range = SubresourceRange{0, 1, 0, 1};
    rtvDesc.Usage = TextureViewUsage::RenderTarget;
    auto rtvOpt = _device->CreateTextureView(rtvDesc);
    if (!rtvOpt.HasValue()) {
        ThrowDebug("DebugContext::CreateOffscreenTarget", "CreateTextureView failed");
    }

    DebugOffscreenTarget target{};
    target.Texture = std::move(rt);
    target.RTV = rtvOpt.Release();
    target.Format = desc.Format;
    target.Width = desc.Width;
    target.Height = desc.Height;
    return target;
}

void DebugContext::ExecutePass(DebugOffscreenTarget& target, DebugPass& pass, TextureState before, TextureState after) {
    if (target.Texture == nullptr || target.RTV == nullptr) {
        ThrowDebug("DebugContext::ExecutePass", "offscreen target is incomplete");
    }

    auto cmdOpt = _device->CreateCommandBuffer(_queue);
    if (!cmdOpt.HasValue()) {
        ThrowDebug("DebugContext::ExecutePass", "CreateCommandBuffer failed");
    }
    auto fenceOpt = _device->CreateFence();
    if (!fenceOpt.HasValue()) {
        ThrowDebug("DebugContext::ExecutePass", "CreateFence failed");
    }

    auto cmd = cmdOpt.Release();
    auto fence = fenceOpt.Release();
    cmd->Begin();
    {
        const ResourceBarrierDescriptor b[] = {
            BarrierTextureDescriptor{target.Texture.get(), before, TextureState::RenderTarget, nullptr, false, false, {}}};
        cmd->ResourceBarrier(b);
    }

    pass.Record(*this, cmd.get(), target);

    {
        const ResourceBarrierDescriptor b[] = {
            BarrierTextureDescriptor{target.Texture.get(), TextureState::RenderTarget, after, nullptr, false, false, {}}};
        cmd->ResourceBarrier(b);
    }
    cmd->End();

    CommandBuffer* submitBuffers[] = {cmd.get()};
    CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitBuffers;
    submitDesc.SignalFence = fence.get();
    _queue->Submit(submitDesc);
    fence->Wait();
}

ImageData DebugContext::ReadbackRGBA8(
    const DebugOffscreenTarget& target,
    TextureState srcStateBeforeCopy,
    uint32_t mipLevel,
    uint32_t arrayLayer) {
    if (target.Texture == nullptr) {
        ThrowDebug("DebugContext::ReadbackRGBA8", "target texture is null");
    }

    auto readback = ReadbackTexture2D(_device.get(), _queue, target.Texture.get(), srcStateBeforeCopy, mipLevel, arrayLayer);
    if (!readback.has_value()) {
        ThrowDebug("DebugContext::ReadbackRGBA8", "ReadbackTexture2D failed");
    }
    auto packed = PackReadbackToTightRGBA8(*readback);
    if (!packed.has_value()) {
        ThrowDebug("DebugContext::ReadbackRGBA8", "PackReadbackToTightRGBA8 failed");
    }
    return std::move(*packed);
}

void DebugContext::WritePNG(const ImageData& img, const PNGWriteSettings& settings) const {
    if (!img.WritePNG(settings)) {
        ThrowDebug("DebugContext::WritePNG", "ImageData::WritePNG failed");
    }
}

PixelCompareResult CompareImageRGBA8(const ImageData& actual, const ImageData& expected, uint8_t tolerance) {
    if (actual.Format != ImageFormat::RGBA8_BYTE || expected.Format != ImageFormat::RGBA8_BYTE) {
        ThrowDebug("CompareImageRGBA8", "only RGBA8_BYTE is supported");
    }
    if (actual.Width != expected.Width || actual.Height != expected.Height) {
        ThrowDebug("CompareImageRGBA8", "image size mismatch");
    }
    if (actual.Data == nullptr || expected.Data == nullptr) {
        ThrowDebug("CompareImageRGBA8", "image data is null");
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

ImageData BuildDiffImageRGBA8(const ImageData& actual, const ImageData& expected) {
    if (actual.Format != ImageFormat::RGBA8_BYTE || expected.Format != ImageFormat::RGBA8_BYTE) {
        ThrowDebug("BuildDiffImageRGBA8", "only RGBA8_BYTE is supported");
    }
    if (actual.Width != expected.Width || actual.Height != expected.Height) {
        ThrowDebug("BuildDiffImageRGBA8", "image size mismatch");
    }
    if (actual.Data == nullptr || expected.Data == nullptr) {
        ThrowDebug("BuildDiffImageRGBA8", "image data is null");
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

void WriteImageComparisonArtifacts(
    const DebugContext& ctx,
    const ImageData& actual,
    const ImageData& expected,
    std::string_view outputDir) {
    if (outputDir.empty()) {
        ThrowDebug("WriteImageComparisonArtifacts", "outputDir is empty");
    }

    const std::filesystem::path outPath{outputDir};
    std::error_code ec;
    std::filesystem::create_directories(outPath, ec);

    const ImageData diff = BuildDiffImageRGBA8(actual, expected);
    ctx.WritePNG(actual, {(outPath / "actual.png").string(), false});
    ctx.WritePNG(expected, {(outPath / "expected.png").string(), false});
    ctx.WritePNG(diff, {(outPath / "diff.png").string(), false});
}

}  // namespace radray::render
