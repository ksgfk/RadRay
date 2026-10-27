#include "external_resource_test_utils.h"
#include "external_texture_test_utils.h"

#include <limits>
#include <filesystem>
#include <fstream>

namespace radray::render::test {
namespace {
unique_ptr<Shader> LoadStage(Device& device, std::string_view name, ShaderStage stage) {
    const string entry = stage == ShaderStage::Vertex ? "VSMain" : stage == ShaderStage::Pixel ? "PSMain"
                                                                                               : "CSMain";
    const auto backend = device.GetBackend();
    const auto path = std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_bytecode" /
                      (string{name} + "." + entry + (backend == RenderBackend::Vulkan ? ".spirv.bin" : ".dxil.bin"));
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0) return {};
    vector<byte> bytes(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    return device.CreateShader({bytes, backend == RenderBackend::Vulkan ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL, stage}).Unwrap();
}
BindingDescriptor TestBinding(uint32_t id, BindingKind kind, uint32_t reg) {
    return {id, kind, 1, ShaderStage::Compute, D3D12BindingDescriptor{GetBindingRegisterClass(kind), reg, 0}, VulkanBindingDescriptor{0, id}};
}
void BufferBarrier(CommandBuffer& command, Buffer* object, BufferStates before, BufferStates after, ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{.Target = object, .Before = before, .After = after, .ExternalTransfer = transfer};
    command.ResourceBarrier({&barrier, 1});
}
void TextureBarrier(CommandBuffer& command, Texture* object, TextureStates before, TextureStates after, ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{.Target = object, .Before = before, .After = after, .ExternalTransfer = transfer};
    command.ResourceBarrier({&barrier, 1});
}
uint8_t BufferPattern(uint32_t seed, uint64_t offset) { return uint8_t((seed * 31 + offset * 17 + offset / 251) & 255); }
void PixelPattern(TextureFormat format, uint32_t seed, uint32_t x, uint32_t y, byte* output) {
    const uint32_t n = (seed + x * 3 + y * 5) & 15;
    if (format == TextureFormat::RGBA8_UNORM || format == TextureFormat::BGRA8_UNORM) {
        uint8_t pixel[]{uint8_t(n * 17), uint8_t((seed & 15) * 17), uint8_t((x ^ y) & 255), 255};
        if (format == TextureFormat::BGRA8_UNORM) std::swap(pixel[0], pixel[2]);
        std::memcpy(output, pixel, sizeof(pixel));
    } else if (format == TextureFormat::RGBA16_FLOAT) {
        // Exact half values in [1, 2); include frame and both coordinates.
        const uint16_t pixel[]{uint16_t(0x3c00 + n * 32), uint16_t(0x3c00 + (seed & 15) * 32), uint16_t(0x3c00 + ((x ^ y) & 255)), 0x3c00};
        std::memcpy(output, pixel, sizeof(pixel));
    } else {
        const float pixel = float(seed * 1024 + x + y * 512);
        std::memcpy(output, &pixel, sizeof(pixel));
    }
}
}  // namespace

testing::AssertionResult ExternalSupportQuerySucceeded(const ExternalSupport& support) {
    // VK_ERROR_FORMAT_NOT_SUPPORTED is a capability result, unlike allocation/device/query errors.
    if (support.Reason.NativeApi == ExternalNativeApi::None || support.Reason.NativeCode == 0 ||
        (support.Reason.NativeApi == ExternalNativeApi::Vulkan && support.Reason.NativeCode == -11))
        return testing::AssertionSuccess();
    return testing::AssertionFailure() << "external capability query failed: api=" << uint32_t(support.Reason.NativeApi)
                                      << " code=" << support.Reason.NativeCode << " reason=" << support.Reason.Message;
}

ExternalSupport QueryExternalTextureSupport(DeviceContext& context, const TextureDescriptor& d, const ExternalTextureDescriptor& options, std::string_view label) {
    const auto support = context.Device->GetExternalResourceExtension()->QueryTextureSupport(d, options);
    RADRAY_INFO_LOG("ExternalQuery label='{}' backend={} adapter='{}' driver='{}' handle={} dim={} format={} usage={} extent={}x{}x{} mips={} samples={} reinterpret={} export={} import={} api={} code={} reason='{}'",
                    label, BackendName(context.Device->GetBackend()), context.AdapterName, context.DriverVersion, uint32_t(options.HandleType), d.Dim, d.Format, d.Usage,
                    d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.SampleCount, options.AllowFormatReinterpretation,
                    support.CanCreateExportable, support.CanImport, uint32_t(support.Reason.NativeApi), support.Reason.NativeCode, support.Reason.Message);
#if defined(RADRAY_ENABLE_D3D12)
    if (context.Device->GetBackend() == RenderBackend::D3D12) {
        auto* device = static_cast<d3d12::DeviceD3D12*>(context.NativeDevice.Get());
        const auto native = d3d12::MakeTextureResourceDesc(d, options.AllowFormatReinterpretation);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT typed{d3d12::MapType(d.Format), {}, {}};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT sampled{d3d12::MapShaderResourceType(d.Format), {}, {}};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT resource{native.Format, {}, {}};
        const auto typedResult = device->_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &typed, sizeof(typed));
        const auto sampledResult = device->_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &sampled, sizeof(sampled));
        const auto resourceResult = device->_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &resource, sizeof(resource));
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{typed.Format, d.SampleCount, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        const auto qualityResult = device->_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
        RADRAY_INFO_LOG("ExternalQueryNative label='{}' api=d3d12 dimension={} resourceFormat={} flags={} layout={} alignment={} typedFormat={} typedResult={} typedSupport1={} typedSupport2={} sampledFormat={} sampledResult={} sampledSupport1={} sampledSupport2={} resourceResult={} resourceSupport1={} resourceSupport2={} qualityResult={} qualityLevels={}",
                        label, uint32_t(native.Dimension), uint32_t(native.Format), uint32_t(native.Flags), uint32_t(native.Layout), native.Alignment,
                        uint32_t(typed.Format), int64_t(typedResult), uint32_t(typed.Support1), uint32_t(typed.Support2), uint32_t(sampled.Format), int64_t(sampledResult), uint32_t(sampled.Support1), uint32_t(sampled.Support2),
                        int64_t(resourceResult), uint32_t(resource.Support1), uint32_t(resource.Support2), int64_t(qualityResult), quality.NumQualityLevels);
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (context.Device->GetBackend() == RenderBackend::Vulkan) {
        auto* device = static_cast<vulkan::DeviceVulkan*>(context.NativeDevice.Get());
        if (device->_externalMemoryWin32) {
            const auto native = vulkan::MakeTextureImageCreateInfo(d, options.AllowFormatReinterpretation);
            const auto handle = options.HandleType == ExternalMemoryHandleType::D3D12Resource ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            const VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr, handle};
            const VkPhysicalDeviceImageFormatInfo2 input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external, native.format, native.imageType, native.tiling, native.usage, native.flags};
            VkExternalImageFormatProperties externalProperties{};
            externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 output{};
            output.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            output.pNext = &externalProperties;
            const auto result = vkGetPhysicalDeviceImageFormatProperties2(device->_physicalDevice, &input, &output);
            const auto& limits = output.imageFormatProperties;
            const auto& memory = externalProperties.externalMemoryProperties;
            RADRAY_INFO_LOG("ExternalQueryNative label='{}' api=vulkan handle={} format={} type={} tiling={} usage={} flags={} result={} features={} compatible={} exportFromImported={} maxExtent={}x{}x{} maxMips={} maxLayers={} sampleCounts={} maxResourceSize={} maxFramebuffer={}x{}",
                            label, uint32_t(handle), uint32_t(native.format), uint32_t(native.imageType), uint32_t(native.tiling), native.usage, native.flags, int32_t(result), memory.externalMemoryFeatures, memory.compatibleHandleTypes, memory.exportFromImportedHandleTypes,
                            limits.maxExtent.width, limits.maxExtent.height, limits.maxExtent.depth, limits.maxMipLevels, limits.maxArrayLayers, limits.sampleCounts, limits.maxResourceSize, device->_properties.limits.maxFramebufferWidth, device->_properties.limits.maxFramebufferHeight);
        }
    }
#endif
    return support;
}

BufferDescriptor ExternalTestBufferDescriptor(uint32_t generation) {
    return {.Size = 1027 + generation * 4, .Memory = MemoryType::Device, .Usage = BufferUse::CopySource | BufferUse::CopyDestination};
}
TextureDescriptor ExternalTestTextureDescriptor(TextureFormat format, ExternalTextureTestUse use, uint32_t generation) {
    auto usage = TextureUse::CopySource | TextureUse::CopyDestination;
    if (use == ExternalTextureTestUse::Sampled) usage |= TextureUse::Resource;
    if (use == ExternalTextureTestUse::Storage) usage |= TextureUse::UnorderedAccess;
    if (use == ExternalTextureTestUse::RenderTarget) usage |= TextureUse::RenderTarget;
    return {.Dim = TextureDimension::Dim2D, .Width = 257 + generation * 2, .Height = 131 + generation * 2, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = format, .Memory = MemoryType::Device, .Usage = usage};
}
bool CreateExternalTestSlot(ExternalResourceExtension& extension, TextureFormat format, ExternalTextureTestUse use, uint32_t generation, ExternalTestSlot& slot, const ExternalResourceDescriptor& external) {
    return TakeExternal(extension.CreateExportableBuffer(ExternalTestBufferDescriptor(generation), external), slot.BufferObject) &&
           TakeExternal(extension.CreateExportableTexture(ExternalTestTextureDescriptor(format, use, generation), {external.HandleType}), slot.TextureObject);
}
bool ExportExternalTestSlot(ExternalResourceExtension& extension, ExternalTestSlot& slot, ExternalTestExports& exported) {
    return TakeExternal(extension.ExportBuffer(slot.BufferObject.get()), exported.BufferObject) &&
           TakeExternal(extension.ExportTexture(slot.TextureObject.get()), exported.TextureObject);
}
bool ImportExternalTestSlot(ExternalResourceExtension& extension, const ExternalTestExports& exported, ExternalTestSlot& slot) {
    return TakeExternal(extension.ImportBuffer(exported.BufferObject.Handle.Borrow(), exported.BufferObject.Info), slot.BufferObject) &&
           TakeExternal(extension.ImportTexture(exported.TextureObject.Handle.Borrow(), exported.TextureObject.Info), slot.TextureObject);
}
bool ExternalTestTurn(DeviceContext& context, ExternalTestSlot& slot, ExternalTextureTestUse use,
                      uint32_t expected, uint32_t written, bool first, Fence* wait, uint64_t waitValue, Fence* signal, uint64_t signalValue) {
    if (use == ExternalTextureTestUse::Copy)
        return ExternalTextureTurn(context, slot.TextureObject.get(), use, expected, written, first, wait, waitValue, signal, signalValue, slot.BufferObject.get());
    auto& device = *context.Device;
    const auto d = slot.TextureObject->GetDesc();
    const auto bytes = GetTextureFormatBytesPerPixel(d.Format);
    const auto align = device.GetCapabilities().Detail.TextureDataPitchAlignment;
    const uint64_t pitch = (uint64_t(d.Width) * bytes + align - 1) / align * align;
    const uint64_t textureSize = pitch * d.Height;
    const uint64_t bufferSize = slot.BufferObject->GetDesc().Size;
    // Texture copy offsets satisfy both APIs' alignment, independently of logical buffer size.
    const uint64_t offset = 65536;
    const uint64_t size = offset + textureSize;
    auto upload = device.CreateBuffer({.Size = size, .Memory = MemoryType::Upload, .Usage = BufferUse::CopySource | BufferUse::MapWrite}).Unwrap();
    auto readback = device.CreateBuffer({.Size = size, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto* data = static_cast<byte*>(upload->Map(0, size));
    if (!data) return false;
    std::memset(data, 0, size_t(size));
    for (uint64_t i = 0; i < bufferSize; ++i) data[i] = byte{BufferPattern(written, i)};
    for (uint32_t y = 0; y < d.Height; ++y)
        for (uint32_t x = 0; x < d.Width; ++x) PixelPattern(d.Format, written, x, y, data + offset + pitch * y + bytes * x);
    upload->FlushMappedRange({0, size});
    upload->Unmap();
    unique_ptr<Shader> cs, vs, ps;
    unique_ptr<Sampler> sampler;
    unique_ptr<BindingGroupLayout> group;
    unique_ptr<PipelineLayout> layout;
    unique_ptr<ComputePipelineState> compute;
    unique_ptr<GraphicsPipelineState> graphics;
    unique_ptr<ShaderParameterSet> parameters;
    unique_ptr<TextureView> view;
    unique_ptr<Buffer> sampled, sampledReadback;
    const uint32_t formatIndex = (d.Format == TextureFormat::RGBA8_UNORM || d.Format == TextureFormat::BGRA8_UNORM) ? 0 : d.Format == TextureFormat::RGBA16_FLOAT ? 1
                                                                                                                                                                  : 2;
    const uint32_t pushValues[]{written, d.Width, d.Height, formatIndex};
    const auto stages = use == ExternalTextureTestUse::RenderTarget ? ShaderStage::Pixel : ShaderStage::Compute;
    const PushConstantDescriptor push{0, 16, stages, PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
    if (use == ExternalTextureTestUse::Sampled || use == ExternalTextureTestUse::Storage) {
        const bool sample = use == ExternalTextureTestUse::Sampled;
        cs = LoadStage(device, sample ? "interop_sample" : d.Format == TextureFormat::BGRA8_UNORM ? "interop_storage_bgra8"
                                                       : formatIndex == 0                         ? "interop_storage_rgba8"
                                                       : formatIndex == 1                         ? "interop_storage_rgba16"
                                                                                                  : "interop_storage_r32",
                       ShaderStage::Compute);
        if (!cs) return false;
        vector<BindingDescriptor> bindings{TestBinding(0, sample ? BindingKind::Texture : BindingKind::RWTexture, 0)};
        if (sample) {
            bindings.push_back(TestBinding(1, BindingKind::RWStructuredBuffer, 0));
            bindings.push_back(TestBinding(2, BindingKind::Sampler, 0));
        }
        group = device.CreateBindingGroupLayout({bindings}).Unwrap();
        const PipelineLayoutGroupDescriptor groupSlot{0, group.get()};
        layout = device.CreatePipelineLayout({{&groupSlot, 1}, {&push, 1}}).Unwrap();
        compute = device.CreateComputePipelineState({layout.get(), {cs.get(), "CSMain"}}).Unwrap();
        view = device.CreateTextureView({slot.TextureObject.get(), TextureDimension::Dim2D, d.Format, {0, 1, 0, 1}, sample ? TextureViewUsage::Resource : TextureViewUsage::UnorderedAccess}).Unwrap();
        parameters = device.CreateShaderParameterSet({group.get()}).Unwrap();
        if (!parameters->Set(0, 0, view.get())) return false;
        if (sample) {
            const uint64_t outputSize = uint64_t(d.Width) * d.Height * 16;
            sampled = device.CreateBuffer({.Size = outputSize, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
            sampledReadback = device.CreateBuffer({.Size = outputSize, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
            sampler = device.CreateSampler({.AddressS = AddressMode::ClampToEdge, .AddressT = AddressMode::ClampToEdge, .AddressR = AddressMode::ClampToEdge, .MinFilter = FilterMode::Nearest, .MagFilter = FilterMode::Nearest, .MipmapFilter = FilterMode::Nearest}).Unwrap();
            if (!sampler || !parameters->Set(1, 0, ShaderBufferBinding{sampled.get(), {0, outputSize}, 16}) || !parameters->Set(2, 0, sampler.get())) return false;
        }
        if (!parameters->FlushWrites()) return false;
    } else if (use == ExternalTextureTestUse::RenderTarget) {
        vs = LoadStage(device, "interop_color", ShaderStage::Vertex);
        ps = LoadStage(device, "interop_color", ShaderStage::Pixel);
        if (!vs || !ps) return false;
        layout = device.CreatePipelineLayout({{}, {&push, 1}}).Unwrap();
        const auto color = ColorTargetState::Default(d.Format);
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        graphics = device.CreateGraphicsPipelineState({.PipelineLayout = layout.get(), .VS = ShaderEntry{vs.get(), "VSMain"}, .PS = ShaderEntry{ps.get(), "PSMain"}, .Primitive = primitive, .MultiSample = {1, std::numeric_limits<uint64_t>::max(), false}, .ColorTargets = {&color, 1}}).Unwrap();
        view = device.CreateTextureView({slot.TextureObject.get(), TextureDimension::Dim2D, d.Format, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    }
    auto allocator = device.CreateCommandAllocator(context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    GpuWorkGuard work{context};
    command->Begin();
    if (first) {
        BufferBarrier(*command, slot.BufferObject.get(), BufferState::Undefined, BufferState::CopyDestination);
        TextureBarrier(*command, slot.TextureObject.get(), TextureState::Undefined, TextureState::CopyDestination);
    } else {
        BufferBarrier(*command, slot.BufferObject.get(), BufferState::Common, BufferState::CopySource, ExternalOwnershipTransfer::Acquire);
        if (written % 2 == 0) {
            // Same-layout ownership transfer must survive even though no local state transition is needed.
            TextureBarrier(*command, slot.TextureObject.get(), TextureState::Common, TextureState::Common, ExternalOwnershipTransfer::Acquire);
            TextureBarrier(*command, slot.TextureObject.get(), TextureState::Common, TextureState::CopySource);
        } else
            TextureBarrier(*command, slot.TextureObject.get(), TextureState::Common, TextureState::CopySource, ExternalOwnershipTransfer::Acquire);
        command->CopyBufferToBuffer(readback.get(), 0, slot.BufferObject.get(), 0, bufferSize);
        command->CopyTextureToBuffer(readback.get(), offset, slot.TextureObject.get(), {0, 1, 0, 1});
        BufferBarrier(*command, slot.BufferObject.get(), BufferState::CopySource, BufferState::CopyDestination);
        if (use == ExternalTextureTestUse::Sampled) {
            TextureBarrier(*command, slot.TextureObject.get(), TextureState::CopySource, TextureState::ShaderRead);
            BufferBarrier(*command, sampled.get(), BufferState::Undefined, BufferState::UnorderedAccess);
            auto pass = command->BeginComputePass().Unwrap();
            pass->BindComputePipelineState(compute.get());
            pass->BindShaderParameterSet(0, parameters.get());
            if (!pass->SetPushConstants(0, 0, std::as_bytes(std::span{pushValues}))) return false;
            pass->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
            command->EndComputePass(std::move(pass));
            BufferBarrier(*command, sampled.get(), BufferState::UnorderedAccess, BufferState::CopySource);
            command->CopyBufferToBuffer(sampledReadback.get(), 0, sampled.get(), 0, sampled->GetDesc().Size);
            TextureBarrier(*command, slot.TextureObject.get(), TextureState::ShaderRead, TextureState::CopyDestination);
        } else
            TextureBarrier(*command, slot.TextureObject.get(), TextureState::CopySource, TextureState::CopyDestination);
    }
    command->CopyBufferToBuffer(slot.BufferObject.get(), 0, upload.get(), 0, bufferSize);
    command->CopyBufferToTexture(slot.TextureObject.get(), {0, 1, 0, 1}, upload.get(), offset);
    TextureStates finalState = TextureState::CopyDestination;
    if (use == ExternalTextureTestUse::Storage) {
        finalState = TextureState::UnorderedAccess;
        TextureBarrier(*command, slot.TextureObject.get(), TextureState::CopyDestination, finalState);
        auto pass = command->BeginComputePass().Unwrap();
        pass->BindComputePipelineState(compute.get());
        pass->BindShaderParameterSet(0, parameters.get());
        if (!pass->SetPushConstants(0, 0, std::as_bytes(std::span{pushValues}))) return false;
        pass->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
        command->EndComputePass(std::move(pass));
    } else if (use == ExternalTextureTestUse::RenderTarget) {
        finalState = TextureState::RenderTarget;
        TextureBarrier(*command, slot.TextureObject.get(), TextureState::CopyDestination, finalState);
        const RenderPassColorAttachment color{view.get(), LoadAction::Load, StoreAction::Store};
        auto pass = command->BeginRenderPass({.Colors = {&color, 1}, .Width = d.Width, .Height = d.Height}).Unwrap();
        pass->BindGraphicsPipelineState(graphics.get());
        pass->SetViewport({0, 0, float(d.Width), float(d.Height), 0, 1});
        pass->SetScissor({0, 0, d.Width, d.Height});
        if (!pass->SetPushConstants(0, 0, std::as_bytes(std::span{pushValues}))) return false;
        pass->Draw(3, 1, 0, 0);
        command->EndRenderPass(std::move(pass));
    }
    BufferBarrier(*command, slot.BufferObject.get(), BufferState::CopyDestination, BufferState::Common, ExternalOwnershipTransfer::Release);
    TextureBarrier(*command, slot.TextureObject.get(), finalState, TextureState::Common, ExternalOwnershipTransfer::Release);
    command->End();
    CommandBuffer* commands[]{command.get()};
    Fence* signals[]{signal};
    Fence* waits[]{wait};
    context.Queue->Submit({.CmdBuffers = commands, .SignalFences = signals, .SignalValues = {&signalValue, 1}, .WaitFences = waitValue ? std::span{waits} : std::span<Fence*>{}, .WaitValues = waitValue ? std::span{&waitValue, 1} : std::span<uint64_t>{}});
    context.Queue->Wait();
    if (first) return context.RhiValidationErrors.load() == 0;
    const auto* actual = static_cast<const byte*>(readback->Map(0, size));
    if (!actual) return false;
    readback->InvalidateMappedRange({0, size});
    bool valid = true;
    for (uint64_t i = 0; i < bufferSize; ++i)
        if (actual[i] != byte{BufferPattern(expected, i)}) {
            ADD_FAILURE() << "buffer mismatch offset=" << i << " frame=" << expected;
            valid = false;
            break;
        }
    array<byte, 8> pixel{};
    for (uint32_t y = 0; valid && y < d.Height; ++y)
        for (uint32_t x = 0; valid && x < d.Width; ++x) {
            PixelPattern(d.Format, expected, x, y, pixel.data());
            if (std::memcmp(actual + offset + pitch * y + bytes * x, pixel.data(), bytes) != 0) {
                ADD_FAILURE() << "texture mismatch x=" << x << " y=" << y << " frame=" << expected;
                valid = false;
            }
        }
    readback->Unmap();
    if (sampledReadback) {
        const auto outputSize = sampledReadback->GetDesc().Size;
        const auto* values = static_cast<const float*>(sampledReadback->Map(0, outputSize));
        if (!values) return false;
        sampledReadback->InvalidateMappedRange({0, outputSize});
        for (uint32_t y = 0; valid && y < d.Height; ++y)
            for (uint32_t x = 0; valid && x < d.Width; ++x) {
                PixelPattern(d.Format, expected, x, y, pixel.data());
                array<float, 4> expectedPixel{0, 0, 0, 1};
                if (formatIndex == 0) {
                    for (uint32_t c = 0; c < 4; ++c) expectedPixel[c] = float(uint8_t(pixel[c])) / 255.0f;
                    if (d.Format == TextureFormat::BGRA8_UNORM) std::swap(expectedPixel[0], expectedPixel[2]);
                } else if (formatIndex == 1) {
                    array<uint16_t, 4> halves{};
                    std::memcpy(halves.data(), pixel.data(), 8);
                    for (uint32_t c = 0; c < 4; ++c) expectedPixel[c] = 1.0f + float(halves[c] - 0x3c00) / 1024.0f;
                } else
                    std::memcpy(expectedPixel.data(), pixel.data(), 4);
                for (uint32_t c = 0; c < 4; ++c)
                    if (!std::isfinite(values[(y * d.Width + x) * 4 + c]) || std::abs(values[(y * d.Width + x) * 4 + c] - expectedPixel[c]) > 0.00001f) {
                        ADD_FAILURE() << "sample mismatch x=" << x << " y=" << y << " channel=" << c << " actual=" << values[(y * d.Width + x) * 4 + c] << " expected=" << expectedPixel[c];
                        valid = false;
                        break;
                    }
            }
        sampledReadback->Unmap();
    }
    return valid && context.RhiValidationErrors.load() == 0;
}
}  // namespace radray::render::test
