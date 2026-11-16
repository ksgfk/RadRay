#include <radray/runtime/asset_manager.h>
#include <radray/runtime/pipeline_layout_cache.h>
#include <radray/shader/shader_metadata_wire.h>
#include <radray/shader_reflector/shader_reflector.h>
#include <radray/runtime/material_parameter_binding_map.h>
#include <radray/basic_math.h>
#include "../../render/tests/gpu_test_fixture.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <radray/file.h>
#include <gtest/gtest.h>

namespace radray::shader_reflector {
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
    ShaderVariantArtifacts Variant(string name) const {
        return {std::move(name), DXIL.Value ? &*DXIL.Value : Nullable<const shader::DxilShaderArtifactView*>{nullptr},
                SPIRV.Value ? &*SPIRV.Value : Nullable<const shader::SpirvShaderArtifactView*>{nullptr}};
    }
    vector<byte> DxBytes, VkBytes;
    shader::DiagnosticResult<shader::DxilShaderArtifactView> DXIL;
    shader::DiagnosticResult<shader::SpirvShaderArtifactView> SPIRV;
};

vector<byte> ReorderTypesWithUnusedRoot(vector<byte> bytes) {
    shader::WireMetadataEnvelope envelope{};
    shader::DecodeShaderMetadataEnvelope(std::span<const byte>{bytes}.first(152), envelope);
    vector<shader::ShaderArtifactTypeRecord> types(envelope.TypeRecords.Size / shader::GetShaderMetadataRecordSize(shader::ShaderArtifactTypeRecord{}));
    for (size_t i = 0; i < types.size(); ++i) EXPECT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{bytes}.subspan(envelope.TypeRecords.Offset + i * 44, 44), types[i]));
    const auto root = std::find_if(types.begin(), types.end(), [](const auto& t) { return t.ParentIndex == shader::kShaderNoType; });
    if (root == types.end()) return bytes;
    const auto oldRoot = static_cast<uint32_t>(root - types.begin());
    const auto extraRoot = static_cast<uint32_t>(types.size());
    vector<shader::ShaderArtifactTypeRecord> extra{*root};
    for (const auto& child : types)
        if (child.ParentIndex == oldRoot) {
            auto copy = child;
            copy.ParentIndex = extraRoot;
            extra.push_back(copy);
        }
    types.insert(types.end(), extra.begin(), extra.end());
    const auto reverseIndex = [&](uint32_t& index) { if (index != shader::kShaderNoType) index = static_cast<uint32_t>(types.size() - 1 - index); };
    for (auto& t : types) {
        reverseIndex(t.ParentIndex);
        reverseIndex(t.TypeIndex);
    }
    std::reverse(types.begin(), types.end());
    for (uint32_t offset = envelope.BindingRecords.Offset; offset < envelope.BindingRecords.Offset + envelope.BindingRecords.Size; offset += shader::GetShaderMetadataRecordSize(shader::ShaderArtifactBindingRecord{})) {
        shader::ShaderArtifactBindingRecord b{};
        EXPECT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{bytes}.subspan(offset, 36), b));
        reverseIndex(b.TypeIndex);
        EXPECT_TRUE(shader::EncodeShaderMetadataRecord(b, std::span<byte>{bytes}.subspan(offset, 36)));
    }
    for (uint32_t offset = envelope.RootConstantRecords.Offset; offset < envelope.RootConstantRecords.Offset + envelope.RootConstantRecords.Size; offset += shader::GetShaderMetadataRecordSize(shader::ShaderArtifactRootConstantRecord{})) {
        shader::ShaderArtifactRootConstantRecord p{};
        EXPECT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{bytes}.subspan(offset, 32), p));
        reverseIndex(p.TypeIndex);
        EXPECT_TRUE(shader::EncodeShaderMetadataRecord(p, std::span<byte>{bytes}.subspan(offset, 32)));
    }
    vector<byte> encodedTypes(types.size() * 44);
    for (size_t i = 0; i < types.size(); ++i) EXPECT_TRUE(shader::EncodeShaderMetadataRecord(types[i], std::span<byte>{encodedTypes}.subspan(i * 44, 44)));
    const auto size = static_cast<uint32_t>(types.size() * shader::GetShaderMetadataRecordSize(shader::ShaderArtifactTypeRecord{}));
    bytes.insert(bytes.begin() + envelope.Bytecode.Offset, encodedTypes.begin(), encodedTypes.end());
    envelope.TypeRecords = {envelope.Bytecode.Offset, size};
    envelope.Bytecode.Offset += size;
    envelope.TotalSize += size;
    shader::EncodeShaderMetadataEnvelope(envelope, std::span<byte>{bytes}.first(152));
    return bytes;
}

TEST(ShaderReflector, ReflectsOwnedFamilyWithoutCompiler) {
    Pair pair{"texture_sampler"};
    ASSERT_TRUE(pair.DXIL.Succeeded()) << pair.DXIL.Diagnostics;
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    const auto result = ReflectShaderVariant(pair.Variant("Default"));
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    ASSERT_EQ(result.Value->SharedLayout.Groups.size(), 1u);
    EXPECT_EQ(result.Value->SharedLayout.Groups[0].GroupSlot, 1u);
    EXPECT_EQ(result.Value->SharedLayout.Groups[0].Bindings.size(), 2u);
    ASSERT_EQ(result.Value->Variants.size(), 1u);
    EXPECT_EQ(result.Value->Variants[0].SPIRV->VertexInputs[0].Semantic, "POSITION");
}

TEST(ShaderReflector, RemapsReorderedTypeTablesAndIgnoresUnusedRoots) {
    Pair pair{"nested_types"};
    ASSERT_TRUE(pair.DXIL.Succeeded()) << pair.DXIL.Diagnostics;
    auto bytes = ReorderTypesWithUnusedRoot(pair.VkBytes);
    auto vk = shader::DecodeSpirvShaderArtifact(bytes, Options(bytes, false));
    ASSERT_TRUE(vk.Succeeded()) << vk.Diagnostics;
    auto input = pair.Variant("Default");
    const auto original = ReflectShaderVariant(input);
    input.SPIRV = &*vk.Value;
    const auto reordered = ReflectShaderVariant(input);
    ASSERT_TRUE(original.Succeeded()) << original.Diagnostics;
    ASSERT_TRUE(reordered.Succeeded()) << reordered.Diagnostics;
    EXPECT_EQ(WriteShaderFamilyLayoutBinary(*original.Value), WriteShaderFamilyLayoutBinary(*reordered.Value));
    const auto& layout = reordered.Value->SharedLayout;
    const auto root = layout.Groups[0].Bindings[0].TypeIndex;
    ASSERT_TRUE(root);
    const auto member = FindShaderTypeMember(layout.Types, *root, "Data.Values[1].Weight");
    ASSERT_TRUE(member);
    EXPECT_EQ(member->AbsoluteOffset, 92u);
}

TEST(ShaderReflector, RejectsRealScalarTypeDisagreement) {
    Pair pair{"nested_types"};
    shader::WireMetadataEnvelope envelope{};
    shader::DecodeShaderMetadataEnvelope(std::span<const byte>{pair.VkBytes}.first(152), envelope);
    for (uint32_t offset = envelope.TypeRecords.Offset; offset < envelope.TypeRecords.Offset + envelope.TypeRecords.Size; offset += shader::GetShaderMetadataRecordSize(shader::ShaderArtifactTypeRecord{})) {
        shader::ShaderArtifactTypeRecord t{};
        ASSERT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{pair.VkBytes}.subspan(offset, 44), t));
        if (t.Kind == static_cast<uint32_t>(shader::ShaderTypeKind::Scalar)) {
            t.ScalarKind = static_cast<uint32_t>(shader::ShaderScalarKind::UnsignedInteger);
            ASSERT_TRUE(shader::EncodeShaderMetadataRecord(t, std::span<byte>{pair.VkBytes}.subspan(offset, 44)));
            break;
        }
    }
    auto vk = shader::DecodeSpirvShaderArtifact(pair.VkBytes, Options(pair.VkBytes, false));
    ASSERT_TRUE(vk.Succeeded()) << vk.Diagnostics;
    auto input = pair.Variant("Default");
    input.SPIRV = &*vk.Value;
    EXPECT_FALSE(ReflectShaderVariant(input).Succeeded());
}

TEST(ShaderReflector, RejectsValidButDifferentMemberOffsets) {
    Pair pair{"layout_constants"};
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    const auto& artifact = pair.SPIRV.Value->Generic();
    const auto envelope = artifact.Envelope();
    bool changed = false;
    for (uint32_t offset = envelope.TypeRecords.Offset; offset < envelope.TypeRecords.Offset + envelope.TypeRecords.Size; offset += shader::GetShaderMetadataRecordSize(shader::ShaderArtifactTypeRecord{})) {
        shader::ShaderArtifactTypeRecord type{};
        ASSERT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{pair.VkBytes}.subspan(offset, 44), type));
        if (artifact.GetName(type.Name) != "Gain") continue;
        type.Offset = 4;
        ASSERT_TRUE(shader::EncodeShaderMetadataRecord(type, std::span<byte>{pair.VkBytes}.subspan(offset, 44)));
        changed = true;
    }
    ASSERT_TRUE(changed);
    auto vk = shader::DecodeSpirvShaderArtifact(pair.VkBytes, Options(pair.VkBytes, false));
    ASSERT_TRUE(vk.Succeeded()) << vk.Diagnostics;
    auto input = pair.Variant("OffsetMismatch");
    input.SPIRV = &*vk.Value;
    EXPECT_FALSE(ReflectShaderVariant(input).Succeeded());
}

TEST(ShaderReflector, RejectsValidButDifferentMatrixAndArrayStrides) {
    Pair pair{"layout_constants"};
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    const auto& artifact = pair.SPIRV.Value->Generic();
    const auto block = artifact.FindBinding("MatrixBlock");
    ASSERT_TRUE(block);
    const auto envelope = artifact.Envelope();
    bool changed = false;
    for (uint32_t index = 0; index < artifact.Types().size(); ++index) {
        auto type = artifact.Types()[index];
        if (artifact.GetName(type.Name) == "Matrices") {
            type.MatrixStride = 20;
            type.ElementSize = 52;
            type.Stride = 64;
            type.Size = 116;
            changed = true;
        } else if (index == block->Record.TypeIndex) {
            type.Size = type.ElementSize = 116;
        }
        ASSERT_TRUE(shader::EncodeShaderMetadataRecord(type, std::span<byte>{pair.VkBytes}.subspan(envelope.TypeRecords.Offset + index * 44, 44)));
    }
    ASSERT_TRUE(changed);
    for (uint32_t offset = envelope.BindingRecords.Offset; offset < envelope.BindingRecords.Offset + envelope.BindingRecords.Size; offset += shader::GetShaderMetadataRecordSize(shader::ShaderArtifactBindingRecord{})) {
        shader::ShaderArtifactBindingRecord binding{};
        ASSERT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{pair.VkBytes}.subspan(offset, 36), binding));
        if (binding.TypeIndex != block->Record.TypeIndex) continue;
        binding.BufferByteSize = 128;
        ASSERT_TRUE(shader::EncodeShaderMetadataRecord(binding, std::span<byte>{pair.VkBytes}.subspan(offset, 36)));
    }
    auto vk = shader::DecodeSpirvShaderArtifact(pair.VkBytes, Options(pair.VkBytes, false));
    ASSERT_TRUE(vk.Succeeded()) << vk.Diagnostics;
    auto input = pair.Variant("StrideMismatch");
    input.SPIRV = &*vk.Value;
    EXPECT_FALSE(ReflectShaderVariant(input).Succeeded());
}

TEST(ShaderReflector, VariantOrderDoesNotChangeFamilyIdsOrBinary) {
    Pair texture{"texture_sampler"}, compute{"compute"};
    array variants{texture.Variant("Texture"), compute.Variant("Compute")};
    const auto first = ReflectShaderFamily(variants);
    std::reverse(variants.begin(), variants.end());
    const auto second = ReflectShaderFamily(variants);
    ASSERT_TRUE(first.Succeeded()) << first.Diagnostics;
    ASSERT_TRUE(second.Succeeded()) << second.Diagnostics;
    EXPECT_EQ(WriteShaderFamilyLayoutBinary(*first.Value), WriteShaderFamilyLayoutBinary(*second.Value));
}

TEST(ShaderReflector, StaticSamplerNeedsPairedState) {
    Pair pair{"shadow_static_sampler"};
    auto input = pair.Variant("Shadow");
    const auto paired = ReflectShaderVariant(input);
    ASSERT_TRUE(paired.Succeeded()) << paired.Diagnostics;
    const auto& bindings = paired.Value->SharedLayout.Groups[0].Bindings;
    EXPECT_TRUE(std::any_of(bindings.begin(), bindings.end(), [](const auto& b) { return b.Binding.ImmutableSampler.has_value(); }));
    input.SPIRV = nullptr;
    EXPECT_FALSE(ReflectShaderVariant(input).Succeeded());
}

TEST(ShaderReflector, ExplicitIdsReserveSlotsBeforeImplicitAssignment) {
    Pair pair{"texture_sampler"};
    ShaderReflectionPolicy policy;
    policy.Bindings.push_back({.Name = "LinearSampler", .GroupSlot = 3, .BindingId = 0, .GroupName = "Material"});
    policy.Bindings.push_back({.Name = "AlbedoTexture", .GroupSlot = 3, .GroupName = "Material"});
    const auto result = ReflectShaderVariant(pair.Variant("Main"), policy);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    const auto& group = result.Value->SharedLayout.Groups[0];
    EXPECT_EQ(group.GroupSlot, 3u);
    EXPECT_EQ(group.Name, "Material");
    EXPECT_EQ(group.Bindings[0].Name, "LinearSampler");
    EXPECT_EQ(group.Bindings[1].Binding.BindingId, 1u);
}

}  // namespace
}  // namespace radray::shader_reflector

namespace radray::shader_reflector {
namespace {

class ShaderLayoutGpuTest : public render::test::BackendTest {};

ShaderAssetDesc GpuAssetDesc(const Pair& pair, const ShaderFamilyLayoutDesc& family, render::RenderBackend backend) {
    ShaderAssetDesc desc;
    desc.Family = family;
    const auto& artifact = backend == render::RenderBackend::D3D12 ? pair.DXIL.Value->Generic() : pair.SPIRV.Value->Generic();
    for (const auto& entry : artifact.Entries()) {
        const auto stage = static_cast<shader::ShaderStage>(entry.Stage);
        const auto bytes = artifact.FindStageBytecode(stage);
        const auto nativeStage = stage == shader::ShaderStage::Compute ? render::ShaderStage::Compute : stage == shader::ShaderStage::Vertex ? render::ShaderStage::Vertex
                                                                                                                                             : render::ShaderStage::Pixel;
        desc.Stages.push_back({0, backend, nativeStage, string{*artifact.GetName(entry.Name)}, vector<byte>{bytes->begin(), bytes->end()}});
    }
    return desc;
}

TEST_P(ShaderLayoutGpuTest, MatrixArrayConstantsAndResourcesReadBack) {
    using namespace render;
    auto& device = GetDevice();
    Pair pair{"layout_constants"};
    ASSERT_TRUE(pair.DXIL.Succeeded()) << pair.DXIL.Diagnostics;
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    auto reflected = ReflectShaderVariant(pair.Variant("Default"));
    ASSERT_TRUE(reflected.Succeeded()) << reflected.Diagnostics;
    // Exercise the independent layout file boundary before creating any RHI objects.
    const auto encoded = WriteShaderFamilyLayoutBinary(*reflected.Value);
    ASSERT_TRUE(encoded);
    ShaderFamilyLayoutDesc family;
    ASSERT_TRUE(ReadShaderFamilyLayoutBinary(*encoded, family));
    auto asset = ShaderAsset::Create(device, GpuAssetDesc(pair, family, device.GetBackend()));
    ASSERT_TRUE(asset);
    PipelineLayoutCache cache{device};
    const auto layout = cache.Acquire(asset->GetLayoutDesc());
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
    render::test::UploadTexture(device, *texture, TextureState::Undefined, render::test::AsBytes(texel));
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
    auto matrixData = render::test::CreateUploadBuffer(device, matrixBytes, BufferUse::CBuffer).Unwrap();
    auto materialData = render::test::CreateUploadBuffer(device, materialBytes, BufferUse::CBuffer).Unwrap();
    auto output = device.CreateBuffer({.Size = 7 * 16, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto set = device.CreateShaderParameterSet({layout->GetDesc().Groups[0].Layout}).Unwrap();
    ASSERT_TRUE(set->Set(asset->FindBinding("Image")->BindingId, 0, view.get()));
    ASSERT_TRUE(set->Set(asset->FindBinding("ImageSampler")->BindingId, 0, sampler.get()));
    ASSERT_TRUE(set->Set(matrixBinding->BindingId, 0, ShaderBufferBinding{matrixData.get(), {0, matrixBytes.size()}, 0}));
    ASSERT_TRUE(set->Set(materialBinding->BindingId, 0, ShaderBufferBinding{materialData.get(), {0, materialBytes.size()}, 0}));
    ASSERT_TRUE(set->Set(outputBinding->BindingId, 0, ShaderBufferBinding{output.get(), {0, 7 * 16}, 16}));
    ASSERT_TRUE(set->FlushWrites());
    auto pipeline = device.CreateComputePipelineState({layout.Get(), *asset->GetStage(0, ShaderStage::Compute)}).Unwrap();
    auto recording = render::test::BeginRecording(device);
    render::test::Barrier(*recording.Commands, {BarrierBufferDescriptor{matrixData.get(), BufferState::HostWrite, BufferState::CBuffer},
                                                BarrierBufferDescriptor{materialData.get(), BufferState::HostWrite, BufferState::CBuffer},
                                                BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess},
                                                BarrierTextureDescriptor{texture.get(), TextureState::CopyDestination, TextureState::ShaderRead}});
    auto pass = recording.Commands->BeginComputePass().Unwrap();
    pass->BindComputePipelineState(pipeline.get());
    pass->BindShaderParameterSet(0, set.get());
    pass->Dispatch(1, 1, 1);
    recording.Commands->EndComputePass(std::move(pass));
    render::test::SubmitAndWait(device, recording);
    const auto values = render::test::FromBytes<float>(render::test::ReadBuffer(device, *output, BufferState::UnorderedAccess, 7 * 16));
    ASSERT_EQ(values.size(), 28u);
    for (uint32_t matrix = 0; matrix < 2; ++matrix)
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t column = 0; column < 3; ++column) EXPECT_FLOAT_EQ(values[(matrix * 3 + row) * 4 + column], float(matrix * 100 + row * 10 + column + 1));
            EXPECT_FLOAT_EQ(values[(matrix * 3 + row) * 4 + 3], 1.0f);
        }
    for (size_t i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(values[24 + i], float((i + 1) * 2));
}

TEST_P(ShaderLayoutGpuTest, ResolvedVertexLocationsDrawKnownColor) {
    using namespace render;
    auto& device = GetDevice();
    Pair pair{"layout_vertex"};
    ASSERT_TRUE(pair.DXIL.Succeeded()) << pair.DXIL.Diagnostics;
    ASSERT_TRUE(pair.SPIRV.Succeeded()) << pair.SPIRV.Diagnostics;
    auto reflected = ReflectShaderVariant(pair.Variant("Default"));
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
    PipelineLayoutCache cache{device};
    const auto layout = cache.Acquire(asset->GetLayoutDesc());
    ASSERT_TRUE(layout);
    VertexInput geometry;
    geometry.Layouts.push_back({0, 24, VertexStepMode::Vertex});
    geometry.Attributes = {{0, 8, "COLOR", 0, VertexFormat::FLOAT32X4}, {0, 0, "POSITION", 0, VertexFormat::FLOAT32X2}};
    ResolvedVertexInputState resolved;
    ASSERT_TRUE(PrepareMaterialVertexInput(*asset->GetVariantInterface(0), geometry, resolved));
    const array<float, 18> vertices{-1, -1, 0, 1, 0, 1, 3, -1, 0, 1, 0, 1, -1, 3, 0, 1, 0, 1};
    auto buffer = render::test::CreateUploadBuffer(device, render::test::AsBytes(vertices), BufferUse::Vertex).Unwrap();
    auto target = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto view = device.CreateTextureView({target.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    primitive.UnclippedDepth = false;
    const auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    auto pipeline = device.CreateGraphicsPipelineState({.PipelineLayout = layout.Get(), .VS = *asset->GetStage(0, ShaderStage::Vertex), .PS = *asset->GetStage(0, ShaderStage::Pixel), .VertexInput = {resolved.Layouts, resolved.Attributes}, .Primitive = primitive, .MultiSample = MultiSampleState::Default(), .ColorTargets = std::span{&color, 1}}).Unwrap();
    auto recording = render::test::BeginRecording(device);
    render::test::Barrier(*recording.Commands, {BarrierBufferDescriptor{buffer.get(), BufferState::HostWrite, BufferState::Vertex},
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
    render::test::SubmitAndWait(device, recording);
    EXPECT_EQ(render::test::FromBytes<uint32_t>(render::test::ReadTexture(device, *target, TextureState::RenderTarget)), vector<uint32_t>(16, 0xff00ff00));
}

INSTANTIATE_TEST_SUITE_P(Backends, ShaderLayoutGpuTest, testing::ValuesIn(render::test::AllBackends), render::test::BackendParamName);

}  // namespace
}  // namespace radray::shader_reflector

namespace radray::shader_reflector {
namespace {
TEST(ShaderReflector, StructRootNamesDoNotDefineAbiIdentity) {
    Pair pair{"nested_types"};
    ASSERT_TRUE(pair.SPIRV.Succeeded());
    shader::WireMetadataEnvelope envelope{};
    shader::DecodeShaderMetadataEnvelope(std::span<const byte>{pair.VkBytes}.first(152), envelope);
    for (uint32_t offset = envelope.TypeRecords.Offset; offset < envelope.TypeRecords.Offset + envelope.TypeRecords.Size; offset += shader::GetShaderMetadataRecordSize(shader::ShaderArtifactTypeRecord{})) {
        shader::ShaderArtifactTypeRecord type{};
        ASSERT_TRUE(shader::DecodeShaderMetadataRecord(std::span<const byte>{pair.VkBytes}.subspan(offset, 44), type));
        if (type.ParentIndex != shader::kShaderNoType) continue;
        type.Name = pair.SPIRV.Value->Generic().Bindings()[0].Name;
        ASSERT_TRUE(shader::EncodeShaderMetadataRecord(type, std::span<byte>{pair.VkBytes}.subspan(offset, 44)));
    }
    auto vk = shader::DecodeSpirvShaderArtifact(pair.VkBytes, Options(pair.VkBytes, false));
    ASSERT_TRUE(vk.Succeeded()) << vk.Diagnostics;
    auto variant = pair.Variant("Renamed");
    variant.SPIRV = &*vk.Value;
    const auto result = ReflectShaderVariant(variant);
    EXPECT_TRUE(result.Succeeded()) << result.Diagnostics;
}

TEST(ShaderReflector, TargetOnlyPushConstantRequiresExplicitPolicy) {
    Pair pair{"spirv_push_constant"};
    ASSERT_TRUE(pair.DXIL.Succeeded());
    ASSERT_TRUE(pair.SPIRV.Succeeded());
    EXPECT_FALSE(ReflectShaderVariant(pair.Variant("Default")).Succeeded());
    const auto allowed = ReflectShaderVariant(pair.Variant("Default"), {.AllowTargetOnlyBindings = true});
    ASSERT_TRUE(allowed.Succeeded()) << allowed.Diagnostics;
    ASSERT_EQ(allowed.Value->SharedLayout.PushConstants.size(), 1u);
    EXPECT_FALSE(allowed.Value->SharedLayout.PushConstants[0].Push.D3D12);
    EXPECT_TRUE(allowed.Value->SharedLayout.PushConstants[0].Push.Vulkan);
    EXPECT_TRUE(allowed.Value->Variants[0].DXIL->ActivePushConstants.empty());
    EXPECT_EQ(allowed.Value->Variants[0].SPIRV->ActivePushConstants.size(), 1u);
}
}  // namespace
}  // namespace radray::shader_reflector
