#include "gpu_test_fixture.h"
#include "rhi_validation.h"
#include <filesystem>
#include <radray/file.h>
#include <radray/scope_guard.h>

#if defined(RADRAY_ENABLE_VULKAN)
namespace radray::render::vulkan {
namespace {

TEST(VulkanCommandContracts, SwapchainExtentHonorsSurfaceBoundsAndSuspension) {
    VkSurfaceCapabilitiesKHR caps{};
    caps.minImageExtent = {16, 8};
    caps.maxImageExtent = {4096, 2048};
    caps.currentExtent = {UINT32_MAX, UINT32_MAX};
    auto extent = ResolveSwapChainExtent({1, 8192}, caps);
    ASSERT_TRUE(extent);
    EXPECT_EQ(extent->width, 16u);
    EXPECT_EQ(extent->height, 2048u);
    caps.currentExtent = {320, 240};
    extent = ResolveSwapChainExtent({640, 480}, caps);
    ASSERT_TRUE(extent);
    EXPECT_EQ(extent->width, 320u);
    EXPECT_EQ(extent->height, 240u);
    caps.currentExtent = {0, 0};
    EXPECT_FALSE(ResolveSwapChainExtent({640, 480}, caps));
    caps.currentExtent = {UINT32_MAX, UINT32_MAX};
    caps.minImageExtent = caps.maxImageExtent = {0, 0};
    EXPECT_FALSE(ResolveSwapChainExtent({640, 480}, caps));
}

TEST(VulkanCommandContracts, GraphicsStateRequiresEnabledFeatures) {
    DeviceFeaturesVulkan features;
    features.Core.depthClamp = features.Core.depthBiasClamp = features.Core.fillModeNonSolid = VK_TRUE;
    features.Core.independentBlend = features.Core.dualSrcBlend = VK_TRUE;
    ColorTargetState colors[]{ColorTargetState::Default(TextureFormat::RGBA8_UNORM), ColorTargetState::Default(TextureFormat::BGRA8_UNORM)};
    GraphicsPipelineStateDescriptor desc{.Primitive = PrimitiveState::Default(), .ColorTargets = colors};
    EXPECT_FALSE(GetUnsupportedGraphicsStateVulkan(desc, features, true));
    auto disabled = features;
    disabled.Core.depthClamp = VK_FALSE;
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    desc.Primitive.UnclippedDepth = false;
    EXPECT_FALSE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    desc.Primitive.Poly = PolygonMode::Line;
    disabled.Core.fillModeNonSolid = VK_FALSE;
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    desc.Primitive.Poly = PolygonMode::Fill;
    desc.Primitive.Conservative = true;
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, features, false));
    desc.Primitive.Conservative = false;
    desc.DepthStencil = DepthStencilState{};
    desc.DepthStencil->DepthBias.Constant = 1;
    desc.DepthStencil->DepthBias.Clamp = 1;
    disabled = features;
    disabled.Core.depthBiasClamp = VK_FALSE;
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    desc.DepthStencil.reset();
    disabled = features;
    disabled.Core.independentBlend = VK_FALSE;
    EXPECT_FALSE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    colors[1].WriteMask = ColorWrite::Red;
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    colors[1].WriteMask = ColorWrite::All;
    colors[1].Blend = BlendState::Default();
    EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    disabled = features;
    disabled.Core.dualSrcBlend = VK_FALSE;
    for (auto factor : {BlendFactor::Src1, BlendFactor::OneMinusSrc1, BlendFactor::Src1Alpha, BlendFactor::OneMinusSrc1Alpha}) {
        colors[1].Blend->Color.Src = factor;
        EXPECT_TRUE(GetUnsupportedGraphicsStateVulkan(desc, disabled, true));
    }
}

class VulkanCommandContractsGpu : public testing::Test {
protected:
    test::DeviceContext Context;

    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::Vulkan, Context, true)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::Vulkan))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
    void TearDown() override { Context.Reset(); }
    shared_ptr<Device> Validated() {
        return render::detail::CreateValidationDevice(Context.Device, {.IsEnabled = true, .LogCallback = test::CaptureRhiValidationMessage, .LogUserData = &Context}).Unwrap();
    }
    bool UseDedicatedDevice(QueueType type) {
        const auto* initial = static_cast<DeviceVulkan*>(Context.Device.get());
        vector<VkQueueFamilyProperties> families;
        EnumerateVectorFromVkFunc(families, vkGetPhysicalDeviceQueueFamilyProperties, initial->_physicalDevice);
        vector<uint32_t> used(families.size(), 0);
        const auto direct = AllocateQueueIndicesVulkan(QueueType::Direct, 1, families, used);
        if (direct.empty()) return false;
        const auto family = direct[0].Family;
        const auto count = families[family].queueCount;
        used[family] = count;
        const auto candidate = AllocateQueueIndicesVulkan(type, 1, families, used);
        const VkQueueFlags unwanted = type == QueueType::Compute ? uint32_t{VK_QUEUE_GRAPHICS_BIT} : uint32_t{VK_QUEUE_GRAPHICS_BIT} | uint32_t{VK_QUEUE_COMPUTE_BIT};
        if (candidate.empty() || (families[candidate[0].Family].queueFlags & unwanted)) return false;
        const VulkanCommandQueueDescriptor requests[]{{QueueType::Direct, count}, {type, 1}};
        auto native = CreateDeviceVulkan({.Queues = requests});
        if (!native) {
            ADD_FAILURE() << "Dedicated queue device creation failed";
            return false;
        }
        Context.WaitAllQueues();
        Context.Device = native.Release();
        Context.Queue = Context.Device->GetCommandQueue(QueueType::Direct, 0).Unwrap();
        return true;
    }
};

TEST_F(VulkanCommandContractsGpu, ConstantBlendDrawUsesZeroConstantsAndRejectsDisabledDepthClamp) {
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
    auto requests = GetRhiFeatureRequestsVulkan();
    requests.DepthClamp = FeatureRequirementVulkan::Disabled;
    auto native = CreateDeviceVulkan({.Queues = queues}, requests).Unwrap();
    Context.Device = native;
    Context.Queue = native->GetCommandQueue(QueueType::Direct, 0).Unwrap();
    auto device = Validated();
    const auto root = std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_bytecode";
    const auto vertexBytes = ReadBinaryFile(root / "no_resource_graphics.VSMain.spirv.bin");
    const auto pixelBytes = ReadBinaryFile(root / "no_resource_graphics.PSMain.spirv.bin");
    ASSERT_TRUE(vertexBytes);
    ASSERT_TRUE(pixelBytes);
    auto vs = device->CreateShader({*vertexBytes, ShaderBlobCategory::SPIRV, ShaderStage::Vertex}).Unwrap();
    auto ps = device->CreateShader({*pixelBytes, ShaderBlobCategory::SPIRV, ShaderStage::Pixel}).Unwrap();
    auto layout = device->CreatePipelineLayout({}).Unwrap();
    const VertexBufferLayout vertexLayout{0, 3 * sizeof(float), VertexStepMode::Vertex};
    const VertexAttribute attribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0};
    auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    color.Blend = BlendState::Default();
    color.Blend->Color.Src = BlendFactor::Constant;
    GraphicsPipelineStateDescriptor desc{.PipelineLayout = layout.get(), .VS = ShaderEntry{vs.get(), "VSMain"}, .PS = ShaderEntry{ps.get(), "PSMain"}, .VertexInput = {std::span{&vertexLayout, 1}, std::span{&attribute, 1}}, .Primitive = PrimitiveState::Default(), .DepthStencil = {}, .MultiSample = MultiSampleState::Default(), .ColorTargets = std::span{&color, 1}};
    EXPECT_FALSE(device->CreateGraphicsPipelineState(desc));
    EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
    desc.Primitive.Cull = CullMode::None;
    desc.Primitive.UnclippedDepth = false;
    auto pipeline = device->CreateGraphicsPipelineState(desc).Unwrap();
    const float vertices[]{-1, -1, 0, 3, -1, 0, -1, 3, 0};
    auto vb = test::MakeUploadBuffer(*device, std::as_bytes(std::span{vertices}), BufferUse::Vertex).Unwrap();
    auto target = test::MakeRenderTarget(device.get(), TextureFormat::RGBA8_UNORM, 8, 8, TextureUse::RenderTarget | TextureUse::CopySource);
    ASSERT_TRUE(target);
    const auto alignment = std::max<uint64_t>(1, device->GetDetail().TextureDataPitchAlignment);
    const auto pitch = (32 + alignment - 1) / alignment * alignment;
    auto readback = device->CreateBuffer({.Size = pitch * 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
    auto allocator = device->CreateCommandAllocator(device->GetCommandQueue(QueueType::Direct, 0).Unwrap()).Unwrap();
    auto command = device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{target->Tex.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&barrier, 1});
    const RenderPassColorAttachment attachment{target->View.get(), LoadAction::Clear, StoreAction::Store, {}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    encoder->BindGraphicsPipelineState(pipeline.get());
    encoder->SetViewport({0, 0, 8, 8, 0, 1});
    encoder->SetScissor({0, 0, 8, 8});
    const VertexBufferBinding binding{0, {vb.get(), 0, sizeof(vertices)}};
    encoder->BindVertexBuffers(std::span{&binding, 1});
    encoder->Draw(3, 1, 0, 0);
    command->EndRenderPass(std::move(encoder));
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{target->Tex.get(), TextureState::RenderTarget, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, target->Tex.get(), {0, 1, 0, 1});
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard work{Context};
    device->GetCommandQueue(QueueType::Direct, 0)->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    const auto* pixels = static_cast<const uint8_t*>(readback->Map(0, pitch * 8));
    ASSERT_NE(pixels, nullptr);
    readback->InvalidateMappedRange({0, pitch * 8});
    const auto center = 4 * pitch + 16;
    EXPECT_EQ(pixels[center], 0);
    EXPECT_EQ(pixels[center + 1], 0);
    EXPECT_EQ(pixels[center + 2], 0);
    EXPECT_EQ(pixels[center + 3], 255);
    readback->Unmap();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
}

TEST_F(VulkanCommandContractsGpu, DepthStencilPlanesRoundTripAcrossMipsAndLayers) {
    auto device = Validated();
    auto texture = device->CreateTexture({.Dim = TextureDimension::Dim2DArray, .Width = 3, .Height = 3, .DepthOrArraySize = 2, .MipLevels = 2, .SampleCount = 1, .Format = TextureFormat::D32_FLOAT_S8_UINT, .Usage = TextureUse::DepthStencilWrite | TextureUse::CopySource | TextureUse::CopyDestination}).Unwrap();
    const auto alignment = std::max<uint64_t>(4, device->GetDetail().TextureDataPitchAlignment);
    array<vector<byte>, 2> data;
    array<unique_ptr<Buffer>, 2> uploads, readbacks;
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const uint32_t bpp = plane ? 1 : 4;
        for (uint32_t mip = 0; mip < 2; ++mip) {
            const uint32_t width = std::max(1u, 3u >> mip);
            const auto pitch = (width * bpp + alignment - 1) / alignment * alignment;
            for (uint32_t layer = 0; layer < 2; ++layer) {
                const auto start = data[plane].size();
                data[plane].resize(start + pitch * width);
                for (uint32_t y = 0; y < width; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        const auto at = start + y * pitch + x * bpp;
                        if (plane)
                            data[plane][at] = static_cast<byte>(17 + mip * 5 + layer * 3 + y + x);
                        else {
                            const float value = float(1 + mip + layer + x + y) / 16.0f;
                            std::memcpy(data[plane].data() + at, &value, sizeof(value));
                        }
                    }
                }
            }
        }
        uploads[plane] = test::MakeUploadBuffer(*device, data[plane], BufferUse::CopySource).Unwrap();
        readbacks[plane] = device->CreateBuffer({.Size = data[plane].size(), .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
    }
    auto allocator = device->CreateCommandAllocator(device->GetCommandQueue(QueueType::Direct, 0).Unwrap()).Unwrap();
    auto command = device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor toDst = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::CopyDestination};
    command->ResourceBarrier(std::span{&toDst, 1});
    command->CopyBufferToTexture(texture.get(), {0, 2, 0, 2}, uploads[0].get(), 0);
    EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
    for (uint32_t plane = 0; plane < 2; ++plane)
        command->CopyBufferToTexture(texture.get(), {0, 2, 0, 2, plane ? TextureAspect::Stencil : TextureAspect::Depth}, uploads[plane].get(), 0);
    const ResourceBarrierDescriptor toSrc = BarrierTextureDescriptor{texture.get(), TextureState::CopyDestination, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toSrc, 1});
    for (uint32_t plane = 0; plane < 2; ++plane)
        command->CopyTextureToBuffer(readbacks[plane].get(), 0, texture.get(), {0, 2, 0, 2, plane ? TextureAspect::Stencil : TextureAspect::Depth});
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard work{Context};
    device->GetCommandQueue(QueueType::Direct, 0)->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    for (uint32_t plane = 0; plane < 2; ++plane) {
        const auto* bytes = static_cast<const byte*>(readbacks[plane]->Map(0, data[plane].size()));
        ASSERT_NE(bytes, nullptr);
        readbacks[plane]->InvalidateMappedRange({0, data[plane].size()});
        const uint32_t bpp = plane ? 1 : 4;
        uint64_t start = 0;
        for (uint32_t mip = 0; mip < 2; ++mip) {
            const uint32_t width = std::max(1u, 3u >> mip);
            const auto pitch = (width * bpp + alignment - 1) / alignment * alignment;
            for (uint32_t layer = 0; layer < 2; ++layer) {
                for (uint32_t y = 0; y < width; ++y)
                    EXPECT_EQ(std::memcmp(bytes + start + y * pitch, data[plane].data() + start + y * pitch, width * bpp), 0);
                start += pitch * width;
            }
        }
        readbacks[plane]->Unmap();
    }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
}

TEST_F(VulkanCommandContractsGpu, DedicatedComputeSupportsDefaultBarriersAndTimestampQueries) {
    if (!UseDedicatedDevice(QueueType::Compute)) GTEST_SKIP() << "Dedicated compute queue unavailable";
    auto device = Validated();
    auto buffer = device->CreateBuffer({.Size = 256, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess}).Unwrap();
    auto texture = device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::UnorderedAccess}).Unwrap();
    auto pool = device->CreateQueryPool({.Count = 1}).Unwrap();
    auto readback = device->CreateBuffer({.Size = 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination}).Unwrap();
    auto queue = device->GetCommandQueue(QueueType::Compute, 0).Unwrap();
    auto allocator = device->CreateCommandAllocator(queue).Unwrap();
    auto command = device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barriers[]{BarrierBufferDescriptor{buffer.get(), BufferState::Common, BufferState::UnorderedAccess},
                                               BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::UnorderedAccess}};
    command->ResourceBarrier(barriers);
    const ResourceBarrierDescriptor invalid = BarrierBufferDescriptor{buffer.get(), BufferState::UnorderedAccess, BufferState::UnorderedAccess,
                                                                      BufferRange::AllRange(), ShaderStage::Vertex, ShaderStage::Compute};
    command->ResourceBarrier(std::span{&invalid, 1});
    EXPECT_EQ(Context.RhiValidationErrors.load(), 1u);
    command->ResetQueryPool(pool.get(), 0, 1);
    command->WriteTimestamp({pool.get(), QueryPipelineStage::Graphics, 0});
    EXPECT_EQ(Context.RhiValidationErrors.load(), 2u);
    auto* nativeQueue = static_cast<QueueVulkan*>(Context.Device->GetCommandQueue(QueueType::Compute, 0).Unwrap());
    if (nativeQueue->_timestampValidBits) {
        command->WriteTimestamp({pool.get(), QueryPipelineStage::Compute, 0});
        command->ResolveQueryData({.Pool = pool.get(), .Count = 1, .Destination = readback.get()});
        const auto validBits = nativeQueue->_timestampValidBits;
        nativeQueue->_timestampValidBits = 0;
        command->WriteTimestamp({pool.get(), QueryPipelineStage::Top, 0});
        nativeQueue->_timestampValidBits = validBits;
        EXPECT_EQ(Context.RhiValidationErrors.load(), 3u);
    }
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard work{Context};
    queue->Submit({.CmdBuffers = commands});
    queue->Wait();
}

TEST_F(VulkanCommandContractsGpu, DedicatedTransferRejectsQueryResetResolveAndShaderBarriers) {
    if (!UseDedicatedDevice(QueueType::Copy)) GTEST_SKIP() << "Dedicated transfer queue unavailable";
    auto device = Validated();
    auto pool = device->CreateQueryPool({.Count = 1}).Unwrap();
    auto buffer = device->CreateBuffer({.Size = 16, .Memory = MemoryType::Device, .Usage = BufferUse::CopyDestination | BufferUse::UnorderedAccess}).Unwrap();
    auto queue = device->GetCommandQueue(QueueType::Copy, 0).Unwrap();
    auto allocator = device->CreateCommandAllocator(queue).Unwrap();
    auto command = device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    command->ResetQueryPool(pool.get(), 0, 1);
    command->ResolveQueryData({.Pool = pool.get(), .Count = 1, .Destination = buffer.get()});
    command->WriteTimestamp({pool.get(), QueryPipelineStage::Compute, 0});
    const ResourceBarrierDescriptor invalid = BarrierBufferDescriptor{buffer.get(), BufferState::Common, BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&invalid, 1});
    command->End();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 4u);
}

}  // namespace
}  // namespace radray::render::vulkan
#endif
