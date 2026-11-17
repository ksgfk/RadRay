#include <radray/runtime/asset_manager.h>
#include <radray/runtime/pipeline_layout_registry.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/shader/shader_reflector.h>
#include <radray/runtime/material_parameter_binding_map.h>
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

ShaderAssetDesc GpuAssetDesc(const Pair& pair, const shader::ShaderFamilyLayoutDesc& family, gpu::RenderBackend backend) {
    ShaderAssetDesc desc;
    desc.Family = family;
    const auto& artifact = backend == gpu::RenderBackend::D3D12 ? pair.DXIL.Value->Generic() : pair.SPIRV.Value->Generic();
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
    AssetManager assets{device};
    const auto shader = assets.CreateShader(GpuAssetDesc(pair, *reflected.Value, device.GetBackend()));
    ASSERT_TRUE(shader);
    const auto asset = assets.Get(*shader);
    MaterialAssetDesc materialDesc;
    materialDesc.Programs = {{"Color", *shader, 0, "VSMain", "PSMain"}};
    materialDesc.Families = {{*shader, {}, {}}};
    const auto material = assets.CreateMaterial(materialDesc);
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

INSTANTIATE_TEST_SUITE_P(Backends, ShaderLayoutGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

}  // namespace
}  // namespace radray::test
