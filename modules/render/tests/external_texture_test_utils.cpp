#include "external_texture_test_utils.h"

#include <bit>
#include <fstream>

namespace radray::render::test {
namespace {
uint64_t Align(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }
struct CopyLayout {
    SubresourceRange Range;
    uint32_t Width, Height, Depth, Bytes;
    uint64_t Offset, Pitch, Size;
};
uint32_t Channels(TextureFormat format) {
    const auto name = format_as(format);
    if (name.starts_with("RGBA") || name.starts_with("BGRA") || name.starts_with("RGB10")) return 4;
    if (format == TextureFormat::RG11B10_FLOAT) return 3;
    return name.starts_with("RG") ? 2 : 1;
}
array<float, 4> Values(const TextureDescriptor& d, ExternalTextureTestUse use, uint32_t seed,
                       const CopyLayout& sub, uint32_t x, uint32_t y, uint32_t z, uint32_t sample = 0) {
    const uint32_t n = seed * 13 + sub.Range.BaseMipLevel * 7 + sub.Range.BaseArrayLayer * 11 + z * 5 + y * 3 + x + sample * 17;
    array<float, 4> result{0, 0, 0, 1};
    if (IsDepthStencilFormat(d.Format)) {
        const auto base = seed * 13 + sub.Range.BaseMipLevel * 7 + sub.Range.BaseArrayLayer * 11;
        result[0] = sub.Range.Aspects == TextureAspect::Stencil ? float(17 + base % 101) : float(1 + base % 7) / 8.0f;
        if (use != ExternalTextureTestUse::RenderTarget) {
            result[0] = sub.Range.Aspects == TextureAspect::Stencil ? float(17 + n % 101) : float(1 + n % 7) / 8.0f;
        }
        if (use == ExternalTextureTestUse::RenderTarget) {
            const bool inside = x < std::max(1u, sub.Width / 2);
            if (sub.Range.Aspects == TextureAspect::Stencil)
                result[0] += inside ? 1.0f : 0.0f;
            else if (!inside)
                result[0] = float(1 + (base + 13) % 7) / 8.0f;
        }
        return result;
    }
    for (uint32_t c = 0; c < Channels(d.Format); ++c) result[c] = float((n >> c) & 1);
    return result;
}
array<float, 4> ReinterpretedColor(array<float, 4> value, bool sampled) {
    for (uint32_t c = 0; c < 3; ++c) {
        value[c] = (value[c] * 128.0f + 64.0f) / 255.0f;
        if (sampled) value[c] = value[c] <= 0.04045f ? value[c] / 12.92f : std::pow((value[c] + 0.055f) / 1.055f, 2.4f);
    }
    return value;
}
void Encode(TextureFormat format, TextureAspects aspect, const array<float, 4>& value, byte* output) {
    if (aspect == TextureAspect::Stencil) {
        *output = byte(uint8_t(value[0]));
        return;
    }
    if (IsDepthStencilFormat(format)) {
        if (format == TextureFormat::D16_UNORM) {
            const uint16_t v = uint16_t(std::lround(value[0] * 65535.0f));
            std::memcpy(output, &v, 2);
        } else if (format == TextureFormat::D24_UNORM_S8_UINT) {
            const uint32_t v = uint32_t(std::lround(double(value[0]) * 16777215.0));
            std::memcpy(output, &v, 4);
        } else
            std::memcpy(output, value.data(), 4);
        return;
    }
    const auto name = format_as(format);
    const auto count = Channels(format);
    if (format == TextureFormat::RGB10A2_UINT || format == TextureFormat::RGB10A2_UNORM || format == TextureFormat::RG11B10_FLOAT) {
        uint32_t packed = 0, shift = 0;
        for (uint32_t c = 0; c < count; ++c) {
            const uint32_t bits = format == TextureFormat::RG11B10_FLOAT ? (c == 2 ? 10 : 11) : c == 3 ? 2
                                                                                                       : 10;
            const uint32_t one = format == TextureFormat::RGB10A2_UINT ? 1 : format == TextureFormat::RG11B10_FLOAT ? 15u << (bits - 5)
                                                                                                                    : (1u << bits) - 1;
            packed |= (value[c] != 0 ? one : 0) << shift;
            shift += bits;
        }
        std::memcpy(output, &packed, 4);
        return;
    }
    const uint32_t bytes = GetTextureFormatBytesPerPixel(format) / count;
    for (uint32_t c = 0; c < count; ++c) {
        const auto component = name.starts_with("BGRA") && c < 3 ? 2 - c : c;
        uint32_t bits = 0;
        if (value[component] != 0) {
            if (name.ends_with("FLOAT"))
                bits = bytes == 2 ? 0x3c00 : 0x3f800000;
            else if (name.ends_with("SNORM"))
                bits = (1u << (bytes * 8 - 1)) - 1;
            else if (IsUintFormat(format) || IsSintFormat(format))
                bits = 1;
            else
                bits = uint32_t(std::lround(value[component] * float((1u << (bytes * 8)) - 1)));
        }
        std::memcpy(output + c * bytes, &bits, bytes);
    }
}
vector<CopyLayout> Layouts(DeviceContext& context, const TextureDescriptor& d) {
    vector<CopyLayout> result;
    uint64_t end = 0;
    const bool is3D = d.Dim == TextureDimension::Dim3D;
    for (auto aspect : {TextureAspect::Color, TextureAspect::Depth, TextureAspect::Stencil}) {
        if (!GetTextureFormatAspects(d.Format).HasFlag(aspect)) continue;
        for (uint32_t mip = 0; mip < d.MipLevels; ++mip) {
            for (uint32_t layer = 0; layer < (is3D ? 1 : d.DepthOrArraySize); ++layer) {
                CopyLayout sub{{layer, 1, mip, 1, aspect}, std::max(1u, d.Width >> mip), std::max(1u, d.Height >> mip), is3D ? std::max(1u, d.DepthOrArraySize >> mip) : 1u, 0, 0, 0, 0};
                const auto detail = context.Device->GetDetail();
                sub.Bytes = aspect == TextureAspect::Stencil ? 1 : IsDepthStencilFormat(d.Format) ? (d.Format == TextureFormat::D16_UNORM ? 2 : 4)
                                                                                                  : GetTextureFormatBytesPerPixel(d.Format);
                sub.Offset = Align(end, std::max<uint64_t>(512, detail.TextureDataPlacementAlignment));
                sub.Pitch = Align(uint64_t(sub.Width) * sub.Bytes, std::max<uint64_t>(IsDepthStencilFormat(d.Format) ? 4 : 1, detail.TextureDataPitchAlignment));
#if defined(RADRAY_ENABLE_D3D12)
                if (context.Device->GetBackend() == RenderBackend::D3D12 && d.SampleCount == 1) {
                    const auto native = d3d12::MakeTextureResourceDesc(d, false);
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                    uint64_t bytes{}, row{};
                    static_cast<d3d12::DeviceD3D12*>(context.NativeDevice.Get())->_device->GetCopyableFootprints(&native, (mip + layer * d.MipLevels + (aspect == TextureAspect::Stencil ? 1 : 0) * d.MipLevels * (is3D ? 1 : d.DepthOrArraySize)), 1, sub.Offset, &footprint, nullptr, &row, &bytes);
                    sub.Offset = footprint.Offset;
                    sub.Pitch = footprint.Footprint.RowPitch;
                    sub.Bytes = uint32_t(row / sub.Width);
                }
#endif
                sub.Size = sub.Pitch * sub.Height * sub.Depth;
                end = sub.Offset + sub.Size;
                result.push_back(sub);
            }
        }
    }
    return result;
}
unique_ptr<Shader> Stage(Device& device, std::string_view name, ShaderStage stage) {
    const auto path = fmt::format("{}/shader_bytecode/{}.{}.{}.bin", RADRAY_TEST_SHADER_DIR, name, stage == ShaderStage::Compute ? "CSMain" : stage == ShaderStage::Vertex ? "VSMain"
                                                                                                                                                                                                 : "PSMain",
                                  device.GetBackend() == RenderBackend::D3D12 ? "dxil" : "spirv");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0) return {};
    vector<byte> data(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    return device.CreateShader({data, device.GetBackend() == RenderBackend::D3D12 ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV, stage}).Unwrap();
}
BindingDescriptor Binding(uint32_t id, BindingKind kind) {
    return {id, kind, 1, ShaderStage::Compute, D3D12BindingDescriptor{GetBindingRegisterClass(kind), 0, 0}, VulkanBindingDescriptor{0, id}};
}
void Barrier(CommandBuffer& command, Texture* texture, TextureStates before, TextureStates after, SubresourceRange range = SubresourceRange::AllSub(), ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{.Target = texture, .Before = before, .After = after, .Range = range, .ExternalTransfer = transfer};
    command.ResourceBarrier({&barrier, 1});
}
void BufferBarrier(CommandBuffer& command, Buffer* buffer, BufferStates before, BufferStates after, ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{.Target = buffer, .Before = before, .After = after, .ExternalTransfer = transfer};
    command.ResourceBarrier({&barrier, 1});
}
struct ShaderWork {
    unique_ptr<Shader> Shader, Vertex;
    unique_ptr<BindingGroupLayout> Group;
    unique_ptr<PipelineLayout> Layout;
    unique_ptr<ComputePipelineState> Compute;
    unique_ptr<GraphicsPipelineState> Graphics;
    unique_ptr<TextureView> View;
    unique_ptr<Sampler> Sampler;
    unique_ptr<Buffer> Output, Readback;
    unique_ptr<ShaderParameterSet> Parameters;
    CopyLayout Sub;
};
uint32_t Dimension(const TextureDescriptor& d, bool read) {
    if (d.SampleCount > 1) return 6;
    if (d.Dim == TextureDimension::Dim1D || d.Dim == TextureDimension::Dim1DArray) return 1;
    if (d.Dim == TextureDimension::Dim3D) return 3;
    if (read && d.Dim == TextureDimension::Cube) return 4;
    if (read && d.Dim == TextureDimension::CubeArray) return 5;
    return 2;
}
unique_ptr<ShaderWork> ComputeWork(Device& device, Texture* texture, const CopyLayout& sub, bool read, bool reinterpretViews) {
    auto work = make_unique<ShaderWork>();
    work->Sub = sub;
    const auto d = texture->GetDesc();
    const auto dim = Dimension(d, read);
    const auto type = IsUintFormat(d.Format) || sub.Range.Aspects == TextureAspect::Stencil ? "uint" : IsSintFormat(d.Format) ? "int"
                                                                                                                              : "float";
    work->Shader = Stage(device, fmt::format("interop_texture_{}_{}_{}", read ? "read" : "write", dim, type), ShaderStage::Compute);
    if (!work->Shader) return {};
    vector<BindingDescriptor> bindings{Binding(0, read ? BindingKind::Texture : BindingKind::RWTexture)};
    if (read) bindings.push_back(Binding(1, BindingKind::RWStructuredBuffer));
    if (dim == 4 || dim == 5) bindings.push_back(Binding(2, BindingKind::Sampler));
    work->Group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor group{0, work->Group.get()};
    const PushConstantDescriptor push{0, 32, ShaderStage::Compute, PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
    work->Layout = device.CreatePipelineLayout({{&group, 1}, {&push, 1}}).Unwrap();
    work->Compute = device.CreateComputePipelineState({work->Layout.get(), {work->Shader.get(), "CSMain"}}).Unwrap();
    const auto viewDim = dim == 1 ? TextureDimension::Dim1DArray : dim == 3 ? TextureDimension::Dim3D
                                                               : dim == 4   ? TextureDimension::Cube
                                                               : dim == 5   ? TextureDimension::CubeArray
                                                                            : TextureDimension::Dim2DArray;
    auto range = sub.Range;
    if (dim == 4 || dim == 5) {
        range.BaseArrayLayer = 0;
        range.ArrayLayerCount = d.DepthOrArraySize;
    }
    auto viewFormat = d.Format;
    if (read && reinterpretViews) viewFormat = d.Format == TextureFormat::RGBA8_UNORM ? TextureFormat::RGBA8_UNORM_SRGB : d.Format == TextureFormat::BGRA8_UNORM ? TextureFormat::BGRA8_UNORM_SRGB
                                                                                                                                                                 : d.Format;
    work->View = device.CreateTextureView({texture, viewDim, viewFormat, range, read ? TextureViewUsage::Resource : TextureViewUsage::UnorderedAccess}).Unwrap();
    work->Parameters = device.CreateShaderParameterSet({work->Group.get()}).Unwrap();
    if (!work->Parameters->Set(0, 0, work->View.get())) return {};
    if (read) {
        const uint64_t size = uint64_t(sub.Width) * sub.Height * sub.Depth * d.SampleCount * 16;
        work->Output = device.CreateBuffer({.Size = size, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
        work->Readback = device.CreateBuffer({.Size = size, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
        if (!work->Parameters->Set(1, 0, ShaderBufferBinding{work->Output.get(), {0, size}, 16})) return {};
    }
    if (dim == 4 || dim == 5) {
        work->Sampler = device.CreateSampler({.AddressS = AddressMode::ClampToEdge, .AddressT = AddressMode::ClampToEdge, .AddressR = AddressMode::ClampToEdge, .MinFilter = FilterMode::Nearest, .MagFilter = FilterMode::Nearest, .MipmapFilter = FilterMode::Nearest}).Unwrap();
        if (!work->Parameters->Set(2, 0, work->Sampler.get())) return {};
    }
    if (!work->Parameters->FlushWrites()) return {};
    return work;
}
void Dispatch(CommandBuffer& command, ShaderWork& work, const TextureDescriptor& d, uint32_t seed, bool read, bool reinterpretViews = false) {
    const auto& s = work.Sub;
    const uint32_t push[]{seed, s.Width, s.Height, s.Depth, s.Range.BaseMipLevel, s.Range.BaseArrayLayer, d.SampleCount, reinterpretViews ? 1u : 0u};
    if (read) BufferBarrier(command, work.Output.get(), BufferState::Undefined, BufferState::UnorderedAccess);
    auto pass = command.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(work.Compute.get());
    pass->BindShaderParameterSet(0, work.Parameters.get());
    EXPECT_TRUE(pass->SetPushConstants(0, 0, std::as_bytes(std::span{push})));
    pass->Dispatch((s.Width + 7) / 8, (s.Height + 7) / 8, s.Depth);
    command.EndComputePass(std::move(pass));
    if (read) {
        BufferBarrier(command, work.Output.get(), BufferState::UnorderedAccess, BufferState::CopySource);
        command.CopyBufferToBuffer(work.Readback.get(), 0, work.Output.get(), 0, work.Output->GetDesc().Size);
    }
}
}  // namespace

void ExpectTextureDescriptor(const TextureDescriptor& actual, const TextureDescriptor& expected) {
    EXPECT_EQ(actual.Dim, expected.Dim);
    EXPECT_EQ(actual.Width, expected.Width);
    EXPECT_EQ(actual.Height, expected.Height);
    EXPECT_EQ(actual.DepthOrArraySize, expected.DepthOrArraySize);
    EXPECT_EQ(actual.MipLevels, expected.MipLevels);
    EXPECT_EQ(actual.SampleCount, expected.SampleCount);
    EXPECT_EQ(actual.Format, expected.Format);
    EXPECT_EQ(actual.Memory, expected.Memory);
    EXPECT_EQ(actual.Usage, expected.Usage);
    EXPECT_EQ(actual.Hints, expected.Hints | ResourceHint::Dedicated);
}

vector<ExternalTextureCase> ExternalTextureCases() {
    vector<ExternalTextureCase> cases;
    const auto add = [&](std::string_view name, TextureDescriptor desc, ExternalTextureTestUse use, bool reinterpret = false) {
        if (desc.SampleCount == 1) desc.Usage |= TextureUse::CopySource | TextureUse::CopyDestination;
        if (use == ExternalTextureTestUse::Sampled || desc.SampleCount > 1) desc.Usage |= TextureUse::Resource;
        if (use == ExternalTextureTestUse::Storage) desc.Usage |= TextureUse::UnorderedAccess;
        if (use == ExternalTextureTestUse::RenderTarget) desc.Usage |= IsDepthStencilFormat(desc.Format) ? TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite : TextureUses{TextureUse::RenderTarget};
        cases.push_back({string(name), desc, reinterpret, use});
    };
    for (auto dim : {TextureDimension::Dim1D, TextureDimension::Dim2D, TextureDimension::Dim3D, TextureDimension::Dim1DArray, TextureDimension::Dim2DArray, TextureDimension::Cube, TextureDimension::CubeArray}) {
        for (auto format : {TextureFormat::RGBA8_UNORM, TextureFormat::RGBA16_FLOAT}) {
            for (uint32_t mips : {1u, 3u, 5u}) {
                const bool one = dim == TextureDimension::Dim1D || dim == TextureDimension::Dim1DArray;
                const bool cube = dim == TextureDimension::Cube || dim == TextureDimension::CubeArray;
                const uint32_t layers = dim == TextureDimension::Dim3D ? 7 : dim == TextureDimension::Cube                                            ? 6
                                                                         : dim == TextureDimension::CubeArray                                         ? 12
                                                                         : dim == TextureDimension::Dim1DArray || dim == TextureDimension::Dim2DArray ? 3
                                                                                                                                                      : 1;
                TextureDescriptor d{dim, 17, one ? 1u : cube ? 17u
                                                             : 9u,
                                    layers, mips, 1, format};
                const auto use = mips == 1 ? ExternalTextureTestUse::Copy : mips == 3 ? ExternalTextureTestUse::Storage
                                                                                      : ExternalTextureTestUse::Sampled;
                add(fmt::format("Shape_{}_{}_M{}_U{}", dim, format, mips, uint32_t(use)), d, use);
            }
        }
    }
    for (int f = int(TextureFormat::R8_SINT); f <= int(TextureFormat::RGBA32_FLOAT); ++f) {
        const auto format = TextureFormat(f);
        for (auto use : {ExternalTextureTestUse::Copy, ExternalTextureTestUse::Sampled, ExternalTextureTestUse::Storage, ExternalTextureTestUse::RenderTarget})
            add(fmt::format("Format_{}_U{}", format, uint32_t(use)), {TextureDimension::Dim2DArray, 9, 5, 2, 2, 1, format}, use);
    }
    for (auto format : {TextureFormat::D16_UNORM, TextureFormat::D32_FLOAT, TextureFormat::D24_UNORM_S8_UINT, TextureFormat::D32_FLOAT_S8_UINT}) {
        add(fmt::format("DepthCopyOnly_{}", format), {TextureDimension::Dim2D, 9, 5, 1, 3, 1, format}, ExternalTextureTestUse::Copy);
        for (auto use : {ExternalTextureTestUse::Copy, ExternalTextureTestUse::Sampled, ExternalTextureTestUse::RenderTarget})
            add(fmt::format("Depth_{}_U{}", format, uint32_t(use)), {TextureDimension::Dim2DArray, 9, 5, 2, 3, 1, format, MemoryType::Device, TextureUse::DepthStencilWrite}, use, use == ExternalTextureTestUse::Sampled);
    }
    for (auto format : {TextureFormat::RGBA8_UNORM, TextureFormat::RGBA16_FLOAT, TextureFormat::R32_UINT, TextureFormat::D32_FLOAT, TextureFormat::D24_UNORM_S8_UINT})
        for (uint32_t samples : {2u, 4u, 8u, 16u})
            for (auto dim : {TextureDimension::Dim2D, TextureDimension::Dim2DArray})
                add(fmt::format("Msaa_{}_{}_S{}", dim, format, samples), {dim, 8, 8, dim == TextureDimension::Dim2D ? 1u : 2u, 1, samples, format}, ExternalTextureTestUse::RenderTarget, IsDepthStencilFormat(format));
    for (auto format : {TextureFormat::RGBA8_UNORM, TextureFormat::BGRA8_UNORM})
        add(fmt::format("Reinterpret_{}", format), {TextureDimension::Dim2DArray, 9, 5, 2, 3, 1, format, MemoryType::Device, TextureUse::Resource | TextureUse::RenderTarget | TextureUse::UnorderedAccess}, ExternalTextureTestUse::Storage, true);
    return cases;
}

bool ExternalTextureTurn(DeviceContext& context, Texture* texture, ExternalTextureTestUse use,
                         uint32_t expected, uint32_t written, bool first,
                         Fence* wait, uint64_t waitValue, Fence* signal, uint64_t signalValue, Nullable<Buffer*> sharedBuffer, bool reinterpretViews) {
    auto& device = *context.Device;
    const auto d = texture->GetDesc();
    const auto subs = Layouts(context, d);
    const uint64_t size = subs.back().Offset + subs.back().Size;
    unique_ptr<Buffer> upload, readback;
    if (d.SampleCount == 1) {
        upload = device.CreateBuffer({.Size = size, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CopySource}).Unwrap();
        readback = device.CreateBuffer({.Size = size, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
        auto* bytes = static_cast<byte*>(upload->Map(0, size));
        if (!bytes) return false;
        std::memset(bytes, 0, size_t(size));
        for (const auto& sub : subs)
            for (uint32_t z = 0; z < sub.Depth; ++z)
                for (uint32_t y = 0; y < sub.Height; ++y)
                    for (uint32_t x = 0; x < sub.Width; ++x)
                        Encode(d.Format, sub.Range.Aspects, Values(d, use, written + (use == ExternalTextureTestUse::Storage || use == ExternalTextureTestUse::RenderTarget ? 1 : 0), sub, x, y, z), bytes + sub.Offset + (z * sub.Height + y) * sub.Pitch + x * sub.Bytes);
        upload->FlushMappedRange({0, size});
        upload->Unmap();
    }
    unique_ptr<Buffer> bufferUpload, bufferReadback;
    const uint64_t bufferSize = sharedBuffer ? sharedBuffer->GetDesc().Size : 0;
    if (sharedBuffer) {
        bufferUpload = device.CreateBuffer({.Size = bufferSize, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CopySource}).Unwrap();
        bufferReadback = device.CreateBuffer({.Size = bufferSize, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
        auto* data = static_cast<byte*>(bufferUpload->Map(0, bufferSize));
        if (!data) return false;
        for (uint64_t i = 0; i < bufferSize; ++i) data[i] = byte((written * 31 + i * 17 + i / 251) & 255);
        bufferUpload->FlushMappedRange({0, bufferSize});
        bufferUpload->Unmap();
    }
    vector<unique_ptr<ShaderWork>> reads, writes;
    auto allocator = device.CreateCommandAllocator(context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    GpuWorkGuard guard{context};
    command->Begin();
    if (sharedBuffer) {
        BufferBarrier(*command, sharedBuffer.Get(), first ? BufferState::Undefined : BufferState::Common, BufferState::CopySource, first ? ExternalOwnershipTransfer::None : ExternalOwnershipTransfer::Acquire);
        if (!first) command->CopyBufferToBuffer(bufferReadback.get(), 0, sharedBuffer.Get(), 0, bufferSize);
        BufferBarrier(*command, sharedBuffer.Get(), BufferState::CopySource, BufferState::CopyDestination);
        command->CopyBufferToBuffer(sharedBuffer.Get(), 0, bufferUpload.get(), 0, bufferSize);
        BufferBarrier(*command, sharedBuffer.Get(), BufferState::CopyDestination, BufferState::Common, ExternalOwnershipTransfer::Release);
    }
    Barrier(*command, texture, first ? TextureStates{TextureState::Undefined} : TextureStates{TextureState::Common}, TextureState::Common, SubresourceRange::AllSub(), first ? ExternalOwnershipTransfer::None : ExternalOwnershipTransfer::Acquire);
    if (!first) {
        if (d.SampleCount == 1) {
            Barrier(*command, texture, TextureState::Common, TextureState::CopySource);
            for (const auto& sub : subs) command->CopyTextureToBuffer(readback.get(), sub.Offset, texture, sub.Range);
            Barrier(*command, texture, TextureState::CopySource, TextureState::Common);
        }
        if (d.Usage.HasFlag(TextureUse::Resource)) {
            Barrier(*command, texture, TextureState::Common, TextureState::ShaderRead);
            for (const auto& sub : subs) {
                auto work = ComputeWork(device, texture, sub, true, reinterpretViews);
                if (!work) return false;
                Dispatch(*command, *work, d, expected, true);
                reads.push_back(std::move(work));
            }
            Barrier(*command, texture, TextureState::ShaderRead, TextureState::Common);
        }
    }
    if (d.SampleCount == 1) {
        Barrier(*command, texture, TextureState::Common, TextureState::CopyDestination);
        for (const auto& sub : subs) command->CopyBufferToTexture(texture, sub.Range, upload.get(), sub.Offset);
        Barrier(*command, texture, TextureState::CopyDestination, TextureState::Common);
    }
    if (use == ExternalTextureTestUse::Storage) {
        for (const auto& sub : subs) {
            Barrier(*command, texture, TextureState::Common, TextureState::UnorderedAccess, sub.Range);
            auto work = ComputeWork(device, texture, sub, false, false);
            if (!work) return false;
            Dispatch(*command, *work, d, written, false, reinterpretViews);
            if ((sub.Range.BaseMipLevel + sub.Range.BaseArrayLayer) & 1)
                Barrier(*command, texture, TextureState::UnorderedAccess, TextureState::CopySource, sub.Range);
            writes.push_back(std::move(work));
        }
        for (const auto& sub : subs)
            Barrier(*command, texture, (sub.Range.BaseMipLevel + sub.Range.BaseArrayLayer) & 1 ? TextureState::CopySource : TextureState::UnorderedAccess, TextureState::Common, sub.Range);
    } else if (use == ExternalTextureTestUse::RenderTarget) {
        const bool depth = IsDepthStencilFormat(d.Format);
        Barrier(*command, texture, TextureState::Common, depth ? TextureState::DepthWrite : TextureState::RenderTarget);
        for (const auto& sub : subs) {
            if (sub.Range.Aspects == TextureAspect::Stencil) continue;
            auto work = make_unique<ShaderWork>();
            auto range = sub.Range;
            range.Aspects = TextureAspect::Auto;
            const auto dim = Dimension(d, false) == 1 ? TextureDimension::Dim1DArray : TextureDimension::Dim2DArray;
            work->View = device.CreateTextureView({texture, dim, d.Format, range, depth ? TextureViewUsage::DepthWrite : TextureViewUsage::RenderTarget}).Unwrap();
            if (depth) {
                auto stencil = sub;
                stencil.Range.Aspects = TextureAspect::Stencil;
                const RenderPassDepthStencilAttachment attachment{work->View.get(), LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store, {Values(d, use, written + 1, sub, 0, 0, 0)[0], uint8_t(Values(d, use, written, stencil, 0, 0, 0)[0] - 1)}};
                auto pass = command->BeginRenderPass({.DepthStencil = attachment, .Width = sub.Width, .Height = sub.Height}).Unwrap();
                command->EndRenderPass(std::move(pass));
                const bool hasStencil = GetTextureFormatAspects(d.Format).HasFlag(TextureAspect::Stencil);
                for (uint32_t phase = 0; phase < (hasStencil ? 2u : 1u); ++phase) {
                    const bool writeDepth = phase == 0;
                    auto draw = make_unique<ShaderWork>();
                    draw->Vertex = Stage(device, "interop_texture_depth", ShaderStage::Vertex);
                    draw->Shader = Stage(device, "interop_texture_depth", ShaderStage::Pixel);
                    if (!draw->Vertex || !draw->Shader) return false;
                    const auto state = !hasStencil ? TextureState::DepthWrite : writeDepth ? TextureState::DepthWriteStencilRead
                                                                                           : TextureState::DepthReadStencilWrite;
                    const auto usage = !hasStencil ? TextureViewUsage::DepthWrite : writeDepth ? TextureViewUsage::DepthWriteStencilRead
                                                                                               : TextureViewUsage::DepthReadStencilWrite;
                    draw->View = device.CreateTextureView({texture, dim, d.Format, range, usage}).Unwrap();
                    const PushConstantDescriptor push{0, 32, ShaderStage::Pixel, PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
                    draw->Layout = device.CreatePipelineLayout({{}, {&push, 1}}).Unwrap();
                    auto primitive = PrimitiveState::Default();
                    primitive.Cull = CullMode::None;
                    auto ds = DepthStencilState::Default();
                    ds.Format = d.Format;
                    ds.DepthTestEnable = true;
                    ds.DepthCompare = CompareFunction::Always;
                    ds.DepthWriteEnable = writeDepth;
                    if (hasStencil) {
                        ds.Stencil = StencilState::Default();
                        if (!writeDepth) ds.Stencil->Front.PassOp = ds.Stencil->Back.PassOp = StencilOperation::IncrementClamp;
                    }
                    draw->Graphics = device.CreateGraphicsPipelineState({.PipelineLayout = draw->Layout.get(), .VS = ShaderEntry{draw->Vertex.get(), "VSMain"}, .PS = ShaderEntry{draw->Shader.get(), "PSMain"}, .Primitive = primitive, .DepthStencil = ds, .MultiSample = {d.SampleCount, std::numeric_limits<uint64_t>::max(), false}}).Unwrap();
                    Barrier(*command, texture, TextureState::DepthWrite, state, range);
                    const RenderPassDepthStencilAttachment loaded{draw->View.get(), LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store};
                    pass = command->BeginRenderPass({.DepthStencil = loaded, .Width = sub.Width, .Height = sub.Height}).Unwrap();
                    pass->BindGraphicsPipelineState(draw->Graphics.get());
                    pass->SetViewport({0, 0, float(sub.Width), float(sub.Height), 0, 1});
                    pass->SetScissor({0, 0, std::max(1u, sub.Width / 2), sub.Height});
                    const uint32_t values[]{written, sub.Width, sub.Height, sub.Depth, sub.Range.BaseMipLevel, sub.Range.BaseArrayLayer, d.SampleCount, 0};
                    if (!pass->SetPushConstants(0, 0, std::as_bytes(std::span{values}))) return false;
                    pass->Draw(3, 1, 0, 0);
                    command->EndRenderPass(std::move(pass));
                    Barrier(*command, texture, state, TextureState::DepthWrite, range);
                    writes.push_back(std::move(draw));
                }
            } else {
                const auto type = IsUintFormat(d.Format) ? "uint" : IsSintFormat(d.Format) ? "int"
                                                                                           : "float";
                const auto name = fmt::format("interop_texture_color_{}", type);
                work->Vertex = Stage(device, name, ShaderStage::Vertex);
                work->Shader = Stage(device, name, ShaderStage::Pixel);
                if (!work->Vertex || !work->Shader) return false;
                const PushConstantDescriptor push{0, 32, ShaderStage::Pixel, PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
                work->Layout = device.CreatePipelineLayout({{}, {&push, 1}}).Unwrap();
                const auto target = ColorTargetState::Default(d.Format);
                auto primitive = PrimitiveState::Default();
                primitive.Cull = CullMode::None;
                work->Graphics = device.CreateGraphicsPipelineState({.PipelineLayout = work->Layout.get(), .VS = ShaderEntry{work->Vertex.get(), "VSMain"}, .PS = ShaderEntry{work->Shader.get(), "PSMain"}, .Primitive = primitive, .MultiSample = {d.SampleCount, std::numeric_limits<uint64_t>::max(), false}, .ColorTargets = {&target, 1}}).Unwrap();
                const RenderPassColorAttachment attachment{work->View.get(), LoadAction::Clear, StoreAction::Store};
                auto pass = command->BeginRenderPass({.Colors = {&attachment, 1}, .Width = sub.Width, .Height = sub.Height}).Unwrap();
                pass->BindGraphicsPipelineState(work->Graphics.get());
                pass->SetViewport({0, 0, float(sub.Width), float(sub.Height), 0, 1});
                pass->SetScissor({0, 0, sub.Width, sub.Height});
                const uint32_t values[]{written, sub.Width, sub.Height, sub.Depth, sub.Range.BaseMipLevel, sub.Range.BaseArrayLayer, d.SampleCount, 0};
                if (!pass->SetPushConstants(0, 0, std::as_bytes(std::span{values}))) return false;
                pass->Draw(3, 1, 0, 0);
                command->EndRenderPass(std::move(pass));
            }
            writes.push_back(std::move(work));
        }
        Barrier(*command, texture, depth ? TextureState::DepthWrite : TextureState::RenderTarget, TextureState::Common);
    }
    Barrier(*command, texture, TextureState::Common, TextureState::Common, SubresourceRange::AllSub(), ExternalOwnershipTransfer::Release);
    command->End();
    CommandBuffer* commands[]{command.get()};
    Fence* waits[]{wait};
    Fence* signals[]{signal};
    context.Queue->Submit({.CmdBuffers = commands, .SignalFences = signals, .SignalValues = {&signalValue, 1}, .WaitFences = waitValue ? std::span{waits} : std::span<Fence*>{}, .WaitValues = waitValue ? std::span{&waitValue, 1} : std::span<uint64_t>{}});
    context.Queue->Wait();
    bool valid = true;
    if (sharedBuffer && !first) {
        const auto* data = static_cast<const byte*>(bufferReadback->Map(0, bufferSize));
        if (!data) return false;
        bufferReadback->InvalidateMappedRange({0, bufferSize});
        for (uint64_t i = 0; i < bufferSize; ++i)
            if (data[i] != byte((expected * 31 + i * 17 + i / 251) & 255)) {
                valid = false;
                break;
            }
        bufferReadback->Unmap();
        EXPECT_TRUE(valid) << "shared buffer contents";
    }
    if (!first && readback) {
        const auto* actual = static_cast<const byte*>(readback->Map(0, size));
        if (!actual) return false;
        readback->InvalidateMappedRange({0, size});
        array<byte, 16> pixel{};
        for (const auto& sub : subs) {
            for (uint32_t z = 0; valid && z < sub.Depth; ++z)
                for (uint32_t y = 0; valid && y < sub.Height; ++y)
                    for (uint32_t x = 0; valid && x < sub.Width; ++x) {
                        auto value = Values(d, use, expected, sub, x, y, z);
                        if (reinterpretViews && !IsDepthStencilFormat(d.Format)) value = ReinterpretedColor(value, false);
                        Encode(d.Format, sub.Range.Aspects, value, pixel.data());
                        const auto* at = actual + sub.Offset + (z * sub.Height + y) * sub.Pitch + x * sub.Bytes;
                        const auto bytes = sub.Range.Aspects == TextureAspect::Stencil ? 1u : d.Format == TextureFormat::D24_UNORM_S8_UINT ? 3u
                                                                                          : IsDepthStencilFormat(d.Format)                 ? (d.Format == TextureFormat::D16_UNORM ? 2u : 4u)
                                                                                                                                           : sub.Bytes;
                        bool equal = std::memcmp(at, pixel.data(), bytes) == 0;
                        // UNORM attachment conversion may differ by one quantization unit.
                        if (use == ExternalTextureTestUse::RenderTarget && sub.Range.Aspects == TextureAspect::Depth && (d.Format == TextureFormat::D16_UNORM || d.Format == TextureFormat::D24_UNORM_S8_UINT)) {
                            uint32_t a = 0, b = 0;
                            std::memcpy(&a, at, bytes);
                            std::memcpy(&b, pixel.data(), bytes);
                            equal = std::abs(int64_t(a) - int64_t(b)) <= 1;
                        }
                        if (!equal) {
                            string actualBytes, expectedBytes;
                            for (uint32_t i = 0; i < bytes; ++i) {
                                actualBytes += fmt::format("{:02x}", uint8_t(at[i]));
                                expectedBytes += fmt::format("{:02x}", uint8_t(pixel[i]));
                            }
                            ADD_FAILURE() << fmt::format("copy mismatch format={} mip={} layer={} aspect={} xyz={},{},{} frame={} bytes={} actual={} expected={}", d.Format, sub.Range.BaseMipLevel, sub.Range.BaseArrayLayer, sub.Range.Aspects, x, y, z, expected, bytes, actualBytes, expectedBytes);
                            valid = false;
                        }
                    }
        }
        readback->Unmap();
    }
    bool sampledValid = true;
    for (const auto& work : reads) {
        const auto& sub = work->Sub;
        const auto bytes = work->Readback->GetDesc().Size;
        const auto* actual = static_cast<const float*>(work->Readback->Map(0, bytes));
        if (!actual) return false;
        work->Readback->InvalidateMappedRange({0, bytes});
        for (uint32_t z = 0; sampledValid && z < sub.Depth; ++z)
            for (uint32_t y = 0; sampledValid && y < sub.Height; ++y)
                for (uint32_t x = 0; sampledValid && x < sub.Width; ++x)
                    for (uint32_t sample = 0; sampledValid && sample < d.SampleCount; ++sample) {
                        auto value = Values(d, use, expected, sub, x, y, z);
                        const bool srgb = reinterpretViews && !IsDepthStencilFormat(d.Format);
                        const auto stored = srgb ? ReinterpretedColor(value, false) : value;
                        if (srgb) value = ReinterpretedColor(value, true);
                        const auto at = (((z * sub.Height + y) * sub.Width + x) * d.SampleCount + sample) * 4;
                        for (uint32_t c = 0; c < 4; ++c) {
                            const auto observed = actual[at + c];
                            bool equal = std::isfinite(observed) && std::abs(observed - value[c]) <= 0.0001f;
                            if (srgb && c < 3 && std::isfinite(observed)) {
                                // Project acceptance threshold; Vulkan does not inherit the D3D sRGB precision contract.
                                const float encoded = observed <= 0.0031308f ? observed * 12.92f : 1.055f * std::pow(observed, 1.0f / 2.4f) - 0.055f;
                                equal = std::abs(encoded - stored[c]) <= 0.5f / 255.0f + 0.000001f;
                            }
                            if (!equal) {
                                ADD_FAILURE() << fmt::format("sample mismatch format={} mip={} layer={} aspect={} xyz={},{},{} sample={} c={} actual={} expected={}", d.Format, sub.Range.BaseMipLevel, sub.Range.BaseArrayLayer, sub.Range.Aspects, x, y, z, sample, c, actual[at + c], value[c]);
                                sampledValid = false;
                                break;
                            }
                        }
                    }
        work->Readback->Unmap();
    }
    return valid && sampledValid && context.RhiValidationErrors.load() == 0;
}
}  // namespace radray::render::test
