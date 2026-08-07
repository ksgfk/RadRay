#include "gpu_test_fixture.h"

#include <radray/dynamic_library.h>
#include <radray/shader/shader_artifact.h>
#include <radray/utility.h>

#include <gtest/gtest.h>

#include <dxc/dxcapi.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

namespace radray::render {
namespace {

using Microsoft::WRL::ComPtr;

using shader::DecodeDxilShaderArtifact;
using shader::DecodeSpirvShaderArtifact;
using shader::DxilShaderArtifactView;
using shader::ShaderArtifactDecodeError;
using shader::ShaderArtifactDecodeOptions;
using shader::SpirvShaderArtifactView;

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr TextureFormat kFormat = TextureFormat::RGBA8_UNORM;

constexpr std::string_view kShaderSource = R"hlsl(
struct VSInput {
    float3 Position : POSITION;
};

struct VSOutput {
    float4 Position : SV_Position;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.Position = float4(input.Position, 1.0f);
    return output;
}

float4 PSMain() : SV_Target0 {
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
)hlsl";

std::optional<vector<byte>> CompileWithStockDxc(
    DynamicLibrary& library,
    std::string_view entryPoint,
    std::string_view profile,
    bool spirv) {
    using DxcCreateInstanceFunction = decltype(&DxcCreateInstance);
    const DxcCreateInstanceFunction createInstance =
        library.GetFunction<DxcCreateInstanceFunction>("DxcCreateInstance");
    if (createInstance == nullptr) {
        return std::nullopt;
    }

    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        return std::nullopt;
    }

    const std::wstring wideEntry{entryPoint.begin(), entryPoint.end()};
    const std::wstring wideProfile{profile.begin(), profile.end()};
    vector<LPCWSTR> arguments{
        L"-E",
        wideEntry.c_str(),
        L"-T",
        wideProfile.c_str(),
        L"-HV",
        L"2021"};
    if (spirv) {
        arguments.push_back(L"-spirv");
        arguments.push_back(L"-fspv-target-env=vulkan1.2");
    }

    const DxcBuffer source{
        .Ptr = kShaderSource.data(),
        .Size = kShaderSource.size(),
        .Encoding = DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(
                  &source,
                  arguments.data(),
                  static_cast<uint32_t>(arguments.size()),
                  nullptr,
                  IID_PPV_ARGS(&result)))) {
        return std::nullopt;
    }

    HRESULT status = E_FAIL;
    if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
        return std::nullopt;
    }

    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) ||
        object == nullptr) {
        return std::nullopt;
    }

    const auto* data = static_cast<const byte*>(object->GetBufferPointer());
    return vector<byte>{data, data + object->GetBufferSize()};
}

vector<byte> MakeLayoutArtifact(
    std::span<const byte> bytecode,
    shader::ShaderTarget target) {
    constexpr std::string_view entryName = "main";
    constexpr std::string_view bindingName = "ColorTexture";
    shader::WireMetadataEnvelope envelope{};
    envelope.Target = static_cast<uint8_t>(target);
    envelope.StageMask = 1u << static_cast<uint8_t>(shader::ShaderStage::Vertex);
    envelope.EntryRecords = {sizeof(shader::WireMetadataEnvelope), sizeof(shader::WireEntryRecord)};
    envelope.BindingRecords = {envelope.EntryRecords.End(), sizeof(shader::WireBindingRecord)};
    const uint32_t nameOffset = envelope.BindingRecords.End();
    envelope.Bytecode = {
        nameOffset + static_cast<uint32_t>(entryName.size() + bindingName.size()),
        static_cast<uint32_t>(bytecode.size())};
    envelope.TotalSize = envelope.Bytecode.End();

    shader::WireEntryRecord entry{};
    entry.Name = {nameOffset, static_cast<uint32_t>(entryName.size())};
    entry.Stage = static_cast<uint8_t>(shader::ShaderStage::Vertex);
    entry.InterfaceSize = static_cast<uint32_t>(bytecode.size());

    shader::WireBindingRecord binding{};
    binding.Name = {nameOffset + static_cast<uint32_t>(entryName.size()), static_cast<uint32_t>(bindingName.size())};
    binding.Group = 0;
    binding.Binding = 3;
    binding.Type = 4;
    binding.Count = 1;
    binding.StageMask = envelope.StageMask;

    vector<byte> result(envelope.TotalSize);
    std::memcpy(result.data(), &envelope, sizeof(envelope));
    std::memcpy(result.data() + envelope.EntryRecords.Offset, &entry, sizeof(entry));
    std::memcpy(result.data() + envelope.BindingRecords.Offset, &binding, sizeof(binding));
    std::memcpy(result.data() + entry.Name.Offset, entryName.data(), entryName.size());
    std::memcpy(result.data() + binding.Name.Offset, bindingName.data(), bindingName.size());
    std::memcpy(result.data() + envelope.Bytecode.Offset, bytecode.data(), bytecode.size());
    return result;
}

template <typename DeviceType, typename ArtifactType>
concept CanCreatePipelineLayout = requires(DeviceType& device, const ArtifactType& artifact) {
    device.CreatePipelineLayout(artifact);
};

#if defined(RADRAY_ENABLE_D3D12) && defined(RADRAY_ENABLE_VULKAN)
static_assert(CanCreatePipelineLayout<d3d12::DeviceD3D12, DxilShaderArtifactView>);
static_assert(CanCreatePipelineLayout<vulkan::DeviceVulkan, SpirvShaderArtifactView>);
static_assert(!CanCreatePipelineLayout<d3d12::DeviceD3D12, SpirvShaderArtifactView>);
static_assert(!CanCreatePipelineLayout<vulkan::DeviceVulkan, DxilShaderArtifactView>);
#endif

Nullable<unique_ptr<PipelineLayout>> CreateBackendPipelineLayout(
    Device& device,
    const DxilShaderArtifactView& artifact) {
    if (device.GetBackend() != RenderBackend::D3D12) {
        return nullptr;
    }
#if defined(RADRAY_ENABLE_D3D12)
    return static_cast<d3d12::DeviceD3D12&>(device).CreatePipelineLayout(artifact);
#else
    return nullptr;
#endif
}

Nullable<unique_ptr<PipelineLayout>> CreateBackendPipelineLayout(
    Device& device,
    const SpirvShaderArtifactView& artifact) {
    if (device.GetBackend() != RenderBackend::Vulkan) {
        return nullptr;
    }
#if defined(RADRAY_ENABLE_VULKAN)
    return static_cast<vulkan::DeviceVulkan&>(device).CreatePipelineLayout(artifact);
#else
    return nullptr;
#endif
}

Nullable<unique_ptr<PipelineLayout>> CreateBackendPipelineLayout(
    Device& device,
    const std::variant<DxilShaderArtifactView, SpirvShaderArtifactView>& artifact) {
    return std::visit(
        [&](const auto& typedArtifact) {
            return CreateBackendPipelineLayout(device, typedArtifact);
        },
        artifact);
}

void RunPsoSmoke(test::DeviceContext& context, RenderBackend backend) {
    Device& device = *context.Device;
    DynamicLibrary compilerLibrary{"dxcompiler"};
    ASSERT_TRUE(compilerLibrary.IsValid())
        << "RADRAY_BUILD_SHADER_COMPILER is enabled but dxcompiler is missing from the test output directory";

    const bool spirv = backend == RenderBackend::Vulkan;
    const auto vertexBytecode = CompileWithStockDxc(compilerLibrary, "VSMain", "vs_6_0", spirv);
    const auto pixelBytecode = CompileWithStockDxc(compilerLibrary, "PSMain", "ps_6_0", spirv);
    ASSERT_TRUE(vertexBytecode.has_value()) << "stock DXC failed to compile the vertex shader";
    ASSERT_TRUE(pixelBytecode.has_value()) << "stock DXC failed to compile the pixel shader";

    const auto layoutBlob = MakeLayoutArtifact(
        *vertexBytecode,
        spirv ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL);
    shader::GpuArtifactHash expectedGpuArtifact{};
    ShaderArtifactDecodeOptions decodeOptions{
        .Target = spirv ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL,
        .ExpectedGpuArtifact = expectedGpuArtifact};
    ShaderArtifactDecodeError decodeError = ShaderArtifactDecodeError::None;
    std::optional<std::variant<DxilShaderArtifactView, SpirvShaderArtifactView>> artifact;
    if (spirv) {
        auto decoded = DecodeSpirvShaderArtifact(layoutBlob, decodeOptions, &decodeError);
        ASSERT_TRUE(decoded.has_value())
            << "DecodeSpirvShaderArtifact failed: " << static_cast<uint32_t>(decodeError);
        artifact.emplace(std::move(decoded.value()));
    } else {
        auto decoded = DecodeDxilShaderArtifact(layoutBlob, decodeOptions, &decodeError);
        ASSERT_TRUE(decoded.has_value())
            << "DecodeDxilShaderArtifact failed: " << static_cast<uint32_t>(decodeError);
        artifact.emplace(std::move(decoded.value()));
    }

    auto layoutResult = CreateBackendPipelineLayout(device, artifact.value());
    ASSERT_TRUE(layoutResult.HasValue()) << "backend CreatePipelineLayout failed";
    unique_ptr<PipelineLayout> layout = layoutResult.Release();

    auto secondLayoutResult = CreateBackendPipelineLayout(device, artifact.value());
    ASSERT_TRUE(secondLayoutResult.HasValue()) << "second backend layout creation failed";
    unique_ptr<PipelineLayout> secondLayout = secondLayoutResult.Release();

    BindingHandle bindingHandle{};
    BindingHandle secondBindingHandle{};
    if (spirv) {
#if defined(RADRAY_ENABLE_VULKAN)
        auto* first = static_cast<vulkan::PipelineLayoutVulkan*>(layout.get());
        auto* second = static_cast<vulkan::PipelineLayoutVulkan*>(secondLayout.get());
        bindingHandle = first->FindBinding("ColorTexture");
        secondBindingHandle = second->FindBinding("ColorTexture");
        EXPECT_FALSE(first->FindBinding("Missing").IsValid());
#endif
    } else {
#if defined(RADRAY_ENABLE_D3D12)
        auto* first = static_cast<d3d12::RootSigD3D12*>(layout.get());
        auto* second = static_cast<d3d12::RootSigD3D12*>(secondLayout.get());
        bindingHandle = first->FindBinding("ColorTexture");
        secondBindingHandle = second->FindBinding("ColorTexture");
        EXPECT_FALSE(first->FindBinding("Missing").IsValid());
#endif
    }
    EXPECT_TRUE(bindingHandle.IsValid());
    EXPECT_TRUE(secondBindingHandle.IsValid());
    EXPECT_NE(bindingHandle, secondBindingHandle);

    const ShaderBlobCategory category = spirv ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
    const ShaderDescriptor vertexDesc{
        .Source = *vertexBytecode,
        .Category = category,
        .Stages = ShaderStage::Vertex};
    const ShaderDescriptor pixelDesc{
        .Source = *pixelBytecode,
        .Category = category,
        .Stages = ShaderStage::Pixel};
    auto vertexResult = device.CreateShader(vertexDesc);
    auto pixelResult = device.CreateShader(pixelDesc);
    ASSERT_TRUE(vertexResult.HasValue()) << "CreateShader(vertex) failed";
    ASSERT_TRUE(pixelResult.HasValue()) << "CreateShader(pixel) failed";
    unique_ptr<Shader> vertexShader = vertexResult.Release();
    unique_ptr<Shader> pixelShader = pixelResult.Release();

    auto renderTarget = test::MakeRenderTarget(
        &device,
        kFormat,
        kWidth,
        kHeight,
        TextureUse::RenderTarget | TextureUse::CopySource);
    ASSERT_TRUE(renderTarget.has_value()) << "CreateTexture/TextureView failed";

    auto parameterSetResult = device.CreateShaderParameterSet(ShaderParameterSetDescriptor{
        .Layout = layout.get(),
        .GroupIndex = 0});
    ASSERT_TRUE(parameterSetResult.HasValue()) << "CreateShaderParameterSet failed";
    unique_ptr<ShaderParameterSet> parameterSet = parameterSetResult.Release();
    EXPECT_TRUE(parameterSet->Set(bindingHandle, 0, renderTarget->View.get()));
#ifdef RADRAY_IS_DEBUG
    EXPECT_DEATH(
        parameterSet->Set(secondBindingHandle, 0, renderTarget->View.get()),
        ".*");
#else
    EXPECT_FALSE(parameterSet->Set(secondBindingHandle, 0, renderTarget->View.get()));
#endif

    const RenderPassColorAttachmentDescriptor colorAttachment{
        .Format = kFormat,
        .SampleCount = 1,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store};
    auto renderPassResult = device.CreateRenderPass(
        RenderPassDescriptor{.ColorAttachments = std::span{&colorAttachment, 1}});
    ASSERT_TRUE(renderPassResult.HasValue()) << "CreateRenderPass failed";
    unique_ptr<RenderPass> renderPass = renderPassResult.Release();

    TextureView* const colorViews[]{renderTarget->View.get()};
    const FramebufferDescriptor framebufferDesc{
        .Pass = renderPass.get(),
        .ColorAttachments = colorViews,
        .DepthStencilAttachment = nullptr,
        .Width = kWidth,
        .Height = kHeight,
        .Layers = 1};
    auto framebufferResult = device.CreateFramebuffer(framebufferDesc);
    ASSERT_TRUE(framebufferResult.HasValue()) << "CreateFramebuffer failed";
    unique_ptr<Framebuffer> framebuffer = framebufferResult.Release();

    const VertexBufferLayout vertexBufferLayout{
        .Binding = 0,
        .ArrayStride = sizeof(float) * 3,
        .StepMode = VertexStepMode::Vertex};
    const VertexAttribute vertexAttribute{
        .BufferBinding = 0,
        .Offset = 0,
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .Format = VertexFormat::FLOAT32X3,
        .Location = 0};
    const VertexInputState vertexInput{
        .Buffers = std::span{&vertexBufferLayout, 1},
        .Attributes = std::span{&vertexAttribute, 1}};
    const ColorTargetState colorTarget = ColorTargetState::Default(kFormat);
    PrimitiveState primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    primitive.UnclippedDepth = false;
    const GraphicsPipelineStateDescriptor psoDesc{
        .PipelineLayout = layout.get(),
        .VS = ShaderEntry{vertexShader.get(), "VSMain"},
        .PS = ShaderEntry{pixelShader.get(), "PSMain"},
        .VertexInput = vertexInput,
        .Primitive = primitive,
        .DepthStencil = std::nullopt,
        .MultiSample = MultiSampleState::Default(),
        .ColorTargets = std::span{&colorTarget, 1},
        .CompatibleRenderPass = renderPass.get()};

    VertexAttribute missingSemantic = vertexAttribute;
    missingSemantic.Semantic = {};
    const VertexInputState missingSemanticInput{
        .Buffers = std::span{&vertexBufferLayout, 1},
        .Attributes = std::span{&missingSemantic, 1}};
    GraphicsPipelineStateDescriptor invalidPsoDesc = psoDesc;
    invalidPsoDesc.VertexInput = missingSemanticInput;
    EXPECT_FALSE(device.CreateGraphicsPipelineState(invalidPsoDesc).HasValue());

    VertexAttribute missingSlot = vertexAttribute;
    missingSlot.BufferBinding = 1;
    const VertexInputState missingSlotInput{
        .Buffers = std::span{&vertexBufferLayout, 1},
        .Attributes = std::span{&missingSlot, 1}};
    invalidPsoDesc.VertexInput = missingSlotInput;
    EXPECT_FALSE(device.CreateGraphicsPipelineState(invalidPsoDesc).HasValue());

    VertexBufferLayout shortStride = vertexBufferLayout;
    shortStride.ArrayStride = sizeof(float);
    const VertexInputState shortStrideInput{
        .Buffers = std::span{&shortStride, 1},
        .Attributes = std::span{&vertexAttribute, 1}};
    invalidPsoDesc.VertexInput = shortStrideInput;
    EXPECT_FALSE(device.CreateGraphicsPipelineState(invalidPsoDesc).HasValue());

    VertexAttribute unknownFormat = vertexAttribute;
    unknownFormat.Format = VertexFormat::UNKNOWN;
    const VertexInputState unknownFormatInput{
        .Buffers = std::span{&vertexBufferLayout, 1},
        .Attributes = std::span{&unknownFormat, 1}};
    invalidPsoDesc.VertexInput = unknownFormatInput;
    EXPECT_FALSE(device.CreateGraphicsPipelineState(invalidPsoDesc).HasValue());

    std::array<VertexAttribute, 2> duplicateLocations{vertexAttribute, vertexAttribute};
    duplicateLocations[1].Semantic = "NORMAL";
    duplicateLocations[1].Offset = 0;
    const VertexInputState duplicateLocationInput{
        .Buffers = std::span{&vertexBufferLayout, 1},
        .Attributes = duplicateLocations};
    invalidPsoDesc.VertexInput = duplicateLocationInput;
    EXPECT_FALSE(device.CreateGraphicsPipelineState(invalidPsoDesc).HasValue());

    auto psoResult = device.CreateGraphicsPipelineState(psoDesc);
    ASSERT_TRUE(psoResult.HasValue()) << "CreateGraphicsPipelineState failed";
    unique_ptr<GraphicsPipelineState> pso = psoResult.Release();

    constexpr std::array<float, 9> triangle{
        0.0f, 0.8f, 0.0f,
        -0.8f, -0.8f, 0.0f,
        0.8f, -0.8f, 0.0f};
    auto vertexBuffer = test::MakeUploadBuffer(
        device,
        std::as_bytes(std::span{triangle}),
        BufferUse::Vertex);
    ASSERT_TRUE(vertexBuffer.HasValue()) << "Create vertex buffer failed";

    const DeviceDetail detail = device.GetDetail();
    const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(kFormat);
    const uint64_t rowPitch = Align(
        static_cast<uint64_t>(kWidth) * bytesPerPixel,
        detail.TextureDataPitchAlignment);
    const uint64_t readbackSize = rowPitch * kHeight;
    auto readbackResult = device.CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
        .Hints = ResourceHint::None});
    ASSERT_TRUE(readbackResult.HasValue()) << "Create readback buffer failed";
    unique_ptr<Buffer> readback = readbackResult.Release();

    auto commandResult = device.CreateCommandBuffer(context.Queue);
    ASSERT_TRUE(commandResult.HasValue()) << "CreateCommandBuffer failed";
    unique_ptr<CommandBuffer> command = commandResult.Release();
    command->Begin();
    const ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
        .Target = renderTarget->Tex.get(),
        .Before = TextureState::Undefined,
        .After = TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toRenderTarget, 1});

    const ColorClearValue clearValue{{0.0f, 0.0f, 0.0f, 1.0f}};
    const RenderPassBeginDescriptor beginDesc{
        .Pass = renderPass.get(),
        .Target = framebuffer.get(),
        .ColorClearValues = std::span{&clearValue, 1},
        .DepthStencilClearValue = std::nullopt,
        .Name = "radray_render_pso_smoke"};
    auto encoderResult = command->BeginRenderPass(beginDesc);
    ASSERT_TRUE(encoderResult.HasValue()) << "BeginRenderPass failed";
    unique_ptr<GraphicsCommandEncoder> encoder = encoderResult.Release();
    encoder->SetViewport(Viewport{
        0.0f,
        0.0f,
        static_cast<float>(kWidth),
        static_cast<float>(kHeight),
        0.0f,
        1.0f});
    encoder->SetScissor(Rect{0, 0, kWidth, kHeight});
    encoder->BindGraphicsPipelineState(pso.get());
    const VertexBufferBinding vertexBinding{
        .Binding = 0,
        .View = VertexBufferView{
            .Target = vertexBuffer.Get(),
            .Offset = 0,
            .Size = triangle.size() * sizeof(float)}};
    encoder->BindVertexBuffers(std::span{&vertexBinding, 1});
    encoder->Draw(3, 1, 0, 0);
    command->EndRenderPass(std::move(encoder));

    const ResourceBarrierDescriptor toCopySource = BarrierTextureDescriptor{
        .Target = renderTarget->Tex.get(),
        .Before = TextureState::RenderTarget,
        .After = TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopySource, 1});
    command->CopyTextureToBuffer(
        readback.get(),
        0,
        renderTarget->Tex.get(),
        SubresourceRange{0, 1, 0, 1});
    command->End();

    CommandBuffer* commandBuffers[]{command.get()};
    context.Queue->Submit(CommandQueueSubmitDescriptor{.CmdBuffers = commandBuffers});
    context.Queue->Wait();

    void* mapped = readback->Map(0, readbackSize);
    ASSERT_NE(mapped, nullptr) << "Map readback failed";
    readback->InvalidateMappedRange(BufferRange{0, readbackSize});
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const uint8_t* center = bytes + rowPitch * (kHeight / 2) + bytesPerPixel * (kWidth / 2);
    EXPECT_EQ(center[0], 255);
    EXPECT_EQ(center[1], 0);
    EXPECT_EQ(center[2], 255);
    EXPECT_EQ(center[3], 255);
    const uint8_t* corner = bytes;
    EXPECT_EQ(corner[0], 0);
    EXPECT_EQ(corner[1], 0);
    EXPECT_EQ(corner[2], 0);
    readback->Unmap();
}

TEST(RadRayRenderPsoSmoke, D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context)) {
        GTEST_SKIP() << "D3D12 is unavailable on this machine";
    }
    RunPsoSmoke(context, RenderBackend::D3D12);
#else
    GTEST_SKIP() << "D3D12 is disabled";
#endif
}

TEST(RadRayRenderPsoSmoke, Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::Vulkan, context)) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    RunPsoSmoke(context, RenderBackend::Vulkan);
#else
    GTEST_SKIP() << "Vulkan is disabled";
#endif
}

}  // namespace
}  // namespace radray::render
