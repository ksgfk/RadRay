#include "gpu_test_fixture.h"
#include "shader_contract_fixtures.h"

#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/runtime/shader_jit.h>

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace radray {
namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr render::TextureFormat kFormat = render::TextureFormat::RGBA8_UNORM;
constexpr std::string_view kSourceName = "runtime_jit/graphics.hlsl";
constexpr std::string_view kComputeSourceName = "runtime_jit/compute.hlsl";
constexpr uint32_t kComputeValue = 0xc0de1234u;
constexpr std::string_view kGraphicsSource = R"hlsl(
struct VSInput {
    float3 Position : POSITION;
};

struct VSOutput {
    float4 Position : SV_Position;
};

[shader("vertex")]
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.Position = float4(input.Position, 1.0f);
    return output;
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
)hlsl";

constexpr std::string_view kKeywordSource = R"hlsl(
#pragma radray_keyword_group QUALITY "low" "high"

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID) {
}
)hlsl";

constexpr std::string_view kComputeSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0)
RWStructuredBuffer<uint> Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID) {
    Output[0] = 0xc0de1234;
}
)hlsl";

vector<byte> CopyBytes(std::string_view value) {
    const auto* data = reinterpret_cast<const byte*>(value.data());
    return {data, data + value.size()};
}

vector<byte> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    vector<byte> result(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(result.data()), size);
    return file.good() || file.eof() ? result : vector<byte>{};
}

vector<std::filesystem::path> ShaderIncludePaths() {
    return {std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib"};
}

shader::CompileVariantRequest MakeRequest(
    const shader::ContractHash& contract,
    shader::ShaderTarget target) {
    return shader::CompileVariantRequest{
        .SourceName = string{kSourceName},
        .RootSource = CopyBytes(kGraphicsSource),
        .Defines = {},
        .Assignments = {},
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)),
        .ExpectedContract = contract};
}

shader::CompileVariantRequest MakeComputeRequest(
    const shader::ContractHash& contract,
    shader::ShaderTarget target) {
    return shader::CompileVariantRequest{
        .SourceName = string{kComputeSourceName},
        .RootSource = CopyBytes(kComputeSource),
        .Defines = {},
        .Assignments = {},
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)),
        .ExpectedContract = contract};
}

void RunGraphicsJitSmoke(
    render::test::DeviceContext& context,
    render::RenderBackend backend) {
    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());
    const std::optional<shader::ShaderTarget> target =
        render::GetShaderTargetForBackend(backend);
    ASSERT_TRUE(target.has_value());
    const auto contract = jit.DiscoverContractHash(
        kSourceName,
        std::as_bytes(std::span{kGraphicsSource.data(), kGraphicsSource.size()}),
        target.value());
    ASSERT_TRUE(contract.has_value());
    const auto request = MakeRequest(contract.value(), target.value());
    const auto jitArtifact = jit.Compile(request, target.value());
    ASSERT_TRUE(jitArtifact.has_value());
    ASSERT_EQ(jitArtifact->Target, target.value());

    shader::ShaderArtifactDecodeOptions options{
        .Target = jitArtifact->Target,
        .ExpectedGpuArtifact = jitArtifact->ExpectedGpuArtifact};
    render::Device& device = *context.Device;
    render::BackendShaderArtifactError artifactError;
    std::optional<render::BackendShaderArtifact> backendArtifact =
        render::CreateBackendShaderArtifact(device, jitArtifact->Metadata, options, &artifactError);
    ASSERT_TRUE(backendArtifact.has_value())
        << static_cast<uint32_t>(artifactError.Failure) << ":"
        << static_cast<uint32_t>(artifactError.DecodeFailure);
    const auto vertexBytecode =
        backendArtifact->Generic().FindStageBytecode(shader::ShaderStage::Vertex);
    const auto pixelBytecode =
        backendArtifact->Generic().FindStageBytecode(shader::ShaderStage::Pixel);
    ASSERT_TRUE(vertexBytecode.has_value());
    ASSERT_TRUE(pixelBytecode.has_value());
    unique_ptr<render::PipelineLayout> layout = std::move(backendArtifact->Layout);
    auto vertexResult = device.CreateShader(render::ShaderDescriptor{
        .Source = vertexBytecode.value(),
        .Category = backendArtifact->Category,
        .Stages = render::ShaderStage::Vertex});
    auto pixelResult = device.CreateShader(render::ShaderDescriptor{
        .Source = pixelBytecode.value(),
        .Category = backendArtifact->Category,
        .Stages = render::ShaderStage::Pixel});
    ASSERT_TRUE(vertexResult.HasValue());
    ASSERT_TRUE(pixelResult.HasValue());
    unique_ptr<render::Shader> vertexShader = vertexResult.Release();
    unique_ptr<render::Shader> pixelShader = pixelResult.Release();

    auto targetTexture = render::test::MakeRenderTarget(
        &device,
        kFormat,
        kWidth,
        kHeight,
        render::TextureUse::RenderTarget | render::TextureUse::CopySource);
    ASSERT_TRUE(targetTexture.has_value());
    const render::RenderPassColorAttachmentDescriptor colorAttachment{
        .Format = kFormat,
        .SampleCount = 1,
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store};
    auto renderPassResult = device.CreateRenderPass(
        render::RenderPassDescriptor{.ColorAttachments = std::span{&colorAttachment, 1}});
    ASSERT_TRUE(renderPassResult.HasValue());
    unique_ptr<render::RenderPass> renderPass = renderPassResult.Release();
    render::TextureView* colorViews[]{targetTexture->View.get()};
    auto framebufferResult = device.CreateFramebuffer(render::FramebufferDescriptor{
        .Pass = renderPass.get(),
        .ColorAttachments = colorViews,
        .Width = kWidth,
        .Height = kHeight,
        .Layers = 1});
    ASSERT_TRUE(framebufferResult.HasValue());
    unique_ptr<render::Framebuffer> framebuffer = framebufferResult.Release();

    const render::VertexBufferLayout vertexBufferLayout{
        .Binding = 0,
        .ArrayStride = sizeof(float) * 3,
        .StepMode = render::VertexStepMode::Vertex};
    const render::VertexAttribute vertexAttribute{
        .BufferBinding = 0,
        .Semantic = "POSITION",
        .Format = render::VertexFormat::FLOAT32X3,
        .Location = 0};
    const render::VertexInputState vertexInput{
        .Buffers = std::span{&vertexBufferLayout, 1},
        .Attributes = std::span{&vertexAttribute, 1}};
    const render::ColorTargetState colorTarget = render::ColorTargetState::Default(kFormat);
    render::PrimitiveState primitive = render::PrimitiveState::Default();
    primitive.Cull = render::CullMode::None;
    auto psoResult = device.CreateGraphicsPipelineState(render::GraphicsPipelineStateDescriptor{
        .PipelineLayout = layout.get(),
        .VS = render::ShaderEntry{vertexShader.get(), "VSMain"},
        .PS = render::ShaderEntry{pixelShader.get(), "PSMain"},
        .VertexInput = vertexInput,
        .Primitive = primitive,
        .DepthStencil = std::nullopt,
        .MultiSample = render::MultiSampleState::Default(),
        .ColorTargets = std::span{&colorTarget, 1},
        .CompatibleRenderPass = renderPass.get()});
    ASSERT_TRUE(psoResult.HasValue());
    unique_ptr<render::GraphicsPipelineState> pso = psoResult.Release();

    constexpr std::array<float, 9> triangle{
        0.0f, 0.8f, 0.0f,
        -0.8f, -0.8f, 0.0f,
        0.8f, -0.8f, 0.0f};
    auto vertexBuffer = render::test::MakeUploadBuffer(
        device,
        std::as_bytes(std::span{triangle}),
        render::BufferUse::Vertex);
    ASSERT_TRUE(vertexBuffer.HasValue());
    const render::DeviceDetail detail = device.GetDetail();
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(kFormat);
    const uint64_t rowPitch = Align(
        static_cast<uint64_t>(kWidth) * bytesPerPixel,
        detail.TextureDataPitchAlignment);
    const uint64_t readbackSize = rowPitch * kHeight;
    auto readbackResult = device.CreateBuffer(render::BufferDescriptor{
        .Size = readbackSize,
        .Memory = render::MemoryType::ReadBack,
        .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
    ASSERT_TRUE(readbackResult.HasValue());
    unique_ptr<render::Buffer> readback = readbackResult.Release();
    auto commandResult = device.CreateCommandBuffer(context.Queue);
    ASSERT_TRUE(commandResult.HasValue());
    unique_ptr<render::CommandBuffer> command = commandResult.Release();
    command->Begin();
    const render::ResourceBarrierDescriptor toTarget = render::BarrierTextureDescriptor{
        .Target = targetTexture->Tex.get(),
        .Before = render::TextureState::Undefined,
        .After = render::TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&toTarget, 1});
    const render::ColorClearValue clearValue{{0.0f, 0.0f, 0.0f, 1.0f}};
    auto encoderResult = command->BeginRenderPass(render::RenderPassBeginDescriptor{
        .Pass = renderPass.get(),
        .Target = framebuffer.get(),
        .ColorClearValues = std::span{&clearValue, 1}});
    ASSERT_TRUE(encoderResult.HasValue());
    unique_ptr<render::GraphicsCommandEncoder> encoder = encoderResult.Release();
    encoder->SetViewport(Viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1});
    encoder->SetScissor(Rect{0, 0, kWidth, kHeight});
    encoder->BindGraphicsPipelineState(pso.get());
    const render::VertexBufferBinding vertexBinding{
        .Binding = 0,
        .View = render::VertexBufferView{.Target = vertexBuffer.Get(), .Size = triangle.size() * sizeof(float)}};
    encoder->BindVertexBuffers(std::span{&vertexBinding, 1});
    encoder->Draw(3, 1, 0, 0);
    command->EndRenderPass(std::move(encoder));
    const render::ResourceBarrierDescriptor toCopy = render::BarrierTextureDescriptor{
        .Target = targetTexture->Tex.get(),
        .Before = render::TextureState::RenderTarget,
        .After = render::TextureState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, targetTexture->Tex.get(), render::SubresourceRange{0, 1, 0, 1});
    command->End();
    render::CommandBuffer* commands[]{command.get()};
    context.Queue->Submit(render::CommandQueueSubmitDescriptor{.CmdBuffers = commands});
    context.Queue->Wait();
    void* mapped = readback->Map(0, readbackSize);
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange(render::BufferRange{0, readbackSize});
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    const auto* center = pixels + rowPitch * (kHeight / 2) + bytesPerPixel * (kWidth / 2);
    EXPECT_EQ(center[0], 255);
    EXPECT_EQ(center[1], 0);
    EXPECT_EQ(center[2], 255);
    EXPECT_EQ(center[3], 255);
    readback->Unmap();
}

void RunComputeJitSmoke(
    render::test::DeviceContext& context,
    render::RenderBackend backend) {
    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());
    const std::optional<shader::ShaderTarget> target =
        render::GetShaderTargetForBackend(backend);
    ASSERT_TRUE(target.has_value());
    const auto source = std::as_bytes(std::span{kComputeSource.data(), kComputeSource.size()});
    const auto contract = jit.DiscoverContractHash(kComputeSourceName, source, target.value());
    ASSERT_TRUE(contract.has_value());
    const auto artifact = jit.Compile(
        MakeComputeRequest(contract.value(), target.value()),
        target.value());
    ASSERT_TRUE(artifact.has_value());
    ASSERT_EQ(artifact->Target, target.value());

    const shader::ShaderArtifactDecodeOptions options{
        .Target = artifact->Target,
        .ExpectedGpuArtifact = artifact->ExpectedGpuArtifact};
    render::Device& device = *context.Device;
    render::BackendShaderArtifactError artifactError;
    std::optional<render::BackendShaderArtifact> backendArtifact =
        render::CreateBackendShaderArtifact(device, artifact->Metadata, options, &artifactError);
    ASSERT_TRUE(backendArtifact.has_value())
        << static_cast<uint32_t>(artifactError.Failure) << ":"
        << static_cast<uint32_t>(artifactError.DecodeFailure);
    const auto computeBytecode =
        backendArtifact->Generic().FindStageBytecode(shader::ShaderStage::Compute);
    ASSERT_TRUE(computeBytecode.has_value());
    unique_ptr<render::PipelineLayout> layout = std::move(backendArtifact->Layout);
    const render::BindingHandle outputBinding = layout->FindBinding("Output");
    ASSERT_TRUE(outputBinding.IsValid());

    auto shaderResult = device.CreateShader(render::ShaderDescriptor{
        .Source = computeBytecode.value(),
        .Category = backendArtifact->Category,
        .Stages = render::ShaderStage::Compute});
    ASSERT_TRUE(shaderResult.HasValue());
    unique_ptr<render::Shader> computeShader = shaderResult.Release();
    auto psoResult = device.CreateComputePipelineState(render::ComputePipelineStateDescriptor{
        .PipelineLayout = layout.get(),
        .CS = render::ShaderEntry{computeShader.get(), "CSMain"}});
    ASSERT_TRUE(psoResult.HasValue());
    unique_ptr<render::ComputePipelineState> pso = psoResult.Release();

    auto outputResult = device.CreateBuffer(render::BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = render::MemoryType::Device,
        .Usage = render::BufferUse::UnorderedAccess | render::BufferUse::CopySource,
        .Hints = render::ResourceHint::None});
    ASSERT_TRUE(outputResult.HasValue());
    unique_ptr<render::Buffer> output = outputResult.Release();
    auto parameterSetResult = device.CreateShaderParameterSet(render::ShaderParameterSetDescriptor{
        .Layout = layout.get(),
        .GroupIndex = 0});
    ASSERT_TRUE(parameterSetResult.HasValue());
    unique_ptr<render::ShaderParameterSet> parameterSet = parameterSetResult.Release();
    ASSERT_TRUE(parameterSet->Set(
        outputBinding,
        0,
        render::ShaderBufferBinding{
            .Target = output.get(),
            .Range = render::BufferRange{0, sizeof(uint32_t)},
            .StructureByteStride = sizeof(uint32_t)}));
    ASSERT_TRUE(parameterSet->FlushWrites());

    auto readbackResult = device.CreateBuffer(render::BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = render::MemoryType::ReadBack,
        .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead,
        .Hints = render::ResourceHint::None});
    ASSERT_TRUE(readbackResult.HasValue());
    unique_ptr<render::Buffer> readback = readbackResult.Release();
    auto commandResult = device.CreateCommandBuffer(context.Queue);
    ASSERT_TRUE(commandResult.HasValue());
    unique_ptr<render::CommandBuffer> command = commandResult.Release();
    command->Begin();
    const render::ResourceBarrierDescriptor toUav = render::BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = render::BufferState::Undefined,
        .After = render::BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto encoderResult = command->BeginComputePass();
    ASSERT_TRUE(encoderResult.HasValue());
    unique_ptr<render::ComputeCommandEncoder> encoder = encoderResult.Release();
    encoder->BindComputePipelineState(pso.get());
    encoder->BindShaderParameterSet(0, parameterSet.get());
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    const render::ResourceBarrierDescriptor toCopy = render::BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = render::BufferState::UnorderedAccess,
        .After = render::BufferState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyBufferToBuffer(readback.get(), 0, output.get(), 0, sizeof(uint32_t));
    command->End();
    render::CommandBuffer* commands[]{command.get()};
    context.Queue->Submit(render::CommandQueueSubmitDescriptor{.CmdBuffers = commands});
    context.Queue->Wait();
    void* mapped = readback->Map(0, sizeof(uint32_t));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange(render::BufferRange{0, sizeof(uint32_t)});
    EXPECT_EQ(*static_cast<const uint32_t*>(mapped), kComputeValue);
    readback->Unmap();
}

TEST(RadRayRuntimeShaderJit, CompilerResultDecodesAndCorruptionFailsClosed) {
    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());
    const auto source = CopyBytes(kGraphicsSource);
    const auto contract = jit.DiscoverContractHash(kSourceName, source, shader::ShaderTarget::DXIL);
    ASSERT_TRUE(contract.has_value());
    const auto artifact = jit.Compile(MakeRequest(contract.value(), shader::ShaderTarget::DXIL), shader::ShaderTarget::DXIL);
    ASSERT_TRUE(artifact.has_value());
    const auto options = shader::ShaderArtifactDecodeOptions{
        .Target = shader::ShaderTarget::DXIL,
        .ExpectedGpuArtifact = artifact->ExpectedGpuArtifact};
    const auto decoded = shader::DecodeDxilShaderArtifact(artifact->Metadata, options);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_FALSE(decoded->Generic().Bytecode().empty());
    auto wrongGpuArtifact = artifact->ExpectedGpuArtifact;
    wrongGpuArtifact.Bytes[0] ^= 1u;
    const auto wrongOptions = shader::ShaderArtifactDecodeOptions{
        .Target = shader::ShaderTarget::DXIL,
        .ExpectedGpuArtifact = wrongGpuArtifact};
    EXPECT_FALSE(shader::DecodeDxilShaderArtifact(artifact->Metadata, wrongOptions).has_value());
}

TEST(RadRayRuntimeShaderJit, InvalidRequestAndArtifactIdentityFailClosed) {
    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());
    const auto source = CopyBytes(kGraphicsSource);
    const auto contract = jit.DiscoverContractHash(kSourceName, source, shader::ShaderTarget::DXIL);
    ASSERT_TRUE(contract.has_value());
    const shader::CompileVariantRequest request = MakeRequest(contract.value(), shader::ShaderTarget::DXIL);

    shader::CompileVariantRequest missingTarget = request;
    missingTarget.Targets = shader::ShaderTargetMask::SPIRV;
    EXPECT_FALSE(jit.Compile(missingTarget, shader::ShaderTarget::DXIL).has_value());

    shader::CompileVariantRequest driftedContract = request;
    driftedContract.ExpectedContract.Bytes[0] ^= 1u;
    EXPECT_FALSE(jit.Compile(driftedContract, shader::ShaderTarget::DXIL).has_value());

    const auto keywordSource = CopyBytes(kKeywordSource);
    constexpr std::string_view keywordSourceName = "runtime_jit/keyword.hlsl";
    const auto keywordContract = jit.DiscoverContractHash(
        keywordSourceName,
        keywordSource,
        shader::ShaderTarget::DXIL);
    ASSERT_TRUE(keywordContract.has_value());
    shader::CompileVariantRequest invalidAssignment{
        .SourceName = string{keywordSourceName},
        .RootSource = keywordSource,
        .Defines = {},
        .Assignments = {{"QUALITY", "invalid"}},
        .Targets = shader::ShaderTargetMask::DXIL,
        .ExpectedContract = keywordContract.value()};
    EXPECT_FALSE(jit.Compile(invalidAssignment, shader::ShaderTarget::DXIL).has_value());

    const auto artifact = jit.Compile(request, shader::ShaderTarget::DXIL);
    ASSERT_TRUE(artifact.has_value());
    shader::ShaderArtifactDecodeOptions options{
        .Target = shader::ShaderTarget::DXIL,
        .ExpectedGpuArtifact = artifact->ExpectedGpuArtifact};
    auto corruptMetadata = artifact->Metadata;
    corruptMetadata[0] = byte{0};
    EXPECT_FALSE(shader::DecodeDxilShaderArtifact(corruptMetadata, options).has_value());
    options.ExpectedToolchainIdentity = 0xdeadbeefull;
    EXPECT_FALSE(shader::DecodeDxilShaderArtifact(artifact->Metadata, options).has_value());
}

TEST(RadRayRuntimeShaderJit, ShaderlibPassMetadataCorruptionFailsClosed) {
    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());

    const std::filesystem::path shaderlibRoot = std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib";
    const vector<byte> source = ReadBytes(shaderlibRoot / "pipelines/forward/forward.hlsl");
    ASSERT_FALSE(source.empty());
    constexpr std::string_view sourceName = "pipelines/forward/forward.hlsl";
    const auto contract = jit.DiscoverContractHash(sourceName, source, shader::ShaderTarget::DXIL);
    ASSERT_TRUE(contract.has_value());

    shader::CompileVariantRequest request{
        .SourceName = string{sourceName},
        .RootSource = source,
        .Defines = {},
        .Assignments = {{"QUALITY", "low"}},
        .Targets = shader::ShaderTargetMask::DXIL,
        .ExpectedContract = contract.value()};

    const auto artifact = jit.Compile(request, shader::ShaderTarget::DXIL);
    ASSERT_TRUE(artifact.has_value());
    auto corruptedMetadata = artifact->Metadata;
    corruptedMetadata[0] = byte{0};
    EXPECT_FALSE(shader::DecodeDxilShaderArtifact(
                     corruptedMetadata,
                     shader::ShaderArtifactDecodeOptions{
                         .Target = shader::ShaderTarget::DXIL,
                         .ExpectedGpuArtifact = artifact->ExpectedGpuArtifact})
                     .has_value());
}

TEST(RadRayRuntimeShaderJit, FixtureCaseReportCoversTargetNativeJitFacts) {
    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());

    struct FixtureCase {
        std::string_view Name;
        shader::ShaderTargetMask Targets;
    };
    constexpr FixtureCase cases[] = {
        {"no_resource_graphics", shader::ShaderTargetMask::All},
        {"texture_sampler", shader::ShaderTargetMask::All},
        {"shadow_static_sampler", shader::ShaderTargetMask::All},
        {"multiple_root_constants", shader::ShaderTargetMask::DXIL},
        {"spirv_push_constant", shader::ShaderTargetMask::SPIRV},
        {"target_specific_bindings", shader::ShaderTargetMask::All},
        {"compute", shader::ShaderTargetMask::All},
    };
    const std::filesystem::path sourceRoot = std::filesystem::path{RADRAY_PROJECT_DIR} /
                                             "modules/render/tests/data/shader_sources";

    for (const FixtureCase& fixtureCase : cases) {
        const auto fixture = std::find_if(
            render::test::GetShaderContractFixtures().begin(),
            render::test::GetShaderContractFixtures().end(),
            [&](const render::test::ShaderContractFixture& value) noexcept {
                return value.Name == fixtureCase.Name;
            });
        ASSERT_NE(fixture, render::test::GetShaderContractFixtures().end());
        const string sourceFile = string{fixture->Name} + ".hlsl";
        const vector<byte> source = ReadBytes(sourceRoot / sourceFile);
        ASSERT_FALSE(source.empty()) << fixtureCase.Name;
        const string sourceName = "fixtures/" + sourceFile;
        const auto contract = jit.DiscoverContractHash(sourceName, source, shader::ShaderTarget::DXIL);
        ASSERT_TRUE(contract.has_value()) << fixtureCase.Name;

        const size_t expectedBindingCount = static_cast<size_t>(std::count_if(
            fixture->Bindings.begin(),
            fixture->Bindings.end(),
            [](const render::test::FixtureBindingFact& value) noexcept {
                return value.Kind != render::test::FixtureResourceKind::RootConstant;
            }));
        const size_t rootFactCount = static_cast<size_t>(std::count_if(
            fixture->Bindings.begin(),
            fixture->Bindings.end(),
            [](const render::test::FixtureBindingFact& value) noexcept {
                return value.Kind == render::test::FixtureResourceKind::RootConstant;
            }));

        for (const shader::ShaderTarget target : {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
            if (!shader::HasTarget(fixtureCase.Targets, target)) {
                continue;
            }
            SCOPED_TRACE(fmt::format("{} target {}", fixtureCase.Name, static_cast<uint32_t>(target)));
            const auto artifact = jit.Compile(
                shader::CompileVariantRequest{
                    .SourceName = sourceName,
                    .RootSource = source,
                    .Defines = {},
                    .Assignments = {},
                    .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)),
                    .ExpectedContract = contract.value()},
                target);
            ASSERT_TRUE(artifact.has_value());
            shader::ShaderArtifactDecodeOptions options{
                .Target = target,
                .ExpectedGpuArtifact = artifact->ExpectedGpuArtifact,
                .ExpectedToolchainIdentity = 0x0000000001090211ull};
            shader::ShaderArtifactDecodeError error = shader::ShaderArtifactDecodeError::None;
            std::optional<shader::ShaderArtifactView> generic;
            // Counted off the resolved layout of whichever target this lane is: the two resolved
            // types are separate by design, so the test compares the counts rather than trying to
            // hold one common layout value.
            std::optional<size_t> resolvedBindingCount;
            std::optional<size_t> resolvedPushCount;
            if (target == shader::ShaderTarget::DXIL) {
#if defined(RADRAY_ENABLE_D3D12)
                const auto typed = shader::DecodeDxilShaderArtifact(artifact->Metadata, options, &error);
                ASSERT_TRUE(typed.has_value()) << static_cast<uint32_t>(error);
                generic = typed->Generic();
                // Native layout creation consumes the resolved layout, so the test resolves first;
                // no modifiers, because this asserts what the compiler published.
                const auto resolved = render::ResolveD3D12Layout(typed.value());
                ASSERT_TRUE(resolved.has_value());
                resolvedBindingCount = resolved->Bindings.size();
                resolvedPushCount = resolved->PushConstants.size();
#else
                GTEST_SKIP() << "D3D12 is disabled";
#endif
            } else {
#if defined(RADRAY_ENABLE_VULKAN)
                const auto typed = shader::DecodeSpirvShaderArtifact(artifact->Metadata, options, &error);
                ASSERT_TRUE(typed.has_value()) << static_cast<uint32_t>(error);
                generic = typed->Generic();
                const auto resolved = render::ResolveVulkanLayout(typed.value());
                ASSERT_TRUE(resolved.has_value());
                resolvedBindingCount = resolved->Bindings.size();
                resolvedPushCount = resolved->PushBlock.has_value() ? 1u : 0u;
#else
                GTEST_SKIP() << "Vulkan is disabled";
#endif
            }
            ASSERT_TRUE(generic.has_value());
            ASSERT_TRUE(resolvedBindingCount.has_value());
            ASSERT_TRUE(resolvedPushCount.has_value());
            EXPECT_EQ(generic->Entries().size(), fixture->Entries.size());
            EXPECT_EQ(generic->Bindings().size(), expectedBindingCount);
            EXPECT_EQ(resolvedBindingCount.value(), expectedBindingCount);
            const size_t expectedRootCount = fixture->HasSingleSpirvPushBlock
                                                 ? target == shader::ShaderTarget::SPIRV ? 1u : 0u
                                             : target == shader::ShaderTarget::DXIL ? rootFactCount
                                                                                    : 0u;
            EXPECT_EQ(generic->RootConstants().size(), expectedRootCount);
            EXPECT_EQ(resolvedPushCount.value(), expectedRootCount);
            for (const render::test::FixtureBindingFact& expected : fixture->Bindings) {
                if (expected.Kind == render::test::FixtureResourceKind::RootConstant) {
                    continue;
                }
                const auto binding = generic->FindBinding(expected.Name);
                ASSERT_TRUE(binding.has_value());
                EXPECT_EQ(
                    binding->Record.Group,
                    target == shader::ShaderTarget::DXIL ? expected.D3D12Group : expected.VulkanSet);
                EXPECT_EQ(
                    binding->Record.Binding,
                    target == shader::ShaderTarget::DXIL ? expected.D3D12Binding : expected.VulkanBinding);
            }
        }
    }
}

TEST(RadRayRuntimeShaderJit, D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, context)) {
        GTEST_SKIP() << "D3D12 is unavailable on this machine";
    }
    RunGraphicsJitSmoke(context, render::RenderBackend::D3D12);
#else
    GTEST_SKIP() << "D3D12 is disabled";
#endif
}

TEST(RadRayRuntimeShaderJit, ExplicitRootSignatureD3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, context)) {
        GTEST_SKIP() << "D3D12 is unavailable on this machine";
    }

    ShaderJit jit{ShaderIncludePaths()};
    ASSERT_TRUE(jit.IsAvailable());
    const string sourceName = "fixtures/shadow_static_sampler.hlsl";
    const vector<byte> source = ReadBytes(
        std::filesystem::path{RADRAY_PROJECT_DIR} /
        "modules/render/tests/data/shader_sources/shadow_static_sampler.hlsl");
    ASSERT_FALSE(source.empty());
    const auto contract = jit.DiscoverContractHash(sourceName, source, shader::ShaderTarget::DXIL);
    ASSERT_TRUE(contract.has_value());
    const auto artifact = jit.Compile(
        shader::CompileVariantRequest{
            .SourceName = sourceName,
            .RootSource = source,
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::DXIL,
            .ExpectedContract = contract.value()},
        shader::ShaderTarget::DXIL);
    ASSERT_TRUE(artifact.has_value());
    const shader::ShaderArtifactDecodeOptions options{
        .Target = shader::ShaderTarget::DXIL,
        .ExpectedGpuArtifact = artifact->ExpectedGpuArtifact};
    shader::ShaderArtifactDecodeError error = shader::ShaderArtifactDecodeError::None;
    const auto typed = shader::DecodeDxilShaderArtifact(
        artifact->Metadata,
        options,
        &error);
    ASSERT_TRUE(typed.has_value()) << static_cast<uint32_t>(error);
    EXPECT_FALSE(typed->Generic().SerializedRootSignature().empty());
    render::BackendShaderArtifactError artifactError;
    const auto backendArtifact = render::CreateBackendShaderArtifact(
        *context.Device,
        artifact->Metadata,
        options,
        &artifactError);
    ASSERT_TRUE(backendArtifact.has_value())
        << static_cast<uint32_t>(artifactError.Failure) << ":"
        << static_cast<uint32_t>(artifactError.DecodeFailure);
#else
    GTEST_SKIP() << "D3D12 is disabled";
#endif
}

TEST(RadRayRuntimeShaderJit, Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::Vulkan, context)) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    RunGraphicsJitSmoke(context, render::RenderBackend::Vulkan);
#else
    GTEST_SKIP() << "Vulkan is disabled";
#endif
}

TEST(RadRayRuntimeShaderJit, VulkanValidation) {
#if defined(RADRAY_ENABLE_VULKAN)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::Vulkan, context, true)) {
        GTEST_SKIP() << "Vulkan validation layer is unavailable on this machine";
    }
    RunGraphicsJitSmoke(context, render::RenderBackend::Vulkan);
#else
    GTEST_SKIP() << "Vulkan is disabled";
#endif
}

TEST(RadRayRuntimeShaderJit, ComputeD3D12) {
#if defined(RADRAY_ENABLE_D3D12)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, context)) {
        GTEST_SKIP() << "D3D12 is unavailable on this machine";
    }
    RunComputeJitSmoke(context, render::RenderBackend::D3D12);
#else
    GTEST_SKIP() << "D3D12 is disabled";
#endif
}

TEST(RadRayRuntimeShaderJit, ComputeVulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(render::RenderBackend::Vulkan, context)) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    RunComputeJitSmoke(context, render::RenderBackend::Vulkan);
#else
    GTEST_SKIP() << "Vulkan is disabled";
#endif
}

}  // namespace
}  // namespace radray
