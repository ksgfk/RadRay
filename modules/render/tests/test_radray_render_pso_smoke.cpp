#include "gpu_test_fixture.h"

#include <radray/basic_math.h>
#include <radray/dynamic_library.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/utility.h>

#include <gtest/gtest.h>

#include <dxc/dxcapi.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace radray::render {
namespace {

using Microsoft::WRL::ComPtr;

using shader::DxilShaderArtifactView;
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
    bool spirv,
    std::string_view shaderSource = kShaderSource) {
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
        .Ptr = shaderSource.data(),
        .Size = shaderSource.size(),
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
    binding.Type = static_cast<uint32_t>(shader::ShaderBindingKind::Texture);
    binding.Count = 1;
    binding.StageMask = envelope.StageMask;
    // No policy produced this artifact, so the binding is an ordinary table entry with no sampler
    // state attached; the defaults already say that, and spelling it out keeps the intent readable.
    binding.Placement = static_cast<uint32_t>(shader::ShaderBindingPlacement::Table);
    binding.SamplerIndex = shader::kShaderNoSampler;

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

void RunPsoSmoke(test::DeviceContext& context, RenderBackend backend) {
    Device& device = *context.Device;
    DynamicLibrary compilerLibrary{"dxcompiler"};
    ASSERT_TRUE(compilerLibrary.IsValid())
        << "RADRAY_BUILD_SHADER_COMPILER is enabled but dxcompiler is missing from the test output directory";

    const std::optional<shader::ShaderTarget> target = GetShaderTargetForBackend(backend);
    ASSERT_TRUE(target.has_value());
    const bool spirv = target.value() == shader::ShaderTarget::SPIRV;
    const auto vertexBytecode = CompileWithStockDxc(compilerLibrary, "VSMain", "vs_6_0", spirv);
    const auto pixelBytecode = CompileWithStockDxc(compilerLibrary, "PSMain", "ps_6_0", spirv);
    ASSERT_TRUE(vertexBytecode.has_value()) << "stock DXC failed to compile the vertex shader";
    ASSERT_TRUE(pixelBytecode.has_value()) << "stock DXC failed to compile the pixel shader";

    const auto layoutBlob = MakeLayoutArtifact(
        *vertexBytecode,
        target.value());
    shader::GpuArtifactHash expectedGpuArtifact{};
    ShaderArtifactDecodeOptions decodeOptions{
        .Target = target.value(),
        .ExpectedGpuArtifact = expectedGpuArtifact};
    ShaderArtifactDecodeOptions wrongTargetOptions = decodeOptions;
    wrongTargetOptions.Target = spirv
                                    ? shader::ShaderTarget::DXIL
                                    : shader::ShaderTarget::SPIRV;
    BackendShaderArtifactError mismatchError;
    EXPECT_FALSE(
        CreateBackendShaderArtifact(device, layoutBlob, wrongTargetOptions, &mismatchError)
            .has_value());
    EXPECT_EQ(mismatchError.Failure, BackendShaderArtifactFailure::TargetMismatch);
    EXPECT_EQ(mismatchError.DecodeFailure, shader::ShaderArtifactDecodeError::None);

    const vector<byte> wrongEnvelopeBlob = MakeLayoutArtifact(
        *vertexBytecode,
        wrongTargetOptions.Target);
    BackendShaderArtifactError envelopeError;
    EXPECT_FALSE(
        CreateBackendShaderArtifact(device, wrongEnvelopeBlob, decodeOptions, &envelopeError)
            .has_value());
    EXPECT_EQ(envelopeError.Failure, BackendShaderArtifactFailure::DecodeFailed);
    EXPECT_NE(envelopeError.DecodeFailure, shader::ShaderArtifactDecodeError::None);

    BackendShaderArtifactError artifactError;
    std::optional<BackendShaderArtifact> artifact =
        CreateBackendShaderArtifact(device, layoutBlob, decodeOptions, &artifactError);
    ASSERT_TRUE(artifact.has_value())
        << static_cast<uint32_t>(artifactError.Failure) << ":"
        << static_cast<uint32_t>(artifactError.DecodeFailure);
    std::optional<BackendShaderArtifact> secondArtifact =
        CreateBackendShaderArtifact(device, layoutBlob, decodeOptions, &artifactError);
    ASSERT_TRUE(secondArtifact.has_value())
        << static_cast<uint32_t>(artifactError.Failure) << ":"
        << static_cast<uint32_t>(artifactError.DecodeFailure);
    unique_ptr<PipelineLayout> layout = std::move(artifact->Layout);
    unique_ptr<PipelineLayout> secondLayout = std::move(secondArtifact->Layout);

    const BindingHandle bindingHandle = layout->FindBinding("ColorTexture");
    const BindingHandle secondBindingHandle = secondLayout->FindBinding("ColorTexture");
    EXPECT_FALSE(layout->FindBinding("Missing").IsValid());
    EXPECT_TRUE(bindingHandle.IsValid());
    EXPECT_TRUE(secondBindingHandle.IsValid());
    EXPECT_NE(bindingHandle, secondBindingHandle);

    const ShaderDescriptor vertexDesc{
        .Source = *vertexBytecode,
        .Category = artifact->Category,
        .Stages = ShaderStage::Vertex};
    const ShaderDescriptor pixelDesc{
        .Source = *pixelBytecode,
        .Category = artifact->Category,
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
        TextureUse::RenderTarget | TextureUse::CopySource | TextureUse::Resource);
    ASSERT_TRUE(renderTarget.has_value()) << "CreateTexture/TextureView failed";
    auto resourceView = device.CreateTextureView({renderTarget->Tex.get(), TextureDimension::Dim2D,
                                                  kFormat, {0, 1, 0, 1}, TextureViewUsage::Resource});
    ASSERT_TRUE(resourceView.HasValue());

    auto parameterSetResult = device.CreateShaderParameterSet(ShaderParameterSetDescriptor{
        .Layout = layout.get(),
        .GroupIndex = 0});
    ASSERT_TRUE(parameterSetResult.HasValue()) << "CreateShaderParameterSet failed";
    unique_ptr<ShaderParameterSet> parameterSet = parameterSetResult.Release();
    EXPECT_TRUE(parameterSet->Set(bindingHandle, 0, resourceView.Get()));
    // The two layouts describe the same shader, so the handles look interchangeable, but each carries
    // the generation of the layout that minted it. Using the foreign one is reported through the
    // return value in every configuration: aborting in Debug would make the fail-closed path
    // untestable in the only configuration these tests run in.
    EXPECT_FALSE(parameterSet->Set(secondBindingHandle, 0, resourceView.Get()));

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

#if defined(RADRAY_ENABLE_D3D12)
TEST(RadRayRenderPsoSmoke, D3D12VisibilityTablesAndExplicitMirrorsDraw) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true)) GTEST_SKIP() << "no d3d12 device";
    auto* device = static_cast<d3d12::DeviceD3D12*>(context.Device.get());
    DynamicLibrary compiler{"dxcompiler"};
    ASSERT_TRUE(compiler.IsValid());
    const std::string_view source = R"hlsl(
struct Value { float4 Data; };
ConstantBuffer<Value> V : register(b0);
ConstantBuffer<Value> P : register(b1);
ConstantBuffer<Value> Shared : register(b2);
[shader("vertex")]
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 xy = float2((id << 1) & 2, id & 2) * 2.0 - 1.0;
    return float4(xy + V.Data.xy + Shared.Data.xy, 0.0, 1.0);
}
[shader("pixel")]
float4 PSMain() : SV_Target0 { return P.Data + Shared.Data; }
)hlsl";
    const auto vs = CompileWithStockDxc(compiler, "VSMain", "vs_6_0", false, source);
    const auto ps = CompileWithStockDxc(compiler, "PSMain", "ps_6_0", false, source);
    ASSERT_TRUE(vs.has_value());
    ASSERT_TRUE(ps.has_value());
    auto vertex = device->CreateShader({.Source = *vs, .Category = ShaderBlobCategory::DXIL, .Stages = ShaderStage::Vertex});
    auto pixel = device->CreateShader({.Source = *ps, .Category = ShaderBlobCategory::DXIL, .Stages = ShaderStage::Pixel});
    ASSERT_TRUE(vertex.HasValue());
    ASSERT_TRUE(pixel.HasValue());
    ResolvedD3D12Layout description;
    for (uint32_t i = 0; i < 3; ++i) {
        ResolvedD3D12Binding binding;
        binding.Name = i == 0 ? "V" : i == 1 ? "P"
                                             : "Shared";
        binding.LogicalKind = shader::ShaderBindingKind::CBuffer;
        binding.Binding = i;
        binding.Count = 1;
        binding.Stages = i == 0 ? ShaderStages{ShaderStage::Vertex} : i == 1 ? ShaderStages{ShaderStage::Pixel}
                                                                             : ShaderStage::Vertex | ShaderStage::Pixel;
        binding.Placement = shader::ShaderBindingPlacement::Table;
        description.Bindings.push_back(binding);
    }
    auto upload = device->CreateBuffer({.Size = 768, .Memory = MemoryType::Upload, .Usage = BufferUse::CBuffer | BufferUse::MapWrite});
    ASSERT_TRUE(upload.HasValue());
    auto* bytes = static_cast<byte*>(upload->Map(0, 768));
    ASSERT_NE(bytes, nullptr);
    std::memset(bytes, 0, 768);
    const float tint[4]{1, 0, 0, 1};
    const float shared[4]{0, 0, 1, 0};
    std::memcpy(bytes + 256, tint, sizeof(tint));
    std::memcpy(bytes + 512, shared, sizeof(shared));
    upload->FlushMappedRange({0, 768});
    upload->Unmap();
    // Explicit tables deliberately contain holes and duplicate b2 in disjoint stages.
    const auto makeCarrier = [](bool reverse) {
        D3D12_DESCRIPTOR_RANGE1 ranges[2][2]{};
        D3D12_ROOT_PARAMETER1 parameters[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            ranges[i][0] = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, i, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 1};
            ranges[i][1] = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 4};
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {2, ranges[i]};
            parameters[i].ShaderVisibility = i == 0 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
        }
        if (reverse) std::swap(parameters[0], parameters[1]);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC native{};
        native.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        native.Desc_1_1.NumParameters = 2;
        native.Desc_1_1.pParameters = parameters;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeVersionedRootSignature(&native, &blob, &error))) return vector<byte>{};
        const auto* begin = static_cast<const byte*>(blob->GetBufferPointer());
        return vector<byte>{begin, begin + blob->GetBufferSize()};
    };
    for (uint32_t mode = 0; mode < 3; ++mode) {
        description.SerializedRootSignature = mode == 0 ? vector<byte>{} : makeCarrier(false);
        auto sourceLayout = device->CreatePipelineLayout(description);
        ASSERT_TRUE(sourceLayout.HasValue());
        if (mode == 2) description.SerializedRootSignature = makeCarrier(true);
        auto targetLayout = device->CreatePipelineLayout(description);
        ASSERT_TRUE(targetLayout.HasValue());
        auto set = device->CreateShaderParameterSet({.Layout = sourceLayout.Get(), .GroupIndex = 0});
        ASSERT_TRUE(set.HasValue());
        for (uint32_t i = 0; i < 3; ++i) {
            ASSERT_TRUE(set->Set(sourceLayout->FindBinding(description.Bindings[i].Name), 0,
                                 ShaderBufferBinding{upload.Get(), {uint64_t{i} * 256, 256}, 0}));
        }
        ASSERT_TRUE(set->FlushWrites());
        if (mode != 0) {
            const auto* nativeSet = d3d12::CastD3D12Object(set.Get());
            ASSERT_EQ(nativeSet->_tables.size(), 2u);
            for (const auto& table : nativeSet->_tables) {
                EXPECT_EQ(table.InitializedSlots, (vector<uint8_t>{0, 1, 0, 0, 1}));
            }
        }
        auto target = test::MakeRenderTarget(device, kFormat, kWidth, kHeight, TextureUse::RenderTarget | TextureUse::CopySource);
        ASSERT_TRUE(target.has_value());
        const RenderPassColorAttachmentDescriptor attachment{kFormat, 1, LoadAction::Clear, StoreAction::Store};
        auto pass = device->CreateRenderPass({.ColorAttachments = std::span{&attachment, 1}});
        ASSERT_TRUE(pass.HasValue());
        TextureView* targetView = target->View.get();
        auto framebuffer = device->CreateFramebuffer({pass.Get(), std::span{&targetView, 1}, nullptr, kWidth, kHeight, 1});
        ASSERT_TRUE(framebuffer.HasValue());
        const auto color = ColorTargetState::Default(kFormat);
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        auto pso = device->CreateGraphicsPipelineState({.PipelineLayout = targetLayout.Get(),
                                                        .VS = ShaderEntry{vertex.Get(), "VSMain"},
                                                        .PS = ShaderEntry{pixel.Get(), "PSMain"},
                                                        .Primitive = primitive,
                                                        .MultiSample = MultiSampleState::Default(),
                                                        .ColorTargets = std::span{&color, 1},
                                                        .CompatibleRenderPass = pass.Get()});
        ASSERT_TRUE(pso.HasValue());
        const auto pitch = Align(uint64_t{kWidth * 4}, device->GetDetail().TextureDataPitchAlignment);
        auto readback = device->CreateBuffer({pitch * kHeight, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead});
        ASSERT_TRUE(readback.HasValue());
        auto command = device->CreateCommandBuffer(context.Queue);
        ASSERT_TRUE(command.HasValue());
        command->Begin();
        const ResourceBarrierDescriptor toTarget = BarrierTextureDescriptor{target->Tex.get(), TextureState::Undefined, TextureState::RenderTarget};
        command->ResourceBarrier(std::span{&toTarget, 1});
        const ColorClearValue clear{{0, 0, 0, 1}};
        auto encoder = command->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}});
        ASSERT_TRUE(encoder.HasValue());
        encoder->SetViewport({0, 0, float(kWidth), float(kHeight), 0, 1});
        encoder->SetScissor({0, 0, kWidth, kHeight});
        encoder->BindGraphicsPipelineState(pso.Get());
        encoder->BindShaderParameterSet(0, set.Get());
        encoder->Draw(3, 1, 0, 0);
        command->EndRenderPass(encoder.Release());
        const ResourceBarrierDescriptor toCopy = BarrierTextureDescriptor{target->Tex.get(), TextureState::RenderTarget, TextureState::CopySource};
        command->ResourceBarrier(std::span{&toCopy, 1});
        command->CopyTextureToBuffer(readback.Get(), 0, target->Tex.get(), {0, 1, 0, 1});
        command->End();
        CommandBuffer* cmd = command.Get();
        context.Queue->Submit({.CmdBuffers = std::span{&cmd, 1}});
        context.Queue->Wait();
        const auto* mapped = static_cast<const uint8_t*>(readback->Map(0, pitch * kHeight));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, pitch * kHeight});
        const auto* center = mapped + pitch * (kHeight / 2) + 4 * (kWidth / 2);
        EXPECT_EQ(center[0], 255) << mode;
        EXPECT_EQ(center[1], 0) << mode;
        EXPECT_EQ(center[2], 255) << mode;
        EXPECT_EQ(center[3], 255) << mode;
        readback->Unmap();
    }
    device->TryDrainValidationMessages();
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}
TEST(RadRayRenderPsoSmoke, D3D12DirtyArraysTextureAndSamplerDispatch) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true)) GTEST_SKIP() << "no d3d12 device";
    auto* device = static_cast<d3d12::DeviceD3D12*>(context.Device.get());
    DynamicLibrary compiler{"dxcompiler"};
    ASSERT_TRUE(compiler.IsValid());
    const std::string_view source = R"hlsl(
struct Value { uint Data; };
ConstantBuffer<Value> Inputs[4] : register(b0);
Texture2D<float4> Texture : register(t0);
RWStructuredBuffer<uint> Output : register(u0);
SamplerState Sampler : register(s0);
[shader("compute")]
[numthreads(4, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    Output[id.x] = Inputs[id.x].Data + uint(Texture.SampleLevel(Sampler, float2(0.5, 0.5), 0).r * 100.0);
}
)hlsl";
    const auto code = CompileWithStockDxc(compiler, "CSMain", "cs_6_0", false, source);
    ASSERT_TRUE(code.has_value());
    auto shader = device->CreateShader({.Source = *code, .Category = ShaderBlobCategory::DXIL, .Stages = ShaderStage::Compute});
    ASSERT_TRUE(shader.HasValue());
    ResolvedD3D12Layout description;
    const shader::ShaderBindingKind kinds[]{shader::ShaderBindingKind::CBuffer, shader::ShaderBindingKind::Texture,
                                            shader::ShaderBindingKind::RWStructuredBuffer, shader::ShaderBindingKind::Sampler};
    const std::string_view names[]{"Inputs", "Texture", "Output", "Sampler"};
    for (uint32_t i = 0; i < 4; ++i) {
        ResolvedD3D12Binding binding;
        binding.Name = names[i];
        binding.LogicalKind = kinds[i];
        binding.Count = i == 0 ? 4 : 1;
        binding.Stages = ShaderStage::Compute;
        binding.Placement = shader::ShaderBindingPlacement::Table;
        description.Bindings.push_back(binding);
    }
    auto layout = device->CreatePipelineLayout(description);
    ASSERT_TRUE(layout.HasValue());
    auto set = device->CreateShaderParameterSet({.Layout = layout.Get(), .GroupIndex = 0});
    ASSERT_TRUE(set.HasValue());
    auto pso = device->CreateComputePipelineState({.PipelineLayout = layout.Get(), .CS = ShaderEntry{shader.Get(), "CSMain"}});
    ASSERT_TRUE(pso.HasValue());
    auto constants = device->CreateBuffer({2048, MemoryType::Upload, BufferUse::CBuffer | BufferUse::MapWrite});
    auto output = device->CreateBuffer({16, MemoryType::Device, BufferUse::UnorderedAccess | BufferUse::CopySource});
    auto readback = device->CreateBuffer({16, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead});
    ASSERT_TRUE(constants.HasValue());
    ASSERT_TRUE(output.HasValue());
    ASSERT_TRUE(readback.HasValue());
    auto* bytes = static_cast<byte*>(constants->Map(0, 2048));
    ASSERT_NE(bytes, nullptr);
    std::memset(bytes, 0, 2048);
    for (uint32_t i = 0; i < 8; ++i) std::memcpy(bytes + i * 256, &i, sizeof(i));
    constants->FlushMappedRange({0, 2048});
    constants->Unmap();
    auto texture = test::MakeRenderTarget(device, kFormat, 1, 1, TextureUse::RenderTarget | TextureUse::Resource);
    ASSERT_TRUE(texture.has_value());
    auto view = device->CreateTextureView({texture->Tex.get(), TextureDimension::Dim2D, kFormat, {0, 1, 0, 1}, TextureViewUsage::Resource});
    auto sampler = device->CreateSampler({});
    ASSERT_TRUE(view.HasValue());
    ASSERT_TRUE(sampler.HasValue());
    const RenderPassColorAttachmentDescriptor attachment{kFormat, 1, LoadAction::Clear, StoreAction::Store};
    auto pass = device->CreateRenderPass({.ColorAttachments = std::span{&attachment, 1}});
    ASSERT_TRUE(pass.HasValue());
    TextureView* renderView = texture->View.get();
    auto framebuffer = device->CreateFramebuffer({pass.Get(), std::span{&renderView, 1}, nullptr, 1, 1, 1});
    ASSERT_TRUE(framebuffer.HasValue());
    ASSERT_TRUE(set->Set(layout->FindBinding("Texture"), 0, view.Get()));
    ASSERT_TRUE(set->Set(layout->FindBinding("Sampler"), 0, sampler.Get()));
    ASSERT_TRUE(set->Set(layout->FindBinding("Output"), 0, ShaderBufferBinding{output.Get(), {0, 16}, 4}));
    uint32_t expected[4]{0, 1, 2, 3};
    for (uint32_t round = 0; round < 5; ++round) {
        if (round == 1) {
            expected[0] = 4;
            expected[3] = 7;
        }
        if (round == 2) {
            expected[1] = 5;
            expected[2] = 6;
        }
        if (round == 4) {
            for (uint32_t i = 0; i < 4; ++i) expected[i] = i;
        } else {
            for (uint32_t i = 0; i < 4; ++i) {
                if (round == 1 && i != 0 && i != 3) continue;
                if (round == 2 && i != 1 && i != 2) continue;
                const uint32_t value = round == 3 ? i : expected[i];
                ASSERT_TRUE(set->Set(layout->FindBinding("Inputs"), i,
                                     ShaderBufferBinding{constants.Get(), {uint64_t{(value + 1) % 8} * 256, 256}, 0}));
                ASSERT_TRUE(set->Set(layout->FindBinding("Inputs"), i,
                                     ShaderBufferBinding{constants.Get(), {uint64_t{value} * 256, 256}, 0}));
            }
        }
        // Round 3 changes only the mirror; binding must keep the previously published values.
        // Round 4 publishes those pending writes without another Set.
        if (round != 3) ASSERT_TRUE(set->FlushWrites());
        auto command = device->CreateCommandBuffer(context.Queue);
        ASSERT_TRUE(command.HasValue());
        command->Begin();
        if (round == 0) {
            const ResourceBarrierDescriptor toTarget = BarrierTextureDescriptor{texture->Tex.get(), TextureState::Undefined, TextureState::RenderTarget};
            command->ResourceBarrier(std::span{&toTarget, 1});
            const ColorClearValue clear{{1, 0, 0, 1}};
            auto encoder = command->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}});
            ASSERT_TRUE(encoder.HasValue());
            command->EndRenderPass(encoder.Release());
            const ResourceBarrierDescriptor toRead = BarrierTextureDescriptor{texture->Tex.get(), TextureState::RenderTarget, TextureState::ShaderRead};
            command->ResourceBarrier(std::span{&toRead, 1});
        }
        const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.Get(), round == 0 ? BufferState::Undefined : BufferState::CopySource, BufferState::UnorderedAccess};
        command->ResourceBarrier(std::span{&toUav, 1});
        auto encoder = command->BeginComputePass();
        ASSERT_TRUE(encoder.HasValue());
        encoder->BindComputePipelineState(pso.Get());
        encoder->BindShaderParameterSet(0, set.Get());
        encoder->Dispatch(1, 1, 1);
        command->EndComputePass(encoder.Release());
        const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.Get(), BufferState::UnorderedAccess, BufferState::CopySource};
        command->ResourceBarrier(std::span{&toCopy, 1});
        command->CopyBufferToBuffer(readback.Get(), 0, output.Get(), 0, 16);
        command->End();
        CommandBuffer* cmd = command.Get();
        context.Queue->Submit({.CmdBuffers = std::span{&cmd, 1}});
        context.Queue->Wait();
        const auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 16));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, 16});
        for (uint32_t i = 0; i < 4; ++i) EXPECT_EQ(mapped[i], expected[i] + 100) << round << ":" << i;
        readback->Unmap();
    }
    device->TryDrainValidationMessages();
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

#endif

#if defined(RADRAY_ENABLE_VULKAN)
TEST(RadRayRenderPsoSmoke, VulkanImmediateDescriptorsAndDynamicOffsets) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::Vulkan, context, true)) GTEST_SKIP() << "no Vulkan device";
    auto* device = static_cast<vulkan::DeviceVulkan*>(context.Device.get());
    DynamicLibrary compiler{"dxcompiler"};
    ASSERT_TRUE(compiler.IsValid());
    const std::string_view source = R"hlsl(
struct Value { uint Data; };
[[vk::binding(0, 0)]] ConstantBuffer<Value> Inputs[4];
[[vk::binding(1, 0)]] Texture2D<float4> Texture;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> Output;
[[vk::binding(3, 0)]] SamplerState Sampler;
[[vk::binding(4, 0)]] Buffer<uint> Palette[2];
[[vk::binding(5, 0)]] RWBuffer<uint> TypedOutput;
[[vk::binding(6, 0)]] ConstantBuffer<Value> DynamicView;
[[vk::binding(7, 0)]] StructuredBuffer<uint> DynamicItems;
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
    uint extra = uint(Texture.SampleLevel(Sampler, float2(0.5, 0.5), 0).r * 100.0)
               + Palette[0][0] + Palette[1][0] + DynamicView.Data + DynamicItems[0];
    [unroll] for (uint i = 0; i < 4; ++i) Output[i] = Inputs[i].Data + extra;
    TypedOutput[0] = Inputs[0].Data + extra;
}
)hlsl";
    const auto code = CompileWithStockDxc(compiler, "CSMain", "cs_6_0", true, source);
    ASSERT_TRUE(code.has_value());
    auto shader = device->CreateShader({.Source = *code, .Category = ShaderBlobCategory::SPIRV, .Stages = ShaderStage::Compute});
    ASSERT_TRUE(shader.HasValue());
    ResolvedVulkanLayout description;
    description.SetCount = 1;
    const shader::ShaderBindingKind kinds[]{shader::ShaderBindingKind::CBuffer, shader::ShaderBindingKind::Texture,
                                            shader::ShaderBindingKind::RWStructuredBuffer, shader::ShaderBindingKind::Sampler,
                                            shader::ShaderBindingKind::TypedBuffer, shader::ShaderBindingKind::RWTypedBuffer,
                                            shader::ShaderBindingKind::CBuffer, shader::ShaderBindingKind::StructuredBuffer};
    const std::string_view names[]{"Inputs", "Texture", "Output", "Sampler", "Palette", "TypedOutput", "DynamicView", "DynamicItems"};
    for (uint32_t i = 0; i < 8; ++i) {
        ResolvedVulkanBinding binding;
        binding.Name = names[i];
        binding.LogicalKind = kinds[i];
        binding.Binding = i;
        binding.Count = i == 0 ? 4 : i == 4 ? 2 : 1;
        binding.Stages = ShaderStage::Compute;
        if (i >= 6) binding.Placement = VulkanBufferDescriptorPlacement::Dynamic;
        description.Bindings.push_back(binding);
    }
    description.DynamicOffsetOrder = {6, 7};
    auto layout = device->CreatePipelineLayout(description);
    ASSERT_TRUE(layout.HasValue());
    auto set = device->CreateShaderParameterSet({.Layout = layout.Get(), .GroupIndex = 0});
    ASSERT_TRUE(set.HasValue());
    auto compatibleLayout = device->CreatePipelineLayout(description);
    ASSERT_TRUE(compatibleLayout.HasValue());
    auto incompatibleDescription = description;
    incompatibleDescription.Bindings[0].Count = 5;
    auto incompatibleLayout = device->CreatePipelineLayout(incompatibleDescription);
    ASSERT_TRUE(incompatibleLayout.HasValue());
    auto incompatibleSet = device->CreateShaderParameterSet({.Layout = incompatibleLayout.Get(), .GroupIndex = 0});
    ASSERT_TRUE(incompatibleSet.HasValue());
    auto pso = device->CreateComputePipelineState({.PipelineLayout = compatibleLayout.Get(), .CS = ShaderEntry{shader.Get(), "CSMain"}});
    ASSERT_TRUE(pso.HasValue());
    auto constants = device->CreateBuffer({2048, MemoryType::Upload, BufferUse::CBuffer | BufferUse::Resource | BufferUse::MapWrite});
    auto output = device->CreateBuffer({512, MemoryType::Device, BufferUse::UnorderedAccess | BufferUse::CopySource});
    auto readback = device->CreateBuffer({512, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead});
    ASSERT_TRUE(constants.HasValue());
    ASSERT_TRUE(output.HasValue());
    ASSERT_TRUE(readback.HasValue());
    auto* bytes = static_cast<byte*>(constants->Map(0, 2048));
    ASSERT_NE(bytes, nullptr);
    std::memset(bytes, 0, 2048);
    for (uint32_t i = 0; i < 8; ++i) std::memcpy(bytes + i * 256, &i, sizeof(i));
    constants->FlushMappedRange({0, 2048});
    constants->Unmap();
    auto texture = test::MakeRenderTarget(device, kFormat, 1, 1, TextureUse::RenderTarget | TextureUse::Resource);
    ASSERT_TRUE(texture.has_value());
    auto view = device->CreateTextureView({texture->Tex.get(), TextureDimension::Dim2D, kFormat, {0, 1, 0, 1}, TextureViewUsage::Resource});
    auto sampler = device->CreateSampler({});
    ASSERT_TRUE(view.HasValue());
    ASSERT_TRUE(sampler.HasValue());
    const RenderPassColorAttachmentDescriptor attachment{kFormat, 1, LoadAction::Clear, StoreAction::Store};
    auto pass = device->CreateRenderPass({.ColorAttachments = std::span{&attachment, 1}});
    ASSERT_TRUE(pass.HasValue());
    TextureView* renderView = texture->View.get();
    auto framebuffer = device->CreateFramebuffer({pass.Get(), std::span{&renderView, 1}, nullptr, 1, 1, 1});
    ASSERT_TRUE(framebuffer.HasValue());
    ASSERT_TRUE(set->Set(layout->FindBinding("Texture"), 0, view.Get()));
    ASSERT_TRUE(set->Set(layout->FindBinding("Sampler"), 0, sampler.Get()));
    ASSERT_TRUE(set->Set(layout->FindBinding("Output"), 0, ShaderBufferBinding{output.Get(), {0, 16}, 4}));
    ASSERT_EQ(vulkan::CastVkObject(set.Get())->_texelBufferViews.size(), 3u);
    ASSERT_TRUE(set->Set(layout->FindBinding("TypedOutput"), 0, ShaderTexelBufferBinding{output.Get(), {256, 4}, TextureFormat::R32_UINT}));
    ASSERT_TRUE(set->Set(layout->FindBinding("DynamicView"), 0, ShaderBufferBinding{constants.Get(), {0, 256}, 0}));
    ASSERT_TRUE(set->Set(layout->FindBinding("DynamicItems"), 0, ShaderBufferBinding{constants.Get(), {0, 4}, 4}));
    const ShaderParameterDynamicOffset offsets[]{
        {compatibleLayout->FindBinding("DynamicItems"), 768}, {compatibleLayout->FindBinding("DynamicView"), 512}};
    uint32_t expected[4]{0, 1, 2, 3};
    for (uint32_t round = 0; round < 3; ++round) {
        if (round == 1) { expected[0] = 4; expected[3] = 7; }
        if (round == 2) { expected[1] = 5; expected[2] = 6; }
        for (uint32_t i = 0; i < 4; ++i) {
            if (round == 1 && i != 0 && i != 3) continue;
            if (round == 2 && i != 1 && i != 2) continue;
            ASSERT_TRUE(set->Set(layout->FindBinding("Inputs"), i,
                                 ShaderBufferBinding{constants.Get(), {uint64_t{(expected[i] + 1) % 8} * 256, 256}, 0}));
            ASSERT_TRUE(set->Set(layout->FindBinding("Inputs"), i,
                                 ShaderBufferBinding{constants.Get(), {uint64_t{expected[i]} * 256, 256}, 0}));
        }
        for (uint32_t i = 0; i < 2; ++i) {
            ASSERT_TRUE(set->Set(layout->FindBinding("Palette"), i,
                                 ShaderTexelBufferBinding{constants.Get(), {uint64_t{round + i} * 256, 4}, TextureFormat::R32_UINT}));
        }
        if (round == 1) {
            auto* nativeSet = vulkan::CastVkObject(set.Get());
            const auto previous = nativeSet->_texelBufferViews[0]->_bufferView;
            const auto createBufferView = device->_ftb.vkCreateBufferView;
            device->_ftb.vkCreateBufferView = [](VkDevice, const VkBufferViewCreateInfo*, const VkAllocationCallbacks*, VkBufferView*) -> VkResult {
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            };
            const bool updated = set->Set(layout->FindBinding("Palette"), 0,
                                          ShaderTexelBufferBinding{constants.Get(), {1792, 4}, TextureFormat::R32_UINT});
            device->_ftb.vkCreateBufferView = createBufferView;
            EXPECT_FALSE(updated);
            EXPECT_EQ(nativeSet->_texelBufferViews[0]->_bufferView, previous);
        }
        // Set 已更新原生 descriptor；只有中间一轮调用兼容 Flush，其余直接绑定。
        if (round == 1) ASSERT_TRUE(set->FlushWrites());
        auto command = device->CreateCommandBuffer(context.Queue);
        ASSERT_TRUE(command.HasValue());
        command->Begin();
        if (round == 0) {
            const ResourceBarrierDescriptor toTarget = BarrierTextureDescriptor{texture->Tex.get(), TextureState::Undefined, TextureState::RenderTarget};
            command->ResourceBarrier(std::span{&toTarget, 1});
            const ColorClearValue clear{{1, 0, 0, 1}};
            auto encoder = command->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}});
            ASSERT_TRUE(encoder.HasValue());
            command->EndRenderPass(encoder.Release());
            const ResourceBarrierDescriptor toRead = BarrierTextureDescriptor{texture->Tex.get(), TextureState::RenderTarget, TextureState::ShaderRead};
            command->ResourceBarrier(std::span{&toRead, 1});
        }
        const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.Get(), round == 0 ? BufferState::Undefined : BufferState::CopySource, BufferState::UnorderedAccess};
        command->ResourceBarrier(std::span{&toUav, 1});
        auto encoder = command->BeginComputePass();
        ASSERT_TRUE(encoder.HasValue());
        encoder->BindComputePipelineState(pso.Get());
        encoder->BindShaderParameterSet(0, set.Get(), offsets);
        // 不兼容 set 必须在原生调用前被拒绝，保留刚绑定的有效 descriptor set。
        encoder->BindShaderParameterSet(0, incompatibleSet.Get(), offsets);
        encoder->Dispatch(1, 1, 1);
        command->EndComputePass(encoder.Release());
        const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.Get(), BufferState::UnorderedAccess, BufferState::CopySource};
        command->ResourceBarrier(std::span{&toCopy, 1});
        command->CopyBufferToBuffer(readback.Get(), 0, output.Get(), 0, 512);
        command->End();
        CommandBuffer* cmd = command.Get();
        context.Queue->Submit({.CmdBuffers = std::span{&cmd, 1}});
        context.Queue->Wait();
        const auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 512));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, 512});
        for (uint32_t i = 0; i < 4; ++i) EXPECT_EQ(mapped[i], expected[i] + 106 + 2 * round) << round << ":" << i;
        EXPECT_EQ(mapped[64], expected[0] + 106 + 2 * round);
        readback->Unmap();
    }
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

#endif

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
