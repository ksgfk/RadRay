#include "gpu_test_fixture.h"
#include <filesystem>
#include <radray/file.h>

#ifdef RADRAY_ENABLE_D3D12
namespace radray::render::d3d12 {
namespace {

class D3D12CommandContracts : public testing::Test {
protected:
    test::DeviceContext Context;
    shared_ptr<Device> Device;

    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::D3D12, Context, true, false, {1, 1, 1})) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
        Device = render::detail::CreateValidationDevice(Context.Device, {.IsEnabled = true, .LogCallback = test::CaptureRhiValidationMessage, .LogUserData = &Context}).Unwrap();
    }
    void TearDown() override {
        Device.reset();
        Context.Reset();
    }
    unique_ptr<Shader> Stage(ShaderStage stage) {
        const auto name = stage == ShaderStage::Vertex ? "command_contracts.VSMain.dxil.bin" : "command_contracts.PSMain.dxil.bin";
        const auto bytes = ReadBinaryFile(std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_bytecode" / name);
        if (!bytes) return {};
        return Device->CreateShader({*bytes, ShaderBlobCategory::DXIL, stage}).Unwrap();
    }
    void DrawAndRead(bool independent, bool constantBlend);
};

void D3D12CommandContracts::DrawAndRead(bool independent, bool constantBlend) {
    auto vs = Stage(ShaderStage::Vertex), ps = Stage(ShaderStage::Pixel);
    ASSERT_TRUE(vs && ps);
    auto layout = Device->CreatePipelineLayout({}).Unwrap();
    const VertexBufferLayout streams[]{{0, 8, VertexStepMode::Vertex}, {1, 12, VertexStepMode::Vertex}, {2, 16, VertexStepMode::Vertex}};
    const VertexAttribute attributes[]{{1, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0},
                                       {0, 0, "TEXCOORD", 0, VertexFormat::FLOAT32X2, 1},
                                       {2, 0, "COLOR", 0, VertexFormat::FLOAT32X4, 2}};
    ColorTargetState colors[]{ColorTargetState::Default(TextureFormat::RGBA8_UNORM), ColorTargetState::Default(TextureFormat::RGBA8_UNORM)};
    if (independent) {
        colors[0].WriteMask = ColorWrite::Red;
        colors[1].Blend = BlendState::Default();
        colors[1].Blend->Color.Src = BlendFactor::Zero;
        colors[1].Blend->Color.Dst = BlendFactor::One;
    }
    if (constantBlend) {
        for (auto& color : colors) {
            color.Blend = BlendState::Default();
            color.Blend->Color.Src = BlendFactor::Constant;
        }
    }
    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    auto pipeline = Device->CreateGraphicsPipelineState({.PipelineLayout = layout.get(), .VS = ShaderEntry{vs.get(), "VSMain"}, .PS = ShaderEntry{ps.get(), "PSMain"}, .VertexInput = {streams, attributes}, .Primitive = primitive, .MultiSample = MultiSampleState::Default(), .ColorTargets = colors}).Unwrap();
    const float positions[]{-1, -1, 0, 3, -1, 0, -1, 3, 0};
    const float offsets[6]{};
    const float values[]{0.25f, 0.5f, 0.75f, 1, 0.25f, 0.5f, 0.75f, 1, 0.25f, 0.5f, 0.75f, 1};
    auto position = test::MakeUploadBuffer(*Device, std::as_bytes(std::span{positions}), BufferUse::Vertex).Unwrap();
    auto offset = test::MakeUploadBuffer(*Device, std::as_bytes(std::span{offsets}), BufferUse::Vertex).Unwrap();
    auto value = test::MakeUploadBuffer(*Device, std::as_bytes(std::span{values}), BufferUse::Vertex).Unwrap();
    auto first = test::MakeRenderTarget(Device.get(), TextureFormat::RGBA8_UNORM, 8, 8, TextureUse::RenderTarget | TextureUse::CopySource);
    auto second = test::MakeRenderTarget(Device.get(), TextureFormat::RGBA8_UNORM, 8, 8, TextureUse::RenderTarget | TextureUse::CopySource);
    ASSERT_TRUE(first && second);
    auto readback = Device->CreateBuffer({.Size = 4096, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
    auto queue = Device->GetCommandQueue(QueueType::Direct).Unwrap();
    auto allocator = Device->CreateCommandAllocator(queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor before[]{BarrierTextureDescriptor{first->Tex.get(), TextureState::Undefined, TextureState::RenderTarget}, BarrierTextureDescriptor{second->Tex.get(), TextureState::Undefined, TextureState::RenderTarget}};
    command->ResourceBarrier(before);
    const RenderPassColorAttachment attachments[]{{first->View.get(), LoadAction::Clear, StoreAction::Store, {}}, {second->View.get(), LoadAction::Clear, StoreAction::Store, {{0, 0.5f, 0.25f, 1}}}};
    auto encoder = command->BeginRenderPass({.Colors = attachments, .Width = 8, .Height = 8}).Unwrap();
    encoder->BindGraphicsPipelineState(pipeline.get());
    encoder->SetViewport({0, 0, 8, 8, 0, 1});
    encoder->SetScissor({0, 0, 8, 8});
    const VertexBufferBinding bindings[]{{1, {position.get(), 0, sizeof(positions)}}, {2, {value.get(), 0, sizeof(values)}}, {0, {offset.get(), 0, sizeof(offsets)}}};
    encoder->BindVertexBuffers(bindings);
    encoder->Draw(3, 1, 0, 0);
    command->EndRenderPass(std::move(encoder));
    const ResourceBarrierDescriptor after[]{BarrierTextureDescriptor{first->Tex.get(), TextureState::RenderTarget, TextureState::CopySource}, BarrierTextureDescriptor{second->Tex.get(), TextureState::RenderTarget, TextureState::CopySource}};
    command->ResourceBarrier(after);
    command->CopyTextureToBuffer(readback.get(), 0, first->Tex.get(), {0, 1, 0, 1});
    command->CopyTextureToBuffer(readback.get(), 2048, second->Tex.get(), {0, 1, 0, 1});
    command->End();
    test::GpuWorkGuard work{Context};
    CommandBuffer* lists[]{command.get()};
    queue->Submit({.CmdBuffers = lists});
    queue->Wait();
    const auto* pixels = static_cast<const uint8_t*>(readback->Map(0, 4096));
    ASSERT_NE(pixels, nullptr);
    const uint32_t center = 4 * 256 + 4 * 4;
    const array<int, 4> expectedFirst = constantBlend ? array<int, 4>{0, 0, 0, 255} : independent ? array<int, 4>{64, 0, 0, 0}
                                                                                                  : array<int, 4>{64, 128, 191, 255};
    const array<int, 4> expectedSecond = independent ? array<int, 4>{0, 128, 64, 255} : expectedFirst;
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(pixels[center + i], expectedFirst[i], 1);
        EXPECT_NEAR(pixels[2048 + center + i], expectedSecond[i], 1);
    }
    readback->Unmap();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_F(D3D12CommandContracts, IndependentBlendAndWriteMasksReachEachRenderTarget) { DrawAndRead(true, false); }
TEST_F(D3D12CommandContracts, ConstantBlendUsesZero) { DrawAndRead(false, true); }

TEST(D3D12StateMappings, EveryColorWriteSubsetIsRepresentable) {
    for (uint32_t bits = 0; bits <= uint32_t(ColorWrite::All); ++bits) {
        const auto mapped = MapColorWrites(static_cast<ColorWrite>(bits));
        ASSERT_TRUE(mapped) << bits;
        EXPECT_EQ(static_cast<uint32_t>(*mapped), bits);
    }
    EXPECT_FALSE(MapColorWrites(static_cast<ColorWrite>(16)));
}

TEST_F(D3D12CommandContracts, ComputeShaderReadBarriersUseSupportedStates) {
    auto buffer = Device->CreateBuffer({.Size = 64, .Usage = BufferUse::Resource | BufferUse::CopyDestination}).Unwrap();
    auto texture = Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::Resource}).Unwrap();
    auto queue = Device->GetCommandQueue(QueueType::Compute).Unwrap();
    auto allocator = Device->CreateCommandAllocator(queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barriers[]{BarrierBufferDescriptor{buffer.get(), BufferState::Common, BufferState::ShaderRead}, BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::ShaderRead}};
    command->ResourceBarrier(barriers);
    command->End();
    test::GpuWorkGuard work{Context};
    CommandBuffer* lists[]{command.get()};
    queue->Submit({.CmdBuffers = lists});
    queue->Wait();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_F(D3D12CommandContracts, DepthStencilCopiesPreserveBothPlanesAcrossMipsAndLayers) {
    for (const auto format : {TextureFormat::D16_UNORM, TextureFormat::D24_UNORM_S8_UINT, TextureFormat::D32_FLOAT, TextureFormat::D32_FLOAT_S8_UINT}) {
        SCOPED_TRACE(fmt::format("{}", format));
        auto texture = Device->CreateTexture({.Dim = TextureDimension::Dim2DArray, .Width = 3, .Height = 3, .DepthOrArraySize = 2, .MipLevels = 2, .SampleCount = 1, .Format = format, .Usage = TextureUse::DepthStencilWrite | TextureUse::CopySource | TextureUse::CopyDestination}).Unwrap();
        const uint32_t planeCount = GetTextureFormatAspects(format).HasFlag(TextureAspect::Stencil) ? 2 : 1;
        const auto nativeDesc = CD3DX12_RESOURCE_DESC::Tex2D(FormatToTypeless(MapType(format)), 3, 3, 2, 2, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        array<uint32_t, 2> pitches{};
        for (uint32_t plane = 0; plane < planeCount; ++plane) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            uint64_t total{}, row{};
            static_cast<DeviceD3D12*>(Context.Device.get())->_device->GetCopyableFootprints(&nativeDesc, plane * 4, 1, 512, &fp, nullptr, &row, &total);
            pitches[plane] = fp.Footprint.RowPitch;
            EXPECT_EQ(row, 3u * (plane ? 1u : format == TextureFormat::D16_UNORM ? 2u
                                                                                 : 4u));
        }
        array<vector<byte>, 2> data;
        array<unique_ptr<Buffer>, 2> uploads, readbacks;
        array<vector<uint64_t>, 2> starts;
        for (uint32_t plane = 0; plane < planeCount; ++plane) {
            const uint32_t bpp = plane ? 1 : format == TextureFormat::D16_UNORM ? 2
                                                                                : 4;
            uint64_t end = 512;
            for (uint32_t mip = 0; mip < 2; ++mip) {
                const uint32_t width = std::max(1u, 3u >> mip);
                for (uint32_t layer = 0; layer < 2; ++layer) {
                    const auto start = (end + 511) / 512 * 512;
                    starts[plane].push_back(start);
                    end = start + pitches[plane] * (width - 1) + width * bpp;
                    data[plane].resize(end);
                    for (uint32_t y = 0; y < width; ++y) {
                        for (uint32_t x = 0; x < width; ++x) {
                            const auto at = start + y * pitches[plane] + x * bpp;
                            const auto value = 1 + mip + layer + y + x;
                            if (plane) {
                                data[plane][at] = static_cast<byte>(17 + value);
                            } else if (format == TextureFormat::D32_FLOAT || format == TextureFormat::D32_FLOAT_S8_UINT) {
                                const float depth = float(value) / 16;
                                std::memcpy(data[plane].data() + at, &depth, 4);
                            } else {
                                const uint32_t depth = value * 4096;
                                std::memcpy(data[plane].data() + at, &depth, bpp);
                            }
                        }
                    }
                }
            }
            uploads[plane] = test::MakeUploadBuffer(*Device, data[plane], BufferUse::CopySource).Unwrap();
            readbacks[plane] = Device->CreateBuffer({.Size = data[plane].size(), .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
        }
        auto queue = Device->GetCommandQueue(QueueType::Direct).Unwrap();
        auto allocator = Device->CreateCommandAllocator(queue).Unwrap();
        auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
        command->Begin();
        const ResourceBarrierDescriptor toDst = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::CopyDestination};
        command->ResourceBarrier(std::span{&toDst, 1});
        const auto errors = Context.RhiValidationErrors.load();
        if (planeCount == 2) {
            command->CopyBufferToTexture(texture.get(), {0, 2, 0, 2}, uploads[0].get(), 512);
            EXPECT_EQ(Context.RhiValidationErrors.load(), errors + 1);
        }
        for (uint32_t plane = 0; plane < planeCount; ++plane)
            command->CopyBufferToTexture(texture.get(), {0, 2, 0, 2, plane ? TextureAspect::Stencil : TextureAspect::Depth}, uploads[plane].get(), 512);
        const ResourceBarrierDescriptor toSrc = BarrierTextureDescriptor{texture.get(), TextureState::CopyDestination, TextureState::CopySource};
        command->ResourceBarrier(std::span{&toSrc, 1});
        for (uint32_t plane = 0; plane < planeCount; ++plane)
            command->CopyTextureToBuffer(readbacks[plane].get(), 512, texture.get(), {0, 2, 0, 2, plane ? TextureAspect::Stencil : TextureAspect::Depth});
        command->End();
        test::GpuWorkGuard work{Context};
        CommandBuffer* lists[]{command.get()};
        queue->Submit({.CmdBuffers = lists});
        queue->Wait();
        for (uint32_t plane = 0; plane < planeCount; ++plane) {
            const auto* bytes = static_cast<const byte*>(readbacks[plane]->Map(0, data[plane].size()));
            ASSERT_NE(bytes, nullptr);
            const uint32_t bpp = plane ? 1 : format == TextureFormat::D16_UNORM ? 2
                                                                                : 4;
            for (uint32_t mip = 0; mip < 2; ++mip) {
                const uint32_t width = std::max(1u, 3u >> mip);
                for (uint32_t layer = 0; layer < 2; ++layer) {
                    const auto start = starts[plane][mip * 2 + layer];
                    for (uint32_t y = 0; y < width; ++y)
                        EXPECT_EQ(std::memcmp(bytes + start + y * pitches[plane], data[plane].data() + start + y * pitches[plane], width * bpp), 0);
                }
            }
            readbacks[plane]->Unmap();
        }
        EXPECT_EQ(Context.RhiValidationErrors.load(), errors + (planeCount == 2 ? 1 : 0));
    }
}

TEST_F(D3D12CommandContracts, InvalidQueueOperationsAreRejectedBeforeNativeRecording) {
    auto buffer = Device->CreateBuffer({.Size = 64, .Usage = BufferUse::Resource | BufferUse::CopyDestination | BufferUse::UnorderedAccess}).Unwrap();
    auto pool = Device->CreateQueryPool({.Count = 1}).Unwrap();
    auto copyAllocator = Device->CreateCommandAllocator(Device->GetCommandQueue(QueueType::Copy).Unwrap()).Unwrap();
    auto copy = Device->CreateCommandBuffer(copyAllocator.get()).Unwrap();
    copy->Begin();
    copy->WriteTimestamp({pool.get(), QueryPipelineStage::Bottom, 0});
    copy->ResolveQueryData({.Pool = pool.get(), .Count = 1, .Destination = buffer.get()});
    const ResourceBarrierDescriptor shader = BarrierBufferDescriptor{buffer.get(), BufferState::Common, BufferState::ShaderRead};
    copy->ResourceBarrier(std::span{&shader, 1});
    const ResourceBarrierDescriptor uav = BarrierUavDescriptor{buffer.get()};
    copy->ResourceBarrier(std::span{&uav, 1});
    copy->End();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 4u);

    auto allocator = Device->CreateCommandAllocator(Device->GetCommandQueue(QueueType::Compute).Unwrap()).Unwrap();
    auto compute = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    compute->Begin();
    const ResourceBarrierDescriptor pixel = BarrierBufferDescriptor{buffer.get(), BufferState::Common, BufferState::ShaderRead, BufferRange::AllRange(), {}, ShaderStage::Pixel};
    compute->ResourceBarrier(std::span{&pixel, 1});
    const ResourceBarrierDescriptor mixed = BarrierBufferDescriptor{buffer.get(), BufferState::Common, BufferState::ShaderRead | BufferState::UnorderedAccess};
    compute->ResourceBarrier(std::span{&mixed, 1});
    compute->End();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 6u);
}

TEST_F(D3D12CommandContracts, HostVisibleBuffersRespectTheirNativeHeapStates) {
    const auto bytes = ReadBinaryFile(std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_bytecode/compute.CSMain.dxil.bin");
    ASSERT_TRUE(bytes);
    auto shader = Device->CreateShader({*bytes, ShaderBlobCategory::DXIL, ShaderStage::Compute}).Unwrap();
    const BindingDescriptor binding{.BindingId = 0, .Kind = BindingKind::RWStructuredBuffer, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{BindingRegisterClass::UAV, 2, 0, D3D12BufferPlacement::Table}};
    auto group = Device->CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    const BindingDescriptor srvBinding{.BindingId = 0, .Kind = BindingKind::StructuredBuffer, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{BindingRegisterClass::SRV, 0, 0, D3D12BufferPlacement::Table}};
    auto srvGroup = Device->CreateBindingGroupLayout({std::span{&srvBinding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor slot{0, group.get()};
    auto layout = Device->CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
    auto pipeline = Device->CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}}).Unwrap();
    auto queue = Device->GetCommandQueue(QueueType::Direct).Unwrap();
    for (const auto memory : {MemoryType::Upload, MemoryType::ReadBack}) {
        const auto host = memory == MemoryType::Upload ? BufferState::HostWrite : BufferState::HostRead;
        const auto map = memory == MemoryType::Upload ? BufferUse::MapWrite : BufferUse::MapRead;
        auto buffer = Device->CreateBuffer({.Size = 256, .Memory = memory, .Usage = map | BufferUse::UnorderedAccess | BufferUse::Resource | BufferUse::CopySource}).Unwrap();
        auto readback = Device->CreateBuffer({.Size = 4, .Memory = MemoryType::ReadBack, .Usage = BufferUse::MapRead | BufferUse::CopyDestination}).Unwrap();
        auto parameters = Device->CreateShaderParameterSet({group.get()}).Unwrap();
        ASSERT_TRUE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, 4}, 4}));
        ASSERT_TRUE(parameters->FlushWrites());
        auto srvParameters = Device->CreateShaderParameterSet({srvGroup.get()}).Unwrap();
        ASSERT_TRUE(srvParameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, 4}, 4}));
        ASSERT_TRUE(srvParameters->FlushWrites());
        auto allocator = Device->CreateCommandAllocator(queue).Unwrap();
        auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
        command->Begin();
        const auto transition = [&](BufferState before, BufferState after) {
            const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{buffer.get(), before, after};
            command->ResourceBarrier(std::span{&barrier, 1});
        };
        transition(host, BufferState::UnorderedAccess);
        auto encoder = command->BeginComputePass().Unwrap();
        encoder->BindComputePipelineState(pipeline.get());
        encoder->BindShaderParameterSet(0, parameters.get(), {});
        encoder->Dispatch(1, 1, 1);
        command->EndComputePass(std::move(encoder));
        transition(BufferState::UnorderedAccess, host);
        transition(host, BufferState::CopySource);
        command->CopyBufferToBuffer(readback.get(), 0, buffer.get(), 0, 4);
        transition(BufferState::CopySource, host);
        command->End();
        test::GpuWorkGuard work{Context};
        CommandBuffer* commands[]{command.get()};
        queue->Submit({.CmdBuffers = commands});
        queue->Wait();
        auto* result = static_cast<const uint32_t*>(readback->Map(0, 4));
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(*result, 0x12345678u);
        readback->Unmap();
        if (memory == MemoryType::ReadBack) {
            result = static_cast<const uint32_t*>(buffer->Map(0, 4));
            ASSERT_NE(result, nullptr);
            EXPECT_EQ(*result, 0x12345678u);
            buffer->Unmap();
        }
    }
    auto upload = Device->CreateBuffer({.Size = 64, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CopySource | BufferUse::Resource}).Unwrap();
    auto allocator = Device->CreateCommandAllocator(queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barriers[]{BarrierBufferDescriptor{upload.get(), BufferState::HostWrite, BufferState::CopySource}, BarrierBufferDescriptor{upload.get(), BufferState::CopySource, BufferState::ShaderRead}};
    command->ResourceBarrier(barriers);
    command->End();
    test::GpuWorkGuard work{Context};
    CommandBuffer* commands[]{command.get()};
    queue->Submit({.CmdBuffers = commands});
    queue->Wait();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_F(D3D12CommandContracts, InvalidHeapUsageAndBufferViewsAreRejectedBeforeNativeCalls) {
    EXPECT_FALSE(Device->CreateBuffer({.Size = 64, .Memory = MemoryType::Upload, .Usage = BufferUse::CopyDestination}));
    EXPECT_FALSE(Device->CreateBuffer({.Size = 64, .Memory = MemoryType::ReadBack, .Usage = BufferUse::Resource}));
    auto buffer = Device->CreateBuffer({.Size = 8192, .Usage = BufferUse::Resource | BufferUse::UnorderedAccess}).Unwrap();
    for (const auto kind : {BindingKind::StructuredBuffer, BindingKind::RawBuffer, BindingKind::TypedBuffer}) {
        const BindingDescriptor binding{.BindingId = 0, .Kind = kind, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(kind), 0, 0, D3D12BufferPlacement::Table}};
        auto group = Device->CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
        auto parameters = Device->CreateShaderParameterSet({group.get()}).Unwrap();
        if (kind == BindingKind::TypedBuffer) {
            EXPECT_FALSE(parameters->Set(0, 0, ShaderTexelBufferBinding{buffer.get(), {0, 16}, TextureFormat::RGBA8_UNORM_SRGB}));
            ASSERT_TRUE(parameters->Set(0, 0, ShaderTexelBufferBinding{buffer.get(), {0, 16}, TextureFormat::R32_UINT}));
        } else {
            EXPECT_FALSE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, 8192}, kind == BindingKind::RawBuffer ? 4u : 4096u}));
            ASSERT_TRUE(parameters->Set(0, 0, ShaderBufferBinding{buffer.get(), {0, 8192}, kind == BindingKind::RawBuffer ? 0u : 2048u}));
        }
        ASSERT_TRUE(parameters->FlushWrites());
    }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 5u);
}

TEST_F(D3D12CommandContracts, InvalidTextureCopyAndResolveAreRejectedBeforeNativeRecording) {
    auto texture = Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::CopySource | TextureUse::CopyDestination}).Unwrap();
    auto source = Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 4, .Format = TextureFormat::R32_UINT, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto destination = Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::R32_UINT, .Usage = TextureUse::CopyDestination}).Unwrap();
    for (const auto queue : {QueueType::Direct, QueueType::Compute, QueueType::Copy}) {
        auto allocator = Device->CreateCommandAllocator(Device->GetCommandQueue(queue).Unwrap()).Unwrap();
        auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
        command->Begin();
        if (queue == QueueType::Direct)
            command->CopyTextureToTexture({.Destination = texture.get(), .DestinationX = 2, .Source = texture.get(), .Width = 2, .Height = 4});
        command->ResolveTexture({.Destination = destination.get(), .Source = source.get()});
        command->End();
    }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 4u);
}

}  // namespace
}  // namespace radray::render::d3d12
#endif
