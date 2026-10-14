#include "gpu_test_fixture.h"
#include <cstring>
#include <radray/basic_math.h>
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_render_pass_cache.h>
#endif

namespace radray::render::test {
#ifdef RADRAY_ENABLE_VULKAN
TEST(RenderScopeKeys, ClearOperationsCompatibilityAndOrderedViews) {
    using namespace vulkan;
    AttachmentSignature signature{{TextureFormat::RGBA8_UNORM, TextureFormat::RG16_FLOAT}, {}, 1};
    array<RenderPassColorAttachment, 2> colors{{{nullptr, LoadAction::Clear}, {nullptr, LoadAction::Load}}};
    RenderPassBeginDescriptor scope{colors, {}, 32, 16, 1};
    const auto original = RenderPassExecutionKeyVulkan::Build(scope, signature);
    EXPECT_EQ(original, RenderPassExecutionKeyVulkan::Build(scope, signature));
    EXPECT_EQ(RenderPassKeyHashVulkan{}(original), RenderPassKeyHashVulkan{}(RenderPassExecutionKeyVulkan::Build(scope, signature)));
    colors[0].Clear.Value[0] = .7f;
    EXPECT_EQ(original, RenderPassExecutionKeyVulkan::Build(scope, signature));
    FramebufferKeyVulkan fb{signature, {10, 20}, 32, 16, 1};
    const auto previous = fb;
    colors[0].Load = LoadAction::Load;
    EXPECT_NE(original, RenderPassExecutionKeyVulkan::Build(scope, signature));
    EXPECT_EQ(fb, previous);
    std::swap(signature.Colors[0], signature.Colors[1]);
    EXPECT_NE(original, RenderPassExecutionKeyVulkan::Build(scope, signature));
    fb.Views[0] = 30;
    EXPECT_NE(fb, previous);
    fb = previous;
    fb.Width = 16;
    EXPECT_NE(fb, previous);
    fb = previous;
    fb.Layers = 2;
    EXPECT_NE(fb, previous);
    fb = previous;
    fb.Signature.Samples = 4;
    EXPECT_NE(fb, previous);
    scope.RenderArea = {1, 1, 10, 10};
    EXPECT_EQ(original.ColorAttachments[1], RenderPassExecutionKeyVulkan::Build(scope, previous.Signature).ColorAttachments[1]);
}
TEST(RenderScopeKeys, DepthWithoutStencilNormalizesUnusedOperations) {
    using namespace vulkan;
    AttachmentSignature signature{{}, TextureFormat::D32_FLOAT, 1};
    RenderPassBeginDescriptor scope{{}, RenderPassDepthStencilAttachment{nullptr}, 8, 8, 1};
    const auto first = RenderPassExecutionKeyVulkan::Build(scope, signature);
    scope.DepthStencil->StencilLoad = LoadAction::Clear;
    scope.DepthStencil->StencilStore = StoreAction::Store;
    EXPECT_EQ(first, RenderPassExecutionKeyVulkan::Build(scope, signature));
    scope.DepthStencil->ReadOnly = true;
    EXPECT_NE(first, RenderPassExecutionKeyVulkan::Build(scope, signature));
}
#endif
class RenderScopeTest : public testing::TestWithParam<RenderBackend> {
protected:
    DeviceContext Context;
    void SetUp() override {
        if (!TryCreateDevice(GetParam(), Context, true)) {
            if (SetupMustFail(Context.Status, RequiredBackend(GetParam()))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
    void TearDown() override { EXPECT_EQ(Context.ValidationErrors.load(), 0u); }
};
TEST_P(RenderScopeTest, RejectsInvalidAttachmentsAndAreasBeforeNativeBegin) {
    auto texture = Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::Resource}).Unwrap();
    auto view = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto sampled = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::Resource}).Unwrap();
    auto allocator = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    array<RenderPassColorAttachment, 2> colors{{{view.get(), LoadAction::Clear}, {view.get(), LoadAction::Load}}};
    RenderPassBeginDescriptor scope{std::span{colors.data(), 1}, {}, 16, 16, 1};
    {
        auto encoder = command->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        EXPECT_FALSE(command->BeginRenderPass(scope));
        command->EndRenderPass(encoder.Release());
    }
    scope.Colors = colors;
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.Colors = std::span{colors.data(), 1};
    colors[0].View = sampled.get();
    EXPECT_FALSE(command->BeginRenderPass(scope));
    colors[0].View = view.get();
    colors[0].Load = static_cast<LoadAction>(-1);
    EXPECT_FALSE(command->BeginRenderPass(scope));
    colors[0].Load = LoadAction::Load;
    scope.Width = 17;
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.Width = 16;
    scope.Layers = 2;
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.Layers = 1;
    scope.RenderArea = Rect{-1, 0, 8, 8};
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.RenderArea = Rect{9, 0, 8, 8};
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.RenderArea = Rect{0, 0, 0, 8};
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.RenderArea.reset();
    scope.Colors = {};
    EXPECT_FALSE(command->BeginRenderPass(scope));
    auto depth = Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::D32_FLOAT, .Usage = TextureUse::DepthStencilRead}).Unwrap();
    auto depthView = Context.Device->CreateTextureView({depth.get(), TextureDimension::Dim2D, TextureFormat::D32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::DepthRead}).Unwrap();
    scope.DepthStencil = RenderPassDepthStencilAttachment{depthView.get(), LoadAction::Load, StoreAction::Store, LoadAction::DontCare, StoreAction::Discard, true};
    {
        auto encoder = command->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        command->EndRenderPass(encoder.Release());
    }
    scope.DepthStencil->DepthLoad = LoadAction::Clear;
    EXPECT_FALSE(command->BeginRenderPass(scope));
    scope.DepthStencil->DepthLoad = LoadAction::Load;
    scope.DepthStencil->DepthStore = StoreAction::Discard;
    EXPECT_FALSE(command->BeginRenderPass(scope));
    command->End();
    allocator->Reset();
}
TEST_P(RenderScopeTest, BeginFailureRecoveryAndClearReadbackAcrossReset) {
    auto texture = Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto view = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto allocator = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
    auto fence = Context.Device->CreateFence().Unwrap();
    const auto pitch = Align(uint64_t{32}, Context.Device->GetDetail().TextureDataPitchAlignment);
    auto readback = Context.Device->CreateBuffer({pitch * 8, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    RenderPassColorAttachment color{view.get(), LoadAction::Clear, StoreAction::Store, {{.25f, .5f, .75f, 1}}};
    const RenderPassBeginDescriptor scope{std::span{&color, 1}, {}, 8, 8, 1};
    EXPECT_FALSE(command->BeginRenderPass(scope));
    for (uint64_t frame = 1; frame <= 4; ++frame) {
        EXPECT_FALSE(command->BeginRenderPass(scope));
        command->Begin();
        EXPECT_FALSE(command->BeginRenderPass({}));
        auto compute = command->BeginComputePass();
        ASSERT_TRUE(compute);
        EXPECT_FALSE(command->BeginRenderPass(scope));
        command->EndComputePass(compute.Release());
        const ResourceBarrierDescriptor before = BarrierTextureDescriptor{texture.get(), frame == 1 ? TextureState::Undefined : TextureState::CopySource, TextureState::RenderTarget};
        command->ResourceBarrier(std::span{&before, 1});
        auto encoder = command->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        EXPECT_FALSE(command->BeginRenderPass(scope));
        command->EndRenderPass(encoder.Release());
        const ResourceBarrierDescriptor after = BarrierTextureDescriptor{texture.get(), TextureState::RenderTarget, TextureState::CopySource};
        command->ResourceBarrier(std::span{&after, 1});
        command->CopyTextureToBuffer(readback.get(), 0, texture.get(), {0, 1, 0, 1});
        command->End();
        EXPECT_FALSE(command->BeginRenderPass(scope));
        CommandBuffer* raw = command.get();
        Fence* f = fence.get();
        Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}, .SignalFences = std::span{&f, 1}, .SignalValues = std::span{&frame, 1}});
        ASSERT_TRUE(fence->WaitFor(frame, 30000));
        readback->InvalidateMappedRange({0, pitch * 8});
        auto* bytes = static_cast<uint8_t*>(readback->Map(0, pitch * 8));
        ASSERT_NE(bytes, nullptr);
        EXPECT_NEAR(bytes[0], 64, 1);
        EXPECT_NEAR(bytes[1], 128, 1);
        EXPECT_NEAR(bytes[2], 191, 1);
        readback->Unmap();
        allocator->Reset();
    }
}
TEST_P(RenderScopeTest, PartialAreaClearPreservesAttachmentContentsOutsideTheArea) {
    auto texture = Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto view = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto allocator = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = Context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
    auto fence = Context.Device->CreateFence().Unwrap();
    const auto pitch = Align(uint64_t{32}, Context.Device->GetDetail().TextureDataPitchAlignment);
    auto readback = Context.Device->CreateBuffer({pitch * 8, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    command->Begin();
    RenderPassColorAttachment color{view.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 1}}};
    auto partialColor = color;
    partialColor.Clear.Value[0] = 1;

    const ResourceBarrierDescriptor before = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&before, 1});
    auto encoder = command->BeginRenderPass({std::span{&color, 1}, {}, 8, 8, 1});
    ASSERT_TRUE(encoder);
    command->EndRenderPass(encoder.Release());
    const ResourceBarrierDescriptor between = BarrierTextureDescriptor{texture.get(), TextureState::RenderTarget, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&between, 1});
    encoder = command->BeginRenderPass({std::span{&partialColor, 1}, {}, 8, 8, 1, Rect{2, 2, 4, 4}});
    ASSERT_TRUE(encoder);
    command->EndRenderPass(encoder.Release());
    const ResourceBarrierDescriptor after = BarrierTextureDescriptor{texture.get(), TextureState::RenderTarget, TextureState::CopySource};
    command->ResourceBarrier(std::span{&after, 1});
    command->CopyTextureToBuffer(readback.get(), 0, texture.get(), {0, 1, 0, 1});
    command->End();
    auto* raw = command.get();
    auto* f = fence.get();
    uint64_t value = 1;
    Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}, .SignalFences = std::span{&f, 1}, .SignalValues = std::span{&value, 1}});
    ASSERT_TRUE(fence->WaitFor(1, 30000));
    readback->InvalidateMappedRange({0, pitch * 8});
    auto* bytes = static_cast<const uint8_t*>(readback->Map(0, pitch * 8));
    ASSERT_NE(bytes, nullptr);
    EXPECT_EQ(bytes[0], 0);
    EXPECT_EQ(bytes[pitch * 3 + 3 * 4], 255);
    EXPECT_EQ(bytes[pitch * 7 + 7 * 4], 0);
    readback->Unmap();
    allocator->Reset();
}
TEST_P(RenderScopeTest, MultipleColorsAndReadOnlyDepthPreserveStoredPixels) {
    auto& device = *Context.Device;
    vector<unique_ptr<Texture>> colors;
    vector<unique_ptr<TextureView>> views;
    for (uint32_t i = 0; i < 2; ++i) {
        colors.push_back(device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap());
        views.push_back(device.CreateTextureView({colors.back().get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap());
    }
    auto depth = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::D32_FLOAT, .Usage = TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite | TextureUse::CopySource}).Unwrap();
    auto depthWrite = device.CreateTextureView({depth.get(), TextureDimension::Dim2D, TextureFormat::D32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::DepthWrite}).Unwrap();
    auto depthRead = device.CreateTextureView({depth.get(), TextureDimension::Dim2D, TextureFormat::D32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::DepthRead}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    auto fence = device.CreateFence().Unwrap();
    const auto pitch = Align(uint64_t{32}, device.GetDetail().TextureDataPitchAlignment);
    const auto imageSize = pitch * 8;
    auto readback = device.CreateBuffer({imageSize * 3, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    array attachments{RenderPassColorAttachment{views[0].get(), LoadAction::Clear, StoreAction::Store, {{1, 0, 0, 1}}},
                      RenderPassColorAttachment{views[1].get(), LoadAction::Clear, StoreAction::Store, {{0, 1, 0, 1}}}};
    command->Begin();

    auto readAttachments = attachments;
    for (auto& color : readAttachments) {
        color.Load = LoadAction::Load;
        color.Clear = {{0, 0, 1, 0}};
    }

    const array<ResourceBarrierDescriptor, 3> initial{
        BarrierTextureDescriptor{colors[0].get(), TextureState::Undefined, TextureState::RenderTarget},
        BarrierTextureDescriptor{colors[1].get(), TextureState::Undefined, TextureState::RenderTarget},
        BarrierTextureDescriptor{depth.get(), TextureState::Undefined, TextureState::DepthWrite}};
    command->ResourceBarrier(initial);
    auto writer = command->BeginRenderPass({attachments, RenderPassDepthStencilAttachment{depthWrite.get(), LoadAction::Clear, StoreAction::Store, LoadAction::DontCare, StoreAction::Discard, false, {.25f, 0}},
                                          8, 8, 1});
    ASSERT_TRUE(writer);
    command->EndRenderPass(writer.Release());
    const ResourceBarrierDescriptor depthToRead = BarrierTextureDescriptor{depth.get(), TextureState::DepthWrite, TextureState::DepthRead};
    command->ResourceBarrier(std::span{&depthToRead, 1});
    auto reader = command->BeginRenderPass({readAttachments, RenderPassDepthStencilAttachment{depthRead.get(), LoadAction::Load, StoreAction::Store, LoadAction::DontCare, StoreAction::Discard, true, {.75f, 0}},
                                          8, 8, 1});
    ASSERT_TRUE(reader);
    command->EndRenderPass(reader.Release());
    const array<ResourceBarrierDescriptor, 3> toCopy{
        BarrierTextureDescriptor{colors[0].get(), TextureState::RenderTarget, TextureState::CopySource},
        BarrierTextureDescriptor{colors[1].get(), TextureState::RenderTarget, TextureState::CopySource},
        BarrierTextureDescriptor{depth.get(), TextureState::DepthRead, TextureState::CopySource}};
    command->ResourceBarrier(toCopy);
    for (uint32_t i = 0; i < 3; ++i) command->CopyTextureToBuffer(readback.get(), imageSize * i, i < 2 ? colors[i].get() : depth.get(), {0, 1, 0, 1});
    command->End();
    auto* raw = command.get();
    auto* signal = fence.get();
    uint64_t value = 1;
    Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}, .SignalFences = std::span{&signal, 1}, .SignalValues = std::span{&value, 1}});
    ASSERT_TRUE(fence->WaitFor(value, 30000));
    readback->InvalidateMappedRange({0, imageSize * 3});
    const auto* bytes = static_cast<const uint8_t*>(readback->Map(0, imageSize * 3));
    ASSERT_NE(bytes, nullptr);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const auto offset = y * pitch + x * 4;
            EXPECT_EQ(bytes[offset], 255);
            EXPECT_EQ(bytes[offset + 1], 0);
            EXPECT_EQ(bytes[imageSize + offset], 0);
            EXPECT_EQ(bytes[imageSize + offset + 1], 255);
            float actualDepth;
            std::memcpy(&actualDepth, bytes + imageSize * 2 + offset, sizeof(actualDepth));
            EXPECT_FLOAT_EQ(actualDepth, .25f);
        }
    readback->Unmap();
    allocator->Reset();
}
TEST_P(RenderScopeTest, ReadOnlyDepthStencilPreservesBothNativePlanes) {
    auto& device = *Context.Device;
    const auto usage = TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite | TextureUse::CopySource;
    const auto support = device.QueryTextureSupport({TextureDimension::Dim2D, TextureFormat::D32_FLOAT_S8_UINT, usage});
    if (!support.SampleCounts.HasFlag(SampleCount::X1)) GTEST_SKIP() << "D32_FLOAT_S8_UINT attachment/readback unsupported";
    auto image = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::D32_FLOAT_S8_UINT, .Usage = usage}).Unwrap();
    auto writeView = device.CreateTextureView({image.get(), TextureDimension::Dim2D, TextureFormat::D32_FLOAT_S8_UINT, {0, 1, 0, 1}, TextureViewUsage::DepthWrite}).Unwrap();
    auto readView = device.CreateTextureView({image.get(), TextureDimension::Dim2D, TextureFormat::D32_FLOAT_S8_UINT, {0, 1, 0, 1}, TextureViewUsage::DepthRead}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    auto fence = device.CreateFence().Unwrap();
    uint64_t depthPitch = Align(uint64_t{32}, device.GetDetail().TextureDataPitchAlignment);
    uint64_t stencilPitch = Align(uint64_t{8}, device.GetDetail().TextureDataPitchAlignment);
    uint64_t stencilOffset = depthPitch * 8;
    uint64_t totalBytes = stencilOffset + stencilPitch * 8;
#ifdef RADRAY_ENABLE_D3D12
    array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    if (GetParam() == RenderBackend::D3D12) {
        const auto* native = static_cast<d3d12::TextureD3D12*>(image.get());
        array<UINT, 2> rows{};
        array<UINT64, 2> rowBytes{};
        static_cast<d3d12::DeviceD3D12*>(&device)->_device->GetCopyableFootprints(&native->_rawDesc, 0, 2, 0, footprints.data(), rows.data(), rowBytes.data(), &totalBytes);
        ASSERT_EQ(rows[0], 8u);
        ASSERT_EQ(rows[1], 8u);
        ASSERT_EQ(rowBytes[0], 32u);
        ASSERT_EQ(rowBytes[1], 8u);
        depthPitch = footprints[0].Footprint.RowPitch;
        stencilPitch = footprints[1].Footprint.RowPitch;
        stencilOffset = footprints[1].Offset;
    }
#endif
    auto readback = device.CreateBuffer({totalBytes, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    command->Begin();

    const ResourceBarrierDescriptor initial = BarrierTextureDescriptor{image.get(), TextureState::Undefined, TextureState::DepthWrite};
    command->ResourceBarrier(std::span{&initial, 1});
    auto writer = command->BeginRenderPass({{}, RenderPassDepthStencilAttachment{writeView.get(), LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store, false, {.25f, 83}}, 8, 8, 1});
    ASSERT_TRUE(writer);
    command->EndRenderPass(writer.Release());
    const ResourceBarrierDescriptor toRead = BarrierTextureDescriptor{image.get(), TextureState::DepthWrite, TextureState::DepthRead};
    command->ResourceBarrier(std::span{&toRead, 1});
    auto reader = command->BeginRenderPass({{}, RenderPassDepthStencilAttachment{readView.get(), LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store, true, {.75f, 0}}, 8, 8, 1});
    ASSERT_TRUE(reader);
    command->EndRenderPass(reader.Release());
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{image.get(), TextureState::DepthRead, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    // Native plane copies provide an independent oracle for the attachment operations.
#ifdef RADRAY_ENABLE_D3D12
    if (GetParam() == RenderBackend::D3D12) {
        auto* native = static_cast<d3d12::CmdListD3D12*>(command.get());
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = static_cast<d3d12::TextureD3D12*>(image.get())->_tex.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = plane;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = static_cast<d3d12::BufferD3D12*>(readback.get())->_buf.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprints[plane];
            native->_cmdList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    }
#endif
#ifdef RADRAY_ENABLE_VULKAN
    if (GetParam() == RenderBackend::Vulkan) {
        const array<VkBufferImageCopy, 2> copies{{{.bufferRowLength = static_cast<uint32_t>(depthPitch / 4), .bufferImageHeight = 8, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}},
                                                  {.bufferOffset = stencilOffset, .bufferRowLength = static_cast<uint32_t>(stencilPitch), .bufferImageHeight = 8, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, .imageExtent = {8, 8, 1}}}};
        auto* native = static_cast<vulkan::CommandBufferVulkan*>(command.get());
        static_cast<vulkan::DeviceVulkan*>(&device)->_ftb.vkCmdCopyImageToBuffer(native->_cmdBuffer, static_cast<vulkan::ImageVulkan*>(image.get())->_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<vulkan::BufferVulkan*>(readback.get())->_buffer, 2, copies.data());
    }
#endif
    const ResourceBarrierDescriptor toHost = BarrierBufferDescriptor{readback.get(), BufferState::CopyDestination, BufferState::HostRead};
    command->ResourceBarrier(std::span{&toHost, 1});
    command->End();
    auto* raw = command.get();
    auto* signal = fence.get();
    uint64_t value = 1;
    Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}, .SignalFences = std::span{&signal, 1}, .SignalValues = std::span{&value, 1}});
    ASSERT_TRUE(fence->WaitFor(value, 30000));
    readback->InvalidateMappedRange({0, totalBytes});
    const auto* bytes = static_cast<const uint8_t*>(readback->Map(0, totalBytes));
    ASSERT_NE(bytes, nullptr);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            float actualDepth;
            std::memcpy(&actualDepth, bytes + y * depthPitch + x * 4, sizeof(actualDepth));
            EXPECT_FLOAT_EQ(actualDepth, .25f);
            EXPECT_EQ(bytes[stencilOffset + y * stencilPitch + x], 83);
        }
    readback->Unmap();
    allocator->Reset();
}
TEST_P(RenderScopeTest, MultisampleResolvePreservesClearThroughCompatibleReload) {
    auto& device = *Context.Device;
    const auto support = device.QueryTextureSupport({TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, TextureUse::RenderTarget | TextureUse::CopySource});
    ASSERT_TRUE(support.SampleCounts.HasFlag(SampleCount::X4));
    auto multisample = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 4, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto single = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::CopyDestination | TextureUse::CopySource}).Unwrap();
    auto view = device.CreateTextureView({multisample.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    auto fence = device.CreateFence().Unwrap();
    const auto pitch = Align(uint64_t{32}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({pitch * 8, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    command->Begin();
    RenderPassColorAttachment color{view.get(), LoadAction::Clear, StoreAction::Store, {{0, 1, 1, 1}}};

    auto reloadColor = color;
    reloadColor.Load = LoadAction::Load;
    reloadColor.Clear = {{1, 0, 0, 1}};

    const ResourceBarrierDescriptor initial = BarrierTextureDescriptor{multisample.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&initial, 1});
    auto writer = command->BeginRenderPass({std::span{&color, 1}, {}, 8, 8, 1});
    ASSERT_TRUE(writer);
    command->EndRenderPass(writer.Release());
    auto preserve = command->BeginRenderPass({std::span{&reloadColor, 1}, {}, 8, 8, 1});
    ASSERT_TRUE(preserve);
    command->EndRenderPass(preserve.Release());
    const array<ResourceBarrierDescriptor, 2> beforeResolve{
        BarrierTextureDescriptor{multisample.get(), TextureState::RenderTarget, TextureState::ResolveSource},
        BarrierTextureDescriptor{single.get(), TextureState::Undefined, TextureState::ResolveDestination}};
    command->ResourceBarrier(beforeResolve);
    command->ResolveTexture({.Destination = single.get(), .Source = multisample.get(), .ArrayLayerCount = 1});
    const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{single.get(), TextureState::ResolveDestination, TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, single.get(), {0, 1, 0, 1});
    command->End();
    auto* raw = command.get();
    auto* signal = fence.get();
    uint64_t value = 1;
    Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}, .SignalFences = std::span{&signal, 1}, .SignalValues = std::span{&value, 1}});
    ASSERT_TRUE(fence->WaitFor(value, 30000));
    readback->InvalidateMappedRange({0, pitch * 8});
    const auto* bytes = static_cast<const uint8_t*>(readback->Map(0, pitch * 8));
    ASSERT_NE(bytes, nullptr);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const auto offset = y * pitch + x * 4;
            EXPECT_EQ(bytes[offset], 0);
            EXPECT_EQ(bytes[offset + 1], 255);
            EXPECT_EQ(bytes[offset + 2], 255);
            EXPECT_EQ(bytes[offset + 3], 255);
        }
    readback->Unmap();
    allocator->Reset();
}
#ifdef RADRAY_ENABLE_VULKAN
TEST_P(RenderScopeTest, VulkanNativeReusePinsBudgetAndLocalViewInvalidation) {
    if (GetParam() != RenderBackend::Vulkan) GTEST_SKIP() << "Vulkan native cache";
    using namespace vulkan;
    auto* device = static_cast<DeviceVulkan*>(Context.Device.get());
    device->_renderPassCache->SetBudget({2, 2, 2, 2});
    auto texture = Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget}).Unwrap();
    auto view = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    const auto id = CastVkObject(view.get())->_viewId;
    auto a = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto b = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
    auto ca = Context.Device->CreateCommandBuffer(a.get()).Unwrap();
    auto cb = Context.Device->CreateCommandBuffer(b.get()).Unwrap();
    auto* va = CastVkObject(ca.get());
    auto* vb = CastVkObject(cb.get());
    RenderPassColorAttachment color{view.get(), LoadAction::Clear, StoreAction::Store};
    const RenderPassBeginDescriptor scope{std::span{&color, 1}, {}, 16, 16, 1};
    ca->Begin();
    cb->Begin();
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    {
        auto encoder = cb->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        cb->EndRenderPass(encoder.Release());
    }
    auto* pass = va->_passPins.front().Get();
    auto* fb = va->_framebufferPins.front().Get();
    EXPECT_EQ(pass, vb->_passPins.front().Get());
    EXPECT_EQ(fb, vb->_framebufferPins.front().Get());
    EXPECT_EQ(fb->Uses, 2u);
    EXPECT_DEATH(view.reset(), "");
    EXPECT_DEATH(device->_renderPassCache->Clear(), "");
    color.Clear.Value[0] = .5f;
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    EXPECT_EQ(va->_passPins.size(), 1u);
    EXPECT_EQ(fb->Uses, 2u);
    color.Load = LoadAction::Load;
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    EXPECT_EQ(va->_passPins.size(), 2u);
    EXPECT_EQ(va->_framebufferPins.size(), 1u);
    color.Store = StoreAction::Discard;
    EXPECT_FALSE(ca->BeginRenderPass(scope));
    ca->End();
    cb->End();
    a->Reset();
    EXPECT_EQ(fb->Uses, 1u);
    EXPECT_DEATH(view.reset(), "");
    b->Reset();
    EXPECT_EQ(fb->Uses, 0u);
    color.Load = LoadAction::Clear;
    color.Store = StoreAction::Store;
    ca->Begin();
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    EXPECT_EQ(pass, va->_passPins.front().Get());
    EXPECT_EQ(fb, va->_framebufferPins.front().Get());
    ca->End();
    a->Reset();
    device->_renderPassCache->SetBudget({2, 0, 2, 0});
    ca->Begin();
    EXPECT_FALSE(ca->BeginRenderPass(scope));
    EXPECT_TRUE(va->_passPins.empty());
    EXPECT_TRUE(va->_framebufferPins.empty());
    ca->End();
    a->Reset();
    device->_renderPassCache->SetBudget({2, 2, 2, 2});
    view.reset();
    view = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    EXPECT_NE(CastVkObject(view.get())->_viewId, id);
    color.View = view.get();
    ca->Begin();
    const auto createFramebuffer = device->_ftb.vkCreateFramebuffer;
    device->_ftb.vkCreateFramebuffer = [](VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer* output) -> VkResult {
        *output = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    };
    EXPECT_FALSE(ca->BeginRenderPass(scope));
    device->_ftb.vkCreateFramebuffer = createFramebuffer;
    EXPECT_TRUE(va->_passPins.empty());
    EXPECT_TRUE(va->_framebufferPins.empty());
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    ca->End();
    a->Reset();
    device->_renderPassCache->SetBudget({2, 3, 2, 3});
    auto unrelated = Context.Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    ca->Begin();
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    auto smaller = scope;
    smaller.Width = smaller.Height = 8;
    {
        auto encoder = ca->BeginRenderPass(smaller);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    EXPECT_EQ(va->_framebufferPins.size(), 2u);
    EXPECT_NE(va->_framebufferPins[0].Get(), va->_framebufferPins[1].Get());
    color.View = unrelated.get();
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    auto* unrelatedFramebuffer = va->_framebufferPins.back().Get();
    ca->End();
    a->Reset();
    view.reset();
    ca->Begin();
    {
        auto encoder = ca->BeginRenderPass(scope);
        ASSERT_TRUE(encoder);
        ca->EndRenderPass(encoder.Release());
    }
    EXPECT_EQ(va->_framebufferPins.front().Get(), unrelatedFramebuffer);
    ca->End();
    a->Reset();
}
#endif
INSTANTIATE_TEST_SUITE_P(Backends, RenderScopeTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));
}  // namespace radray::render::test
