#include "render_test_framework.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace radray::render::test {
namespace {

constexpr std::string_view kIndirectDispatchShader = R"(
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> gOutput : register(u0, space0);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    gOutput[tid.x] = 100 + tid.x;
}
)";

constexpr std::string_view kIndirectDrawShader = R"(
struct DrawSettings {
    float XOffset;
    float3 Padding;
};

[[vk::push_constant]] ConstantBuffer<DrawSettings> gSettings : register(b0, space0);

struct VertexOutput {
    float4 Position : SV_Position;
    nointerpolation float4 Color : COLOR0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(0.0, -1.0), float2(-1.0, 1.0),
        float2(-1.0, 1.0), float2(0.0, -1.0), float2(0.0, 1.0)
    };
    VertexOutput output;
    float2 position = positions[vertexId];
    position.x += gSettings.XOffset;
    output.Position = float4(position, 0.0, 1.0);
    output.Color = gSettings.XOffset < 0.5
        ? float4(1.0, 0.0, 0.0, 1.0)
        : float4(0.0, 1.0, 0.0, 1.0);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    return input.Color;
}
)";

struct DrawSettings {
    float XOffset{0.0f};
    float Padding[3]{};
};
static_assert(sizeof(DrawSettings) == 16);

struct GraphicsProgram {
    vector<byte> VertexBlob{};
    vector<byte> PixelBlob{};
    unique_ptr<Shader> VertexShader{};
    unique_ptr<Shader> PixelShader{};
    unique_ptr<PipelineLayout> Layout{};
    unique_ptr<GraphicsPipelineState> Pipeline{};
};

std::optional<unique_ptr<Shader>> CompileShader(
    ComputeTestContext& context,
    std::string_view source,
    std::string_view entryPoint,
    ShaderStage stage,
    vector<byte>& blob,
    string* reason) {
    DxcCompileParams params{};
    params.Code = source;
    params.EntryPoint = entryPoint;
    params.Stage = stage;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = context.GetBackend() == TestBackend::Vulkan;
    auto outputOpt = context.GetDxc()->Compile(params);
    if (!outputOpt.has_value()) {
        *reason = fmt::format("DXC compile failed for {}", entryPoint);
        return std::nullopt;
    }
    auto output = std::move(outputOpt.value());

    ShaderReflectionDesc reflection{};
    ShaderBlobCategory category = ShaderBlobCategory::DXIL;
    if (context.GetBackend() == TestBackend::D3D12) {
        auto reflectionOpt = context.GetDxc()->GetShaderDescFromOutput(output.Refl);
        if (!reflectionOpt.has_value()) {
            *reason = fmt::format("DXIL reflection failed for {}", entryPoint);
            return std::nullopt;
        }
        reflection = std::move(reflectionOpt.value());
    } else {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        auto reflectionOpt = ReflectSpirv(SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = entryPoint,
            .Stage = stage});
        if (!reflectionOpt.has_value()) {
            *reason = fmt::format("SPIR-V reflection failed for {}", entryPoint);
            return std::nullopt;
        }
        reflection = std::move(reflectionOpt.value());
        category = ShaderBlobCategory::SPIRV;
#else
        *reason = "SPIR-V Cross reflection is not enabled";
        return std::nullopt;
#endif
    }

    blob = std::move(output.Data);
    auto shaderOpt = context.GetDevicePtr()->CreateShader(ShaderDescriptor{
        .Source = std::span<const byte>{blob.data(), blob.size()},
        .Category = category,
        .Stages = stage,
        .Reflection = std::move(reflection)});
    if (!shaderOpt.HasValue()) {
        *reason = fmt::format("CreateShader failed for {}", entryPoint);
        return std::nullopt;
    }
    return shaderOpt.Release();
}

std::optional<GraphicsProgram> CreateGraphicsProgram(
    ComputeTestContext& context,
    RenderPass* compatiblePass,
    string* reason) {
    GraphicsProgram program{};
    auto vsOpt = CompileShader(
        context, kIndirectDrawShader, "VSMain", ShaderStage::Vertex, program.VertexBlob, reason);
    if (!vsOpt.has_value()) {
        return std::nullopt;
    }
    program.VertexShader = std::move(vsOpt.value());
    auto psOpt = CompileShader(
        context, kIndirectDrawShader, "PSMain", ShaderStage::Pixel, program.PixelBlob, reason);
    if (!psOpt.has_value()) {
        return std::nullopt;
    }
    program.PixelShader = std::move(psOpt.value());

    Shader* shaders[] = {program.VertexShader.get(), program.PixelShader.get()};
    const PushConstantBinding pushConstant{.Group = 0, .Binding = 0};
    auto layoutOpt = context.GetDevicePtr()->CreatePipelineLayout(PipelineLayoutDescriptor{
        .Shaders = shaders,
        .PushConstantBindings = std::span{&pushConstant, 1}});
    if (!layoutOpt.HasValue()) {
        *reason = "CreatePipelineLayout failed for indirect draw program";
        return std::nullopt;
    }
    program.Layout = layoutOpt.Release();

    PrimitiveState primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    const ColorTargetState colorTarget = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    auto pipelineOpt = context.GetDevicePtr()->CreateGraphicsPipelineState(
        GraphicsPipelineStateDescriptor{
            .PipelineLayout = program.Layout.get(),
            .VS = ShaderEntry{program.VertexShader.get(), "VSMain"},
            .PS = ShaderEntry{program.PixelShader.get(), "PSMain"},
            .VertexLayouts = {},
            .Primitive = primitive,
            .DepthStencil = std::nullopt,
            .MultiSample = MultiSampleState::Default(),
            .ColorTargets = std::span{&colorTarget, 1},
            .CompatibleRenderPass = compatiblePass});
    if (!pipelineOpt.HasValue()) {
        *reason = "CreateGraphicsPipelineState failed for indirect draw program";
        return std::nullopt;
    }
    program.Pipeline = pipelineOpt.Release();
    return program;
}

class RhiCommandRuntimeTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        if (GetParam() == TestBackend::Vulkan && _vulkanUnavailableReason.has_value()) {
            GTEST_SKIP() << _vulkanUnavailableReason.value();
        }
        string reason{};
        if (!_context.Initialize(GetParam(), &reason)) {
            if (GetParam() == TestBackend::Vulkan) {
                _vulkanUnavailableReason = reason;
                GTEST_SKIP() << reason;
            }
            FAIL() << reason;
        }
        _context.ClearCapturedErrors();
    }

    void TearDown() override {
        _context.Reset();
    }

    unique_ptr<Buffer> CreateBuffer(const BufferDescriptor& desc) {
        string reason{};
        auto result = _context.CreateBuffer(desc, &reason);
        EXPECT_TRUE(result.HasValue()) << reason;
        return result.HasValue() ? result.Release() : nullptr;
    }

    unique_ptr<Texture> CreateTexture(const TextureDescriptor& desc) {
        string reason{};
        auto result = _context.CreateTexture(desc, &reason);
        EXPECT_TRUE(result.HasValue()) << reason;
        return result.HasValue() ? result.Release() : nullptr;
    }

    unique_ptr<TextureView> CreateTextureView(const TextureViewDescriptor& desc) {
        string reason{};
        auto result = _context.CreateTextureView(desc, &reason);
        EXPECT_TRUE(result.HasValue()) << reason;
        return result.HasValue() ? result.Release() : nullptr;
    }

    unique_ptr<CommandBuffer> CreateCommandBuffer() {
        string reason{};
        auto result = _context.CreateCommandBuffer(&reason);
        EXPECT_TRUE(result.HasValue()) << reason;
        return result.HasValue() ? result.Release() : nullptr;
    }

    uint64_t TextureReadbackPitch(uint32_t width) const {
        return AlignUp<uint64_t>(
            static_cast<uint64_t>(width) * 4,
            std::max<uint64_t>(1, _context.GetDeviceDetail().TextureDataPitchAlignment));
    }

    unique_ptr<Buffer> CreateTextureReadback(uint32_t width, uint32_t height) {
        return CreateBuffer(BufferDescriptor{
            .Size = TextureReadbackPitch(width) * height,
            .Memory = MemoryType::ReadBack,
            .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
            .Hints = ResourceHint::None});
    }

    void RecordTextureReadback(
        CommandBuffer* command,
        Texture* texture,
        TextureStates before,
        Buffer* readback) {
        vector<ResourceBarrierDescriptor> barriers{};
        barriers.push_back(BarrierTextureDescriptor{
            .Target = texture,
            .Before = before,
            .After = TextureState::CopySource});
        _context.AppendReadbackPreCopyBarrier(barriers, readback);
        command->ResourceBarrier(barriers);
        command->CopyTextureToBuffer(
            readback, 0, texture, SubresourceRange{0, 1, 0, 1});
        barriers.clear();
        _context.AppendReadbackPostCopyBarrier(barriers, readback);
        if (!barriers.empty()) {
            command->ResourceBarrier(barriers);
        }
    }

    std::optional<vector<byte>> ReadTextureData(
        Buffer* readback,
        uint32_t width,
        uint32_t height) {
        string reason{};
        auto data = _context.ReadHostVisibleBuffer(
            readback, TextureReadbackPitch(width) * height, &reason);
        EXPECT_TRUE(data.has_value()) << reason;
        return data;
    }

    std::array<uint8_t, 4> ReadPixel(
        std::span<const byte> data,
        uint32_t width,
        uint32_t x,
        uint32_t y) const {
        const uint64_t offset = y * TextureReadbackPitch(width) + x * 4;
        return {
            std::to_integer<uint8_t>(data[offset]),
            std::to_integer<uint8_t>(data[offset + 1]),
            std::to_integer<uint8_t>(data[offset + 2]),
            std::to_integer<uint8_t>(data[offset + 3])};
    }

    ComputeTestContext _context{};
    inline static std::optional<string> _vulkanUnavailableReason{};
};

TEST_P(RhiCommandRuntimeTest, FlushMappedRangesAcceptsAllForPersistentAndTransientMaps) {
    auto persistent = CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
        .Hints = ResourceHint::PersistentMap});
    auto transient = CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource});
    ASSERT_NE(persistent, nullptr);
    ASSERT_NE(transient, nullptr);

    auto writeAndFlush = [this](Buffer* buffer) {
        auto* mapped = static_cast<byte*>(buffer->Map(0, 64));
        ASSERT_NE(mapped, nullptr);
        std::memset(mapped + 8, 0x5a, 56);
        const MappedBufferRange range{
            .Target = buffer,
            .Range = BufferRange{.Offset = 8, .Size = BufferRange::All()}};
        _context.GetDevicePtr()->FlushMappedRanges(std::span{&range, 1});
        buffer->Unmap();
    };

    writeAndFlush(persistent.get());
    writeAndFlush(transient.get());
}

TEST_P(RhiCommandRuntimeTest, D3D12FlushMappedRangesAreNoOps) {
    if (GetParam() != TestBackend::D3D12) {
        GTEST_SKIP() << "D3D12-specific behavior";
    }

    auto readback = CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination});
    ASSERT_NE(readback, nullptr);

    const MappedBufferRange invalidRanges[] = {
        MappedBufferRange{},
        MappedBufferRange{
            .Target = readback.get(),
            .Range = BufferRange{.Offset = 64, .Size = 1}}};
    _context.GetDevicePtr()->FlushMappedRanges(invalidRanges);
    readback->FlushMappedRange(BufferRange{.Offset = 64, .Size = 1});
}

TEST_P(RhiCommandRuntimeTest, VulkanFlushMappedRangesRejectsInvalidSpanEntries) {
    if (GetParam() != TestBackend::Vulkan) {
        GTEST_SKIP() << "Vulkan-specific behavior";
    }

    auto writable = CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
        .Hints = ResourceHint::PersistentMap});
    auto readback = CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination});
    ASSERT_NE(writable, nullptr);
    ASSERT_NE(readback, nullptr);
    ASSERT_NE(writable->Map(0, 64), nullptr);

    const MappedBufferRange nullTarget{};
    EXPECT_DEATH(
        _context.GetDevicePtr()->FlushMappedRanges(std::span{&nullTarget, 1}),
        "");

    const MappedBufferRange wrongUsage{
        .Target = readback.get(),
        .Range = BufferRange{.Offset = 0, .Size = 0}};
    EXPECT_DEATH(
        _context.GetDevicePtr()->FlushMappedRanges(std::span{&wrongUsage, 1}),
        "");

    const MappedBufferRange outOfBounds{
        .Target = writable.get(),
        .Range = BufferRange{.Offset = 60, .Size = 8}};
    EXPECT_DEATH(
        _context.GetDevicePtr()->FlushMappedRanges(std::span{&outOfBounds, 1}),
        "");

    const MappedBufferRange mixed[] = {
        MappedBufferRange{
            .Target = writable.get(),
            .Range = BufferRange{.Offset = 0, .Size = 4}},
        nullTarget};
    EXPECT_DEATH(
        _context.GetDevicePtr()->FlushMappedRanges(mixed),
        "");

    ComputeTestContext otherContext;
    string reason;
    ASSERT_TRUE(otherContext.Initialize(GetParam(), &reason)) << reason;
    auto otherBufferOpt = otherContext.CreateBuffer(
        BufferDescriptor{
            .Size = 64,
            .Memory = MemoryType::Upload,
            .Usage = BufferUse::MapWrite,
            .Hints = ResourceHint::PersistentMap},
        &reason);
    ASSERT_TRUE(otherBufferOpt.HasValue()) << reason;
    auto otherBuffer = otherBufferOpt.Release();
    const MappedBufferRange crossDevice{
        .Target = otherBuffer.get(),
        .Range = BufferRange{.Offset = 0, .Size = 4}};
    EXPECT_DEATH(
        _context.GetDevicePtr()->FlushMappedRanges(std::span{&crossDevice, 1}),
        "");

    writable->Unmap();
    otherBuffer.reset();
    otherContext.Reset();
}

TEST_P(RhiCommandRuntimeTest, TextureToTextureCopyPreservesTexels) {
    constexpr uint32_t width = 4;
    constexpr uint32_t height = 4;
    const TextureDescriptor sourceDesc{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::CopyDestination | TextureUse::CopySource | TextureUse::Resource,
        .Hints = ResourceHint::None};
    auto source = CreateTexture(sourceDesc);
    auto destination = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::CopyDestination | TextureUse::CopySource,
        .Hints = ResourceHint::None});
    auto readback = CreateTextureReadback(width, height);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);
    ASSERT_NE(readback, nullptr);

    vector<byte> expected(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (y * width + x) * 4;
            expected[offset] = static_cast<byte>(10 + x);
            expected[offset + 1] = static_cast<byte>(20 + y);
            expected[offset + 2] = static_cast<byte>(30 + x + y);
            expected[offset + 3] = static_cast<byte>(255);
        }
    }
    string reason{};
    ASSERT_TRUE(_context.UploadTexture2D(source.get(), expected, &reason)) << reason;

    auto command = CreateCommandBuffer();
    ASSERT_NE(command, nullptr);
    command->Begin();
    ResourceBarrierDescriptor beforeCopy[] = {
        BarrierTextureDescriptor{
            .Target = source.get(),
            .Before = TextureState::ShaderRead,
            .After = TextureState::CopySource},
        BarrierTextureDescriptor{
            .Target = destination.get(),
            .Before = TextureState::Undefined,
            .After = TextureState::CopyDestination}};
    command->ResourceBarrier(beforeCopy);
    command->CopyTextureToTexture(TextureCopyDescriptor{
        .Destination = destination.get(),
        .DestinationMipLevel = 0,
        .DestinationArrayLayer = 0,
        .DestinationX = 0,
        .DestinationY = 0,
        .DestinationZ = 0,
        .Source = source.get(),
        .SourceMipLevel = 0,
        .SourceArrayLayer = 0,
        .SourceX = 0,
        .SourceY = 0,
        .SourceZ = 0,
        .Width = width,
        .Height = height,
        .Depth = 1,
        .ArrayLayerCount = 1});
    RecordTextureReadback(
        command.get(), destination.get(), TextureState::CopyDestination, readback.get());
    command->End();
    ASSERT_TRUE(_context.SubmitAndWait(command.get(), &reason)) << reason;

    auto actualOpt = ReadTextureData(readback.get(), width, height);
    ASSERT_TRUE(actualOpt.has_value());
    const auto& actual = actualOpt.value();
    const uint64_t pitch = TextureReadbackPitch(width);
    for (uint32_t y = 0; y < height; ++y) {
        EXPECT_TRUE(std::equal(
            actual.data() + y * pitch,
            actual.data() + y * pitch + width * 4,
            expected.data() + y * width * 4));
    }
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();
}

TEST_P(RhiCommandRuntimeTest, DispatchIndirectUsesGpuArguments) {
    string reason{};
    auto programOpt = _context.CreateComputeProgram(
        kIndirectDispatchShader, "CSMain", false, &reason);
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    constexpr uint32_t valueCount = 4;
    auto output = CreateBuffer(BufferDescriptor{
        .Size = valueCount * sizeof(uint32_t),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource,
        .Hints = ResourceHint::None});
    auto readback = CreateBuffer(BufferDescriptor{
        .Size = valueCount * sizeof(uint32_t),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
        .Hints = ResourceHint::None});
    auto arguments = CreateBuffer(BufferDescriptor{
        .Size = sizeof(DispatchIndirectArguments),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::Indirect | BufferUse::CopyDestination,
        .Hints = ResourceHint::None});
    ASSERT_NE(output, nullptr);
    ASSERT_NE(readback, nullptr);
    ASSERT_NE(arguments, nullptr);

    const DispatchIndirectArguments dispatchArgs{valueCount, 1, 1};
    ASSERT_TRUE(_context.UploadBufferData(
        arguments.get(), BytesOf(dispatchArgs), BufferState::Indirect, &reason)) << reason;

    auto poolOpt = _context.CreateDescriptorPool(DescriptorPoolDescriptor{
        .MaxBindingGroups = 1,
        .MaxStorageBuffers = 1}, &reason);
    ASSERT_TRUE(poolOpt.HasValue()) << reason;
    auto pool = poolOpt.Release();
    auto groupOpt = _context.CreateBindingGroup(
        pool.get(), program.PipelineLayout, 0, &reason);
    ASSERT_TRUE(groupOpt.HasValue()) << reason;
    auto group = groupOpt.Release();
    ASSERT_TRUE(group->SetResource(0, BufferBindingDescriptor{
        .Target = output.get(),
        .Range = BufferRange{0, valueCount * sizeof(uint32_t)},
        .Stride = sizeof(uint32_t),
        .Usage = BufferViewUsage::ReadWriteStorage}));

    auto command = CreateCommandBuffer();
    ASSERT_NE(command, nullptr);
    command->Begin();
    ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = BufferState::Common,
        .After = BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto encoderOpt = command->BeginComputePass();
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->BindComputePipelineState(program.PipelineObject.get());
    encoder->BindBindingGroup(0, group.get());
    encoder->DispatchIndirect(arguments.get(), 0);
    command->EndComputePass(std::move(encoder));

    vector<ResourceBarrierDescriptor> readbackBarriers{};
    readbackBarriers.push_back(BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource});
    _context.AppendReadbackPreCopyBarrier(readbackBarriers, readback.get());
    command->ResourceBarrier(readbackBarriers);
    command->CopyBufferToBuffer(
        readback.get(), 0, output.get(), 0, valueCount * sizeof(uint32_t));
    readbackBarriers.clear();
    _context.AppendReadbackPostCopyBarrier(readbackBarriers, readback.get());
    if (!readbackBarriers.empty()) {
        command->ResourceBarrier(readbackBarriers);
    }
    command->End();
    ASSERT_TRUE(_context.SubmitAndWait(command.get(), &reason)) << reason;

    auto bytesOpt = _context.ReadHostVisibleBuffer(
        readback.get(), valueCount * sizeof(uint32_t), &reason);
    ASSERT_TRUE(bytesOpt.has_value()) << reason;
    std::array<uint32_t, valueCount> values{};
    std::memcpy(values.data(), bytesOpt->data(), bytesOpt->size());
    EXPECT_EQ(values, (std::array<uint32_t, valueCount>{100, 101, 102, 103}));
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();
}

TEST_P(RhiCommandRuntimeTest, DrawAndDrawIndexedIndirectRenderExpectedHalves) {
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 4;
    string reason{};
    const RenderPassColorAttachmentDescriptor attachment{
        .Format = TextureFormat::RGBA8_UNORM,
        .SampleCount = 1,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store};
    auto passOpt = _context.GetDevicePtr()->CreateRenderPass(RenderPassDescriptor{
        .ColorAttachments = std::span{&attachment, 1}});
    ASSERT_TRUE(passOpt.HasValue());
    auto pass = passOpt.Release();
    auto programOpt = CreateGraphicsProgram(_context, pass.get(), &reason);
    ASSERT_TRUE(programOpt.has_value()) << reason;
    auto program = std::move(programOpt.value());

    auto target = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::RenderTarget | TextureUse::CopySource,
        .Hints = ResourceHint::None});
    ASSERT_NE(target, nullptr);
    auto targetView = CreateTextureView(TextureViewDescriptor{
        .Target = target.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::RenderTarget});
    ASSERT_NE(targetView, nullptr);
    TextureView* colorViews[] = {targetView.get()};
    auto framebufferOpt = _context.GetDevicePtr()->CreateFramebuffer(FramebufferDescriptor{
        .Pass = pass.get(),
        .ColorAttachments = colorViews,
        .DepthStencilAttachment = nullptr,
        .Width = width,
        .Height = height,
        .Layers = 1});
    ASSERT_TRUE(framebufferOpt.HasValue());
    auto framebuffer = framebufferOpt.Release();

    auto drawArguments = CreateBuffer(BufferDescriptor{
        .Size = 2 * sizeof(DrawIndirectArguments),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::Indirect | BufferUse::CopyDestination,
        .Hints = ResourceHint::None});
    auto indexedArguments = CreateBuffer(BufferDescriptor{
        .Size = 2 * sizeof(DrawIndexedIndirectArguments),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::Indirect | BufferUse::CopyDestination,
        .Hints = ResourceHint::None});
    constexpr std::array<uint32_t, 6> indices{0, 1, 2, 3, 4, 5};
    auto indexBuffer = CreateBuffer(BufferDescriptor{
        .Size = sizeof(indices),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::Index | BufferUse::CopyDestination,
        .Hints = ResourceHint::None});
    auto readback = CreateTextureReadback(width, height);
    ASSERT_NE(drawArguments, nullptr);
    ASSERT_NE(indexedArguments, nullptr);
    ASSERT_NE(indexBuffer, nullptr);
    ASSERT_NE(readback, nullptr);

    const std::array<DrawIndirectArguments, 2> drawArgs{{
        {0, 1, 0, 0},
        {6, 1, 0, 0}}};
    const std::array<DrawIndexedIndirectArguments, 2> indexedArgs{{
        {0, 1, 0, 0, 0},
        {6, 1, 0, 0, 0}}};
    ASSERT_TRUE(_context.UploadBufferData(
        drawArguments.get(), BytesOf(drawArgs), BufferState::Indirect, &reason)) << reason;
    ASSERT_TRUE(_context.UploadBufferData(
        indexedArguments.get(), BytesOf(indexedArgs), BufferState::Indirect, &reason)) << reason;
    ASSERT_TRUE(_context.UploadBufferData(
        indexBuffer.get(), BytesOf(std::span<const uint32_t>{indices}), BufferState::Index, &reason)) << reason;

    auto command = CreateCommandBuffer();
    ASSERT_NE(command, nullptr);
    command->Begin();
    ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
        .Target = target.get(),
        .Before = TextureState::Undefined,
        .After = TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRenderTarget, 1});
    const ColorClearValue clear{{0.0f, 0.0f, 0.0f, 1.0f}};
    auto encoderOpt = command->BeginRenderPass(RenderPassBeginDescriptor{
        .Pass = pass.get(),
        .Target = framebuffer.get(),
        .ColorClearValues = std::span{&clear, 1},
        .Name = "indirect draw test"});
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    encoder->SetViewport(Viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1});
    encoder->SetScissor(Rect{0, 0, width, height});
    encoder->BindGraphicsPipelineState(program.Pipeline.get());
    const DrawSettings settings{};
    ASSERT_TRUE(encoder->SetPushConstants(
        program.Layout.get(), 0, 0, BytesOf(settings)));
    encoder->DrawIndirect(drawArguments.get(), 0, 2);
    encoder->BindIndexBuffer(IndexBufferView{indexBuffer.get(), 0, sizeof(uint32_t)});
    const DrawSettings indexedSettings{.XOffset = 1.0f};
    ASSERT_TRUE(encoder->SetPushConstants(
        program.Layout.get(), 0, 0, BytesOf(indexedSettings)));
    encoder->DrawIndexedIndirect(indexedArguments.get(), 0, 2);
    command->EndRenderPass(std::move(encoder));
    RecordTextureReadback(command.get(), target.get(), TextureState::RenderTarget, readback.get());
    command->End();
    ASSERT_TRUE(_context.SubmitAndWait(command.get(), &reason)) << reason;

    auto dataOpt = ReadTextureData(readback.get(), width, height);
    ASSERT_TRUE(dataOpt.has_value());
    const auto left = ReadPixel(*dataOpt, width, 1, 1);
    const auto right = ReadPixel(*dataOpt, width, 6, 1);
    EXPECT_EQ(left, (std::array<uint8_t, 4>{255, 0, 0, 255}));
    EXPECT_EQ(right, (std::array<uint8_t, 4>{0, 255, 0, 255}));
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();
}

TEST_P(RhiCommandRuntimeTest, ResolveTextureConvertsMsaaColorToSingleSample) {
    constexpr uint32_t width = 4;
    constexpr uint32_t height = 4;
    string reason{};
    auto command = CreateCommandBuffer();
    ASSERT_NE(command, nullptr);
    auto multisampled = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 4,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::RenderTarget | TextureUse::CopySource,
        .Hints = ResourceHint::None});
    auto resolved = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::CopyDestination | TextureUse::CopySource,
        .Hints = ResourceHint::None});
    auto readback = CreateTextureReadback(width, height);
    ASSERT_NE(multisampled, nullptr);
    ASSERT_NE(resolved, nullptr);
    ASSERT_NE(readback, nullptr);

    auto msaaView = CreateTextureView(TextureViewDescriptor{
        .Target = multisampled.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::RenderTarget});
    ASSERT_NE(msaaView, nullptr);
    const RenderPassColorAttachmentDescriptor attachment{
        .Format = TextureFormat::RGBA8_UNORM,
        .SampleCount = 4,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store};
    auto passOpt = _context.GetDevicePtr()->CreateRenderPass(RenderPassDescriptor{
        .ColorAttachments = std::span{&attachment, 1}});
    ASSERT_TRUE(passOpt.HasValue());
    auto pass = passOpt.Release();
    TextureView* colorViews[] = {msaaView.get()};
    auto framebufferOpt = _context.GetDevicePtr()->CreateFramebuffer(FramebufferDescriptor{
        .Pass = pass.get(),
        .ColorAttachments = colorViews,
        .DepthStencilAttachment = nullptr,
        .Width = width,
        .Height = height,
        .Layers = 1});
    ASSERT_TRUE(framebufferOpt.HasValue());
    auto framebuffer = framebufferOpt.Release();

    command->Begin();
    ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
        .Target = multisampled.get(),
        .Before = TextureState::Undefined,
        .After = TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRenderTarget, 1});
    const ColorClearValue clear{{0.25f, 0.5f, 0.75f, 1.0f}};
    auto encoderOpt = command->BeginRenderPass(RenderPassBeginDescriptor{
        .Pass = pass.get(),
        .Target = framebuffer.get(),
        .ColorClearValues = std::span{&clear, 1},
        .Name = "resolve test"});
    ASSERT_TRUE(encoderOpt.HasValue());
    auto encoder = encoderOpt.Release();
    command->EndRenderPass(std::move(encoder));
    ResourceBarrierDescriptor resolveBarriers[] = {
        BarrierTextureDescriptor{
            .Target = multisampled.get(),
            .Before = TextureState::RenderTarget,
            .After = TextureState::ResolveSource},
        BarrierTextureDescriptor{
            .Target = resolved.get(),
            .Before = TextureState::Undefined,
            .After = TextureState::ResolveDestination}};
    command->ResourceBarrier(resolveBarriers);
    command->ResolveTexture(TextureResolveDescriptor{
        .Destination = resolved.get(),
        .DestinationMipLevel = 0,
        .DestinationArrayLayer = 0,
        .Source = multisampled.get(),
        .SourceMipLevel = 0,
        .SourceArrayLayer = 0,
        .ArrayLayerCount = 1});
    RecordTextureReadback(
        command.get(), resolved.get(), TextureState::ResolveDestination, readback.get());
    command->End();
    ASSERT_TRUE(_context.SubmitAndWait(command.get(), &reason)) << reason;

    auto dataOpt = ReadTextureData(readback.get(), width, height);
    ASSERT_TRUE(dataOpt.has_value());
    const auto pixel = ReadPixel(*dataOpt, width, 2, 2);
    EXPECT_NEAR(pixel[0], 64, 1);
    EXPECT_NEAR(pixel[1], 128, 1);
    EXPECT_NEAR(pixel[2], 191, 1);
    EXPECT_EQ(pixel[3], 255);
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();
}

TEST_P(RhiCommandRuntimeTest, TextureViewAllArrayLayersResolveToTargetExtent) {
    auto texture = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2DArray,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 3,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
        .Hints = ResourceHint::None});
    ASSERT_NE(texture, nullptr);

    auto view = CreateTextureView(TextureViewDescriptor{
        .Target = texture.get(),
        .Dim = TextureDimension::Dim2DArray,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange::AllSub(),
        .Usage = TextureViewUsage::Resource});
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();
}

TEST_P(RhiCommandRuntimeTest, TextureViewSupportsCompatibleFormatOverride) {
    auto texture = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
        .Hints = ResourceHint::None});
    ASSERT_NE(texture, nullptr);

    auto view = CreateTextureView(TextureViewDescriptor{
        .Target = texture.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = TextureFormat::RGBA8_UNORM_SRGB,
        .Range = SubresourceRange::AllSub(),
        .Usage = TextureViewUsage::Resource});
    ASSERT_NE(view, nullptr);
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();
}

TEST_P(RhiCommandRuntimeTest, TextureViewRejectsUnknownUsage) {
    auto texture = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
        .Hints = ResourceHint::None});
    ASSERT_NE(texture, nullptr);

    _context.ClearCapturedErrors();
    auto view = _context.GetDevicePtr()->CreateTextureView(TextureViewDescriptor{
        .Target = texture.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange::AllSub(),
        .Usage = TextureViewUsage::UNKNOWN});
    EXPECT_FALSE(view.HasValue());
    EXPECT_FALSE(_context.GetCapturedErrors().empty());
}

TEST_P(RhiCommandRuntimeTest, TextureViewRejectsNonPortableLayerRanges) {
    auto cubeArray = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::CubeArray,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 12,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
        .Hints = ResourceHint::None});
    auto texture3D = CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim3D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 4,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
        .Hints = ResourceHint::None});
    ASSERT_NE(cubeArray, nullptr);
    ASSERT_NE(texture3D, nullptr);

    auto cubeArrayView = CreateTextureView(TextureViewDescriptor{
        .Target = cubeArray.get(),
        .Dim = TextureDimension::CubeArray,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange::AllSub(),
        .Usage = TextureViewUsage::Resource});
    auto texture3DView = CreateTextureView(TextureViewDescriptor{
        .Target = texture3D.get(),
        .Dim = TextureDimension::Dim3D,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange::AllSub(),
        .Usage = TextureViewUsage::Resource});
    ASSERT_NE(cubeArrayView, nullptr);
    ASSERT_NE(texture3DView, nullptr);
    EXPECT_TRUE(_context.GetCapturedErrors().empty()) << _context.JoinCapturedErrors();

    _context.ClearCapturedErrors();
    auto secondCubeView = _context.GetDevicePtr()->CreateTextureView(TextureViewDescriptor{
        .Target = cubeArray.get(),
        .Dim = TextureDimension::Cube,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange{6, 6, 0, 1},
        .Usage = TextureViewUsage::Resource});
    EXPECT_FALSE(secondCubeView.HasValue());
    EXPECT_FALSE(_context.GetCapturedErrors().empty());

    _context.ClearCapturedErrors();
    auto sliced3DView = _context.GetDevicePtr()->CreateTextureView(TextureViewDescriptor{
        .Target = texture3D.get(),
        .Dim = TextureDimension::Dim3D,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange{1, 1, 0, 1},
        .Usage = TextureViewUsage::Resource});
    EXPECT_FALSE(sliced3DView.HasValue());
    EXPECT_FALSE(_context.GetCapturedErrors().empty());
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    RhiCommandRuntimeTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
