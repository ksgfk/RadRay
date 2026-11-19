#include <radray/runtime/material.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/pipeline_layout_registry.h>
#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/shader/shader_reflector.h>
#include <radray/runtime/material_parameter_binding_map.h>
#include <radray/runtime/shader_asset.h>
#include <radray/basic_math.h>
#include "../../gpu_impl/tests/gpu_test_fixture.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <radray/file.h>
#include <gtest/gtest.h>

namespace radray::test {
namespace {

vector<byte> Fixture(std::string_view name, bool dxil) {
    const auto path = std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_artifacts" /
                      (string{name} + (dxil ? ".dxil.bin" : ".spirv.bin"));
    auto bytes = ReadBinaryFile(path);
    EXPECT_TRUE(bytes) << path.string();
    return bytes.value_or(vector<byte>{});
}

shader::ShaderArtifactDecodeOptions Options(std::span<const byte> bytes, bool dxil) {
    shader::WireMetadataEnvelope envelope{};
    if (bytes.size() >= sizeof(envelope)) shader::DecodeShaderMetadataEnvelope(std::span<const byte>{bytes}.first(152), envelope);
    return {dxil ? shader::ShaderTarget::DXIL : shader::ShaderTarget::SPIRV, envelope.GpuArtifact, shader::kShaderToolchainIdentity};
}

class Pair {
public:
    explicit Pair(std::string_view name) : DxBytes(Fixture(name, true)), VkBytes(Fixture(name, false)),
                                           DXIL(shader::DecodeDxilShaderArtifact(DxBytes, Options(DxBytes, true))),
                                           SPIRV(shader::DecodeSpirvShaderArtifact(VkBytes, Options(VkBytes, false))) {}
    shader::ShaderVariantArtifacts Variant(string name) const {
        return {std::move(name), DXIL.Value ? &*DXIL.Value : Nullable<const shader::DxilShaderArtifactView*>{nullptr},
                SPIRV.Value ? &*SPIRV.Value : Nullable<const shader::SpirvShaderArtifactView*>{nullptr}};
    }
    vector<byte> DxBytes, VkBytes;
    shader::DiagnosticResult<shader::DxilShaderArtifactView> DXIL;
    shader::DiagnosticResult<shader::SpirvShaderArtifactView> SPIRV;
};

class ShaderLayoutGpuTest : public gpu::test::BackendTest {};

ShaderAssetDesc GpuAssetDesc(const Pair& pair, const shader::ShaderFamilyLayoutDesc& family, gpu::GpuBackend backend) {
    ShaderAssetDesc desc;
    desc.Family = family;
    const auto& artifact = backend == gpu::GpuBackend::D3D12 ? pair.DXIL.Value->Generic() : pair.SPIRV.Value->Generic();
    for (const auto& entry : artifact.Entries()) {
        const auto stage = static_cast<shader::ShaderStage>(entry.Stage);
        const auto bytes = artifact.FindStageBytecode(stage);
        const auto nativeStage = stage == shader::ShaderStage::Compute ? gpu::ShaderStage::Compute : stage == shader::ShaderStage::Vertex ? gpu::ShaderStage::Vertex
                                                                                                                                             : gpu::ShaderStage::Pixel;
        desc.Stages.push_back({0, backend, nativeStage, string{*artifact.GetName(entry.Name)}, vector<byte>{bytes->begin(), bytes->end()}});
    }
    return desc;
}

TEST_P(ShaderLayoutGpuTest, OfflineAndDeviceSamplerValidation) {
    auto& device = GetDevice();
    shader::ShaderPipelineLayoutDesc layout;
    auto& group = layout.Groups.emplace_back();
    group.Name = "Material";
    group.Bindings.push_back({.Name = "S", .Binding = {
        .Kind = gpu::BindingKind::Sampler, .Stages = gpu::ShaderStage::Pixel,
        .D3D12 = gpu::D3D12BindingDescriptor{gpu::BindingRegisterClass::Sampler},
        .Vulkan = gpu::VulkanBindingDescriptor{}, .ImmutableSampler = gpu::SamplerDescriptor{}}});
    auto& sampler = *group.Bindings[0].Binding.ImmutableSampler;
    EXPECT_TRUE(shader::ValidateShaderPipelineLayoutDesc(layout));
    EXPECT_TRUE(device.CreateSampler(sampler));
    sampler.LodMin = sampler.LodMax + 1;
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(layout));
    EXPECT_FALSE(device.CreateSampler(sampler));
    EXPECT_GT(TakeRhiValidationErrors(), 0u);
    sampler.LodMin = 0;
    sampler.MipLodBias = device.GetCapabilities().Limits.MaxSamplerLodBias + 1;
    EXPECT_TRUE(shader::ValidateShaderPipelineLayoutDesc(layout));
    EXPECT_FALSE(device.CreateSampler(sampler));
    EXPECT_GT(TakeRhiValidationErrors(), 0u);
}

TEST_P(ShaderLayoutGpuTest, MatrixArrayConstantsAndResourcesReadBack) {
    using namespace gpu;
    auto& device = GetDevice();
    Pair pair{"layout_constants"};
    ASSERT_TRUE(pair.DXIL.Succeeded()) << pair.DXIL.Diagnostics;
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    auto reflected = shader::ReflectShaderVariant(pair.Variant("Default"));
    ASSERT_TRUE(reflected.Succeeded()) << reflected.Diagnostics;
    // Exercise the independent layout file boundary before creating any RHI objects.
    const auto encoded = WriteShaderFamilyLayoutBinary(*reflected.Value);
    ASSERT_TRUE(encoded);
    shader::ShaderFamilyLayoutDesc family;
    ASSERT_TRUE(ReadShaderFamilyLayoutBinary(*encoded, family));
    auto asset = ShaderAsset::Create(device, GpuAssetDesc(pair, family, device.GetBackend()));
    ASSERT_TRUE(asset);
    PipelineLayoutRegistry registry{device};
    const auto layout = registry.Acquire(asset->GetLayoutDesc());
    ASSERT_TRUE(layout);
    const auto matrixBinding = asset->FindBinding("MatrixBlock");
    const auto materialBinding = asset->FindBinding("MaterialBlock");
    const auto outputBinding = asset->FindBinding("Output");
    ASSERT_TRUE(matrixBinding && materialBinding && outputBinding);
    EXPECT_EQ(matrixBinding->BufferByteSize, 96u);
    const auto matrices = asset->FindBufferMember("MatrixBlock", "Matrices");
    ASSERT_TRUE(matrices);
    EXPECT_EQ(matrices->Size, 92u);
    EXPECT_EQ(matrices->ElementSize, 44u);
    EXPECT_EQ(matrices->ArrayStride, 48u);
    const auto alignment = std::max<uint64_t>(16, device.GetCapabilities().Detail.CBufferAlignment);
    vector<byte> matrixBytes(static_cast<size_t>(Align(uint64_t{matrixBinding->BufferByteSize}, alignment)), byte{0});
    for (uint32_t matrix = 0; matrix < 2; ++matrix) {
        const auto member = asset->FindBufferMember("MatrixBlock", radray::format("Matrices[{}]", matrix));
        ASSERT_TRUE(member);
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t column = 0; column < 3; ++column) {
                const float value = float(matrix * 100 + row * 10 + column + 1);
                const auto offset = member->AbsoluteOffset + (member->RowMajor ? row : column) * member->MatrixStride + (member->RowMajor ? column : row) * 4;
                std::memcpy(matrixBytes.data() + offset, &value, 4);
            }
    }
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 1, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::Resource | TextureUse::CopyDestination}).Unwrap();
    const uint32_t texel = 0xffffffff;
    gpu::test::UploadTexture(device, *texture, TextureState::Undefined, gpu::test::AsBytes(texel));
    auto view = device.CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::Resource}).Unwrap();
    auto sampler = device.CreateSampler({.AddressS = AddressMode::ClampToEdge, .AddressT = AddressMode::ClampToEdge, .AddressR = AddressMode::ClampToEdge}).Unwrap();
    vector<byte> materialBytes(static_cast<size_t>(Align(uint64_t{materialBinding->BufferByteSize}, alignment)), byte{0});
    const auto gainLocation = asset->FindBufferMember("MaterialBlock", "Gain");
    const auto tintLocation = asset->FindBufferMember("MaterialBlock", "Tint");
    ASSERT_TRUE(gainLocation && tintLocation);
    const float gain = 2;
    const array<float, 4> tint{1, 2, 3, 4};
    std::memcpy(materialBytes.data() + gainLocation->AbsoluteOffset, &gain, sizeof(gain));
    std::memcpy(materialBytes.data() + tintLocation->AbsoluteOffset, tint.data(), sizeof(tint));
    auto matrixData = gpu::test::CreateUploadBuffer(device, matrixBytes, BufferUse::CBuffer).Unwrap();
    auto materialData = gpu::test::CreateUploadBuffer(device, materialBytes, BufferUse::CBuffer).Unwrap();
    auto output = device.CreateBuffer({.Size = 7 * 16, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    const auto group = layout->GetGroupLayout(matrixBinding->Reference.GroupSlot);
    ASSERT_TRUE(group);
    auto set = device.CreateShaderParameterSet({group.Get()}).Unwrap();
    ASSERT_TRUE(set->Set(asset->FindBinding("Image")->Reference.BindingId, 0, view.get()));
    ASSERT_TRUE(set->Set(asset->FindBinding("ImageSampler")->Reference.BindingId, 0, sampler.get()));
    ASSERT_TRUE(set->Set(matrixBinding->Reference.BindingId, 0, ShaderBufferBinding{matrixData.get(), {0, matrixBytes.size()}, 0}));
    ASSERT_TRUE(set->Set(materialBinding->Reference.BindingId, 0, ShaderBufferBinding{materialData.get(), {0, materialBytes.size()}, 0}));
    ASSERT_TRUE(set->Set(outputBinding->Reference.BindingId, 0, ShaderBufferBinding{output.get(), {0, 7 * 16}, 16}));
    ASSERT_TRUE(set->FlushWrites());
    auto pipeline = device.CreateComputePipelineState({&layout->GetLayout(), *asset->GetStage(0, ShaderStage::Compute)}).Unwrap();
    auto recording = gpu::test::BeginRecording(device);
    gpu::test::Barrier(*recording.Commands, {BarrierBufferDescriptor{matrixData.get(), BufferState::HostWrite, BufferState::CBuffer},
                                                BarrierBufferDescriptor{materialData.get(), BufferState::HostWrite, BufferState::CBuffer},
                                                BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess},
                                                BarrierTextureDescriptor{texture.get(), TextureState::CopyDestination, TextureState::ShaderRead}});
    auto pass = recording.Commands->BeginComputePass().Unwrap();
    pass->BindComputePipelineState(pipeline.get());
    pass->BindShaderParameterSet(0, set.get());
    pass->Dispatch(1, 1, 1);
    recording.Commands->EndComputePass(std::move(pass));
    gpu::test::SubmitAndWait(device, recording);
    const auto values = gpu::test::FromBytes<float>(gpu::test::ReadBuffer(device, *output, BufferState::UnorderedAccess, 7 * 16));
    ASSERT_EQ(values.size(), 28u);
    for (uint32_t matrix = 0; matrix < 2; ++matrix)
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t column = 0; column < 3; ++column) EXPECT_FLOAT_EQ(values[(matrix * 3 + row) * 4 + column], float(matrix * 100 + row * 10 + column + 1));
            EXPECT_FLOAT_EQ(values[(matrix * 3 + row) * 4 + 3], 1.0f);
        }
    for (size_t i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(values[24 + i], float((i + 1) * 2));
}

TEST_P(ShaderLayoutGpuTest, ResolvedVertexLocationsDrawKnownColor) {
    using namespace gpu;
    auto& device = GetDevice();
    Pair pair{"layout_vertex"};
    ASSERT_TRUE(pair.DXIL.Succeeded()) << pair.DXIL.Diagnostics;
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    auto reflected = shader::ReflectShaderVariant(pair.Variant("Default"));
    ASSERT_TRUE(reflected.Succeeded()) << reflected.Diagnostics;
    AssetManager assets;
    auto value = ShaderAsset::Create(device, GpuAssetDesc(pair, *reflected.Value, device.GetBackend()));
    ASSERT_TRUE(value);
    const auto shader = assets.Emplace(std::move(*value));
    const auto asset = shader.Get();
    MaterialAssetDesc materialDesc;
    materialDesc.Programs = {{"Color", shader.GetHandle(), 0, "VSMain", "PSMain"}};
    materialDesc.Families = {{shader.GetHandle(), {}, {}}};
    const auto material = CreateMaterial(assets, materialDesc);
    ASSERT_TRUE(material);
    PipelineLayoutRegistry registry{device};
    const auto layout = registry.Acquire(asset->GetLayoutDesc());
    ASSERT_TRUE(layout);
    VertexInput geometry;
    geometry.Layouts.push_back({0, 24, VertexStepMode::Vertex});
    geometry.Attributes = {{0, 8, "COLOR", 0, VertexFormat::FLOAT32X4}, {0, 0, "POSITION", 0, VertexFormat::FLOAT32X2}};
    ResolvedVertexInputState resolved;
    ASSERT_TRUE(PrepareMaterialVertexInput(*asset->GetVariantInterface(0), geometry, resolved));
    const array<float, 18> vertices{-1, -1, 0, 1, 0, 1, 3, -1, 0, 1, 0, 1, -1, 3, 0, 1, 0, 1};
    auto buffer = gpu::test::CreateUploadBuffer(device, gpu::test::AsBytes(vertices), BufferUse::Vertex).Unwrap();
    auto target = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto view = device.CreateTextureView({target.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    primitive.UnclippedDepth = false;
    const auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    auto pipeline = device.CreateGraphicsPipelineState({.PipelineLayout = &layout->GetLayout(), .VS = *asset->GetStage(0, ShaderStage::Vertex), .PS = *asset->GetStage(0, ShaderStage::Pixel), .VertexInput = {resolved.Layouts, resolved.Attributes}, .Primitive = primitive, .MultiSample = MultiSampleState::Default(), .ColorTargets = std::span{&color, 1}}).Unwrap();
    auto recording = gpu::test::BeginRecording(device);
    gpu::test::Barrier(*recording.Commands, {BarrierBufferDescriptor{buffer.get(), BufferState::HostWrite, BufferState::Vertex},
                                                BarrierTextureDescriptor{target.get(), TextureState::Undefined, TextureState::RenderTarget}});
    const RenderPassColorAttachment attachment{view.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 1}}};
    auto pass = recording.Commands->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 4, .Height = 4}).Unwrap();
    pass->BindGraphicsPipelineState(pipeline.get());
    const VertexBufferBinding binding{0, {buffer.get(), 0, sizeof(vertices)}};
    pass->BindVertexBuffers(std::span{&binding, 1});
    pass->SetViewport({0, 0, 4, 4, 0, 1});
    pass->SetScissor({0, 0, 4, 4});
    pass->Draw(3, 1, 0, 0);
    recording.Commands->EndRenderPass(std::move(pass));
    gpu::test::SubmitAndWait(device, recording);
    EXPECT_EQ(gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *target, TextureState::RenderTarget)), vector<uint32_t>(16, 0xff00ff00));
}

TEST_P(ShaderLayoutGpuTest, MaterialInstanceMatrixRowAndColumnMajorParityAndAllValueTypes) {
    using namespace gpu;
    auto& device = GetDevice();
    shader::Client compiler;
    const auto makeSource = [](std::string_view matrixQualifier) {
        return radray::format(R"(
#include <core/platform.hlsli>
VK_BINDING(0, 0) cbuffer MaterialBlock : register(b0, space0) {{
    {} float4x4 Transform;
    int4 I4;
    uint4 U4;
    float3 F3;
    float F1;
    float2 F2;
    int I1;
    uint U1;
    float4 F4;
}};
[shader("vertex")]
float4 VSMain(uint id : SV_VertexID) : SV_Position {{
    float2 pos[3] = {{ float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) }};
    return float4(pos[id], 0.0, 1.0);
}}
struct PSOut {{
    float4 MatrixResult : SV_Target0;
    float4 ValueSummary : SV_Target1;
}};
[shader("pixel")]
PSOut PSMain() {{
    PSOut o;
    o.MatrixResult = mul(Transform, float4(1.0, 2.0, 3.0, 1.0));
    o.ValueSummary = float4(
        F1 + F2.x + F2.y + F3.x + F3.y + F3.z,
        float(I1 + I4.x + I4.y + I4.z + I4.w),
        float(U1 + U4.x + U4.y + U4.z + U4.w),
        F4.x + F4.y + F4.z + F4.w);
    return o;
}}
)",
                              matrixQualifier);
    };

    const array<std::filesystem::path, 1> includePaths{std::filesystem::path{RADRAY_SHADERLIB_DIR}};
    const auto compileShader = [&](std::string_view uriText, std::string_view qualifier) -> std::optional<ShaderAsset> {
        const auto uri = Uri::TryCreate(uriText);
        EXPECT_TRUE(uri);
        const string source = makeSource(qualifier);
        const ShaderCompileDesc desc{.SourceUri = *uri, .Source = std::as_bytes(std::span{source}), .IncludePaths = includePaths, .Variants = {{"Default", {}}}};
        auto compiled = ShaderAsset::Compile(device, compiler, desc);
        EXPECT_TRUE(compiled.Succeeded()) << compiled.Diagnostics;
        return compiled.Succeeded() ? std::move(compiled.Value) : std::nullopt;
    };

    auto rowCompiled = compileShader("virtual://tests/mat_row_major.hlsl", "row_major");
    // 不写限定符验证 HLSL 默认即为 column_major，且与 CPU 列主序直拷一致。
    auto colCompiled = compileShader("virtual://tests/mat_col_major.hlsl", "");
    ASSERT_TRUE(rowCompiled && colCompiled);

    AssetManager assets;
    const auto rowShader = assets.Emplace(std::move(*rowCompiled));
    const auto colShader = assets.Emplace(std::move(*colCompiled));
    ASSERT_TRUE(rowShader && colShader);
    const auto rowMember = rowShader.Get()->FindBufferMember("MaterialBlock", "Transform");
    const auto colMember = colShader.Get()->FindBufferMember("MaterialBlock", "Transform");
    ASSERT_TRUE(rowMember && colMember);
    EXPECT_TRUE(rowMember->RowMajor);
    EXPECT_FALSE(colMember->RowMajor);

    const vector<string> paramNames{"Transform", "I4", "U4", "F3", "F1", "F2", "I1", "U1", "F4"};
    const array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    MaterialAssetDesc matDesc;
    matDesc.Programs = {
        {"RowMajor", rowShader.GetHandle(), 0, "VSMain", "PSMain"},
        {"ColMajor", colShader.GetHandle(), 0, "VSMain", "PSMain"}};
    matDesc.Parameters = {
        {"Transform", identity},
        {"I4", array<int32_t, 4>{0, 0, 0, 0}},
        {"U4", array<uint32_t, 4>{0u, 0u, 0u, 0u}},
        {"F3", array<float, 3>{0, 0, 0}},
        {"F1", 0.0f},
        {"F2", array<float, 2>{0, 0}},
        {"I1", int32_t{0}},
        {"U1", uint32_t{0u}},
        {"F4", array<float, 4>{0, 0, 0, 0}}};
    matDesc.Families = {
        {rowShader.GetHandle(), "MaterialBlock", paramNames},
        {colShader.GetHandle(), "MaterialBlock", paramNames}};
    const auto material = CreateMaterial(assets, matDesc);
    ASSERT_TRUE(material);

    // 直接拷贝 Eigen::Matrix4f（默认列主序）到 array<float, 16>，验证与默认 column_major 和显式 row_major 着色器一致。
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix(0, 1) = 2.0f;
    matrix(0, 3) = 5.0f;
    matrix(1, 2) = 3.0f;
    matrix(1, 3) = 7.0f;
    matrix(2, 3) = 9.0f;
    array<float, 16> asymmetricColMajor{};
    std::memcpy(asymmetricColMajor.data(), matrix.data(), sizeof(asymmetricColMajor));
    MaterialInstance instance{material.GetHandle()};
    ASSERT_TRUE(instance.SetParameter(assets, 0, asymmetricColMajor));
    ASSERT_TRUE(instance.SetParameter(assets, 1, array<int32_t, 4>{-10, 4, -2, 1}));
    ASSERT_TRUE(instance.SetParameter(assets, 2, array<uint32_t, 4>{3u, 5u, 7u, 11u}));
    ASSERT_TRUE(instance.SetParameter(assets, 3, array<float, 3>{1.5f, 2.5f, 3.5f}));
    ASSERT_TRUE(instance.SetParameter(assets, 4, 0.5f));
    ASSERT_TRUE(instance.SetParameter(assets, 5, array<float, 2>{4.0f, 6.0f}));
    ASSERT_TRUE(instance.SetParameter(assets, 6, int32_t{-3}));
    ASSERT_TRUE(instance.SetParameter(assets, 7, uint32_t{13u}));
    ASSERT_TRUE(instance.SetParameter(assets, 8, array<float, 4>{1.0f, 2.0f, 3.0f, 4.0f}));

    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(assets, instance));

    PipelineLayoutRegistry registry{device};
    const auto runFamily = [&](AssetHandle<ShaderAsset> shaderHandle, bool expectTranspose) {
        const auto* shader = assets.Get(shaderHandle).Get();
        const auto* map = material.Get()->GetBindingMap(shaderHandle).Get();
        EXPECT_EQ(map->GetConstantTargets()[0].TransposeMatrix4x4, expectTranspose);
        const auto layout = registry.Acquire(shader->GetLayoutDesc());
        EXPECT_TRUE(layout);
        const auto cbBinding = shader->FindBinding("MaterialBlock");
        EXPECT_TRUE(cbBinding);
        const auto alignment = std::max<uint64_t>(16, device.GetCapabilities().Detail.CBufferAlignment);
        vector<byte> cbBytes(static_cast<size_t>(Align(uint64_t{cbBinding->BufferByteSize}, alignment)), byte{0});
        EXPECT_TRUE(map->WriteConstants(proxy, cbBytes));
        auto cbBuffer = gpu::test::CreateUploadBuffer(device, cbBytes, BufferUse::CBuffer).Unwrap();
        const auto group = layout->GetGroupLayout(cbBinding->Reference.GroupSlot);
        auto set = device.CreateShaderParameterSet({group.Get()}).Unwrap();
        EXPECT_TRUE(set->Set(cbBinding->Reference.BindingId, 0, ShaderBufferBinding{cbBuffer.get(), {0, cbBytes.size()}, 0}));
        EXPECT_TRUE(set->FlushWrites());

        auto rt0 = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 1, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA32_FLOAT, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
        auto rt1 = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 1, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA32_FLOAT, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
        auto rt0View = device.CreateTextureView({rt0.get(), TextureDimension::Dim2D, TextureFormat::RGBA32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
        auto rt1View = device.CreateTextureView({rt1.get(), TextureDimension::Dim2D, TextureFormat::RGBA32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();

        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        primitive.UnclippedDepth = false;
        const array<ColorTargetState, 2> colorTargets{ColorTargetState::Default(TextureFormat::RGBA32_FLOAT), ColorTargetState::Default(TextureFormat::RGBA32_FLOAT)};
        auto pipeline = device.CreateGraphicsPipelineState({
            .PipelineLayout = &layout->GetLayout(),
            .VS = *shader->GetStage(0, ShaderStage::Vertex),
            .PS = *shader->GetStage(0, ShaderStage::Pixel),
            .Primitive = primitive,
            .MultiSample = MultiSampleState::Default(),
            .ColorTargets = colorTargets}).Unwrap();

        auto recording = gpu::test::BeginRecording(device);
        gpu::test::Barrier(*recording.Commands, {
            BarrierBufferDescriptor{cbBuffer.get(), BufferState::HostWrite, BufferState::CBuffer},
            BarrierTextureDescriptor{rt0.get(), TextureState::Undefined, TextureState::RenderTarget},
            BarrierTextureDescriptor{rt1.get(), TextureState::Undefined, TextureState::RenderTarget}});
        const array<RenderPassColorAttachment, 2> attachments{
            RenderPassColorAttachment{rt0View.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 0}}},
            RenderPassColorAttachment{rt1View.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 0}}}};
        auto pass = recording.Commands->BeginRenderPass({.Colors = attachments, .Width = 1, .Height = 1}).Unwrap();
        pass->BindGraphicsPipelineState(pipeline.get());
        pass->BindShaderParameterSet(cbBinding->Reference.GroupSlot, set.get());
        pass->SetViewport({0, 0, 1, 1, 0, 1});
        pass->SetScissor({0, 0, 1, 1});
        pass->Draw(3, 1, 0, 0);
        recording.Commands->EndRenderPass(std::move(pass));
        gpu::test::SubmitAndWait(device, recording);

        const auto out0 = gpu::test::FromBytes<float>(gpu::test::ReadTexture(device, *rt0, TextureState::RenderTarget));
        const auto out1 = gpu::test::FromBytes<float>(gpu::test::ReadTexture(device, *rt1, TextureState::RenderTarget));
        return std::pair{out0, out1};
    };

    const auto [rowMat, rowSummary] = runFamily(rowShader.GetHandle(), true);
    const auto [colMat, colSummary] = runFamily(colShader.GetHandle(), false);
    const Eigen::Vector4f expected = matrix * Eigen::Vector4f{1.0f, 2.0f, 3.0f, 1.0f};
    ASSERT_EQ(rowMat.size(), 4u);
    ASSERT_EQ(colMat.size(), 4u);
    EXPECT_EQ(rowMat, colMat);
    EXPECT_FLOAT_EQ(rowMat[0], expected.x());
    EXPECT_FLOAT_EQ(rowMat[1], expected.y());
    EXPECT_FLOAT_EQ(rowMat[2], expected.z());
    EXPECT_FLOAT_EQ(rowMat[3], expected.w());

    ASSERT_EQ(rowSummary.size(), 4u);
    EXPECT_EQ(rowSummary, colSummary);
    EXPECT_FLOAT_EQ(rowSummary[0], 18.0f);   // 0.5 + 4 + 6 + 1.5 + 2.5 + 3.5
    EXPECT_FLOAT_EQ(rowSummary[1], -10.0f);  // -3 + (-10 + 4 - 2 + 1)
    EXPECT_FLOAT_EQ(rowSummary[2], 39.0f);   // 13 + (3 + 5 + 7 + 11)
    EXPECT_FLOAT_EQ(rowSummary[3], 10.0f);   // 1 + 2 + 3 + 4
}

TEST_P(ShaderLayoutGpuTest, SameLayoutPipelineSwitchPreservesParameterSetAndPushConstants) {
    using namespace gpu;
    auto& device = GetDevice();
    shader::Client compiler;
    constexpr std::string_view kSource = R"hlsl(
#include <core/platform.hlsli>
#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
           "DescriptorTable(CBV(b0, space=0))," \
           "RootConstants(num32BitConstants=4, b1, space=0)"
struct PassData {
    float4 PassBias;
};
struct DrawData {
    float4 DrawTint;
};
VK_BINDING(0, 0) ConstantBuffer<PassData> PassBlock : register(b0, space0);
VK_PUSH_CONSTANT ConstantBuffer<DrawData> DrawBlock : register(b1, space0);

[RootSignature(RS)]
[shader("vertex")]
float4 VSMain(float2 pos : POSITION) : SV_Position {
    return float4(pos, 0.0, 1.0);
}

[RootSignature(RS)]
[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return PassBlock.PassBias + DrawBlock.DrawTint;
}
)hlsl";
    const auto uri = Uri::TryCreate("virtual://tests/same_layout_switch.hlsl");
    ASSERT_TRUE(uri);
    const array<std::filesystem::path, 1> includePaths{std::filesystem::path{RADRAY_SHADERLIB_DIR}};
    const ShaderCompileDesc desc{.SourceUri = *uri, .Source = std::as_bytes(std::span{kSource}), .IncludePaths = includePaths, .Variants = {{"Default", {}}}};
    auto compiled = ShaderAsset::Compile(device, compiler, desc);
    ASSERT_TRUE(compiled.Succeeded()) << compiled.Diagnostics;

    PipelineLayoutRegistry registry{device};
    const auto layout = registry.Acquire(compiled.Value->GetLayoutDesc());
    ASSERT_TRUE(layout);
    const auto passBinding = compiled.Value->FindBinding("PassBlock");
    const auto pushLocation = compiled.Value->FindPushConstant("DrawBlock");
    ASSERT_TRUE(passBinding && pushLocation && pushLocation->ActiveOnDevice);

    // 三个互不重叠的垂直条带（4x1 纹理：x=0、x=1、x=2，x=3 保留清屏值；完全不受 D3D12/Vulkan Y 轴方向影响）。
    const array<float, 36> vertices{
        -1.0f, -1.0f, -0.5f, -1.0f, -0.5f, 1.0f, -1.0f, -1.0f, -0.5f, 1.0f, -1.0f, 1.0f,
        -0.5f, -1.0f,  0.0f, -1.0f,  0.0f, 1.0f, -0.5f, -1.0f,  0.0f, 1.0f, -0.5f, 1.0f,
         0.0f, -1.0f,  0.5f, -1.0f,  0.5f, 1.0f,  0.0f, -1.0f,  0.5f, 1.0f,  0.0f, 1.0f};
    const array<uint32_t, 18> indices{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    auto vb = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{vertices}), BufferUse::Vertex).Unwrap();
    auto ib = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{indices}), BufferUse::Index).Unwrap();

    const array<float, 4> passBias{0.25f, 0.25f, 0.0f, 0.0f};
    const auto alignment = std::max<uint64_t>(16, device.GetCapabilities().Detail.CBufferAlignment);
    vector<byte> cbBytes(static_cast<size_t>(Align(uint64_t{passBinding->BufferByteSize}, alignment)), byte{0});
    std::memcpy(cbBytes.data(), passBias.data(), sizeof(passBias));
    auto cb = gpu::test::CreateUploadBuffer(device, cbBytes, BufferUse::CBuffer).Unwrap();

    const auto group = layout->GetGroupLayout(passBinding->Reference.GroupSlot);
    ASSERT_TRUE(group);
    auto passSet = device.CreateShaderParameterSet({group.Get()}).Unwrap();
    ASSERT_TRUE(passSet->Set(passBinding->Reference.BindingId, 0, ShaderBufferBinding{cb.get(), {0, cbBytes.size()}, 0}));
    ASSERT_TRUE(passSet->FlushWrites());

    VertexInput vertexInput;
    vertexInput.Layouts = {{0, 2 * sizeof(float), VertexStepMode::Vertex}};
    vertexInput.Attributes = {{0, 0, "POSITION", 0, VertexFormat::FLOAT32X2}};
    vertexInput.Streams = {{vb.get(), 0, sizeof(vertices), 0}};
    ResolvedVertexInputState resolved;
    ASSERT_TRUE(PrepareMaterialVertexInput(*compiled.Value->GetVariantInterface(0), vertexInput, resolved));

    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    primitive.UnclippedDepth = false;
    auto allChannels = ColorTargetState::Default(TextureFormat::RGBA32_FLOAT);
    auto noRedChannel = ColorTargetState::Default(TextureFormat::RGBA32_FLOAT);
    noRedChannel.WriteMask = ColorWrite::Green | ColorWrite::Blue | ColorWrite::Alpha;

    auto psoAll = device.CreateGraphicsPipelineState({
        .PipelineLayout = &layout->GetLayout(),
        .VS = *compiled.Value->GetStage(0, ShaderStage::Vertex),
        .PS = *compiled.Value->GetStage(0, ShaderStage::Pixel),
        .VertexInput = {resolved.Layouts, resolved.Attributes},
        .Primitive = primitive,
        .MultiSample = MultiSampleState::Default(),
        .ColorTargets = std::span{&allChannels, 1}}).Unwrap();
    auto psoNoRed = device.CreateGraphicsPipelineState({
        .PipelineLayout = &layout->GetLayout(),
        .VS = *compiled.Value->GetStage(0, ShaderStage::Vertex),
        .PS = *compiled.Value->GetStage(0, ShaderStage::Pixel),
        .VertexInput = {resolved.Layouts, resolved.Attributes},
        .Primitive = primitive,
        .MultiSample = MultiSampleState::Default(),
        .ColorTargets = std::span{&noRedChannel, 1}}).Unwrap();
    EXPECT_EQ(psoAll->GetLayout().Get(), &layout->GetLayout());
    EXPECT_EQ(psoNoRed->GetLayout().Get(), &layout->GetLayout());

    MeshDrawCommandBindingStorage storage;
    const auto makeDraw = [&](GraphicsPipelineState* pso, uint32_t firstIndex, const array<float, 4>& tint) {
        MeshDrawCommand cmd;
        cmd.Pipeline = pso;
        cmd.IndexBuffer = {ib.get(), 0, 4};
        cmd.IndexCount = 6;
        cmd.FirstIndex = firstIndex;
        MeshDrawCommandBindings bindings;
        bindings.VertexBuffers.push_back({0, {vb.get(), 0, sizeof(vertices)}});
        bindings.VertexStreamLayouts.push_back({0, 2 * sizeof(float), VertexStepMode::Vertex});
        MeshPushConstantCommandBinding push{pushLocation->PushId, 0, {}};
        const auto tintBytes = std::as_bytes(std::span{tint});
        push.Data.assign(tintBytes.begin(), tintBytes.end());
        bindings.PushConstants = std::move(push);
        storage.Store(cmd, bindings);
        return cmd;
    };

    const array<float, 4> tint0{0.5f, 0.25f, 0.5f, 1.0f};
    const array<float, 4> tint1{0.0f, 0.5f, 0.25f, 1.0f};
    // cmd0(psoAll, tint0) -> cmd1(psoNoRed, 同 tint0，验证同 Layout 切 PSO 保留 Pass 参数与 Push) -> cmd2(psoNoRed, tint1)
    const array cmds{
        makeDraw(psoAll.get(), 0, tint0),
        makeDraw(psoNoRed.get(), 6, tint0),
        makeDraw(psoNoRed.get(), 12, tint1)};
    const array visible{
        VisibleMeshDrawCommand{.CachedCommand = &cmds[0]},
        VisibleMeshDrawCommand{.CachedCommand = &cmds[1]},
        VisibleMeshDrawCommand{.CachedCommand = &cmds[2]}};

    auto rt = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA32_FLOAT, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto rtView = device.CreateTextureView({rt.get(), TextureDimension::Dim2D, TextureFormat::RGBA32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();

    auto recording = gpu::test::BeginRecording(device);
    gpu::test::Barrier(*recording.Commands, {
        BarrierBufferDescriptor{vb.get(), BufferState::HostWrite, BufferState::Vertex},
        BarrierBufferDescriptor{ib.get(), BufferState::HostWrite, BufferState::Index},
        BarrierBufferDescriptor{cb.get(), BufferState::HostWrite, BufferState::CBuffer},
        BarrierTextureDescriptor{rt.get(), TextureState::Undefined, TextureState::RenderTarget}});
    const RenderPassColorAttachment attachment{rtView.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 0}}};
    auto pass = recording.Commands->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 4, .Height = 1}).Unwrap();
    pass->SetViewport({0, 0, 4, 1, 0, 1});
    pass->SetScissor({0, 0, 4, 1});
    const MeshShaderParameterBinding passParam{passBinding->Reference.GroupSlot, passSet.get(), {}};
    ASSERT_TRUE(SubmitMeshDrawCommands(*pass, visible, {.ParameterSets = std::span{&passParam, 1}}));
    recording.Commands->EndRenderPass(std::move(pass));
    gpu::test::SubmitAndWait(device, recording);

    const auto pixels = gpu::test::FromBytes<float>(gpu::test::ReadTexture(device, *rt, TextureState::RenderTarget));
    ASSERT_EQ(pixels.size(), 16u);
    const auto expectPixel = [&](size_t x, array<float, 4> want) {
        const size_t base = x * 4;
        EXPECT_FLOAT_EQ(pixels[base + 0], want[0]);
        EXPECT_FLOAT_EQ(pixels[base + 1], want[1]);
        EXPECT_FLOAT_EQ(pixels[base + 2], want[2]);
        EXPECT_FLOAT_EQ(pixels[base + 3], want[3]);
    };
    expectPixel(0, {0.75f, 0.50f, 0.50f, 1.0f});  // cmd0: psoAll + PassBias + tint0
    expectPixel(1, {0.00f, 0.50f, 0.50f, 1.0f});  // cmd1: psoNoRed + 保留的 PassBias 与 tint0
    expectPixel(2, {0.00f, 0.75f, 0.25f, 1.0f});  // cmd2: psoNoRed + 保留的 PassBias + 新 tint1
    expectPixel(3, {0.00f, 0.00f, 0.00f, 0.0f});  // 未绘制条带保持清屏值
}

INSTANTIATE_TEST_SUITE_P(Backends, ShaderLayoutGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

}  // namespace
}  // namespace radray::test
