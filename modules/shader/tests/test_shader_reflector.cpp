#include <radray/shader/shader_compiler_contract.h>
#include <radray/shader/shader_reflector.h>
#include <radray/shader/client.h>

#include "compiler_test_utils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <radray/file.h>
#include <gtest/gtest.h>

namespace radray::shader {
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
}  // namespace radray::shader



namespace radray::shader {
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

struct CompiledDualVariant {
    vector<byte> DxBytes;
    vector<byte> VkBytes;
    std::optional<DxilShaderArtifactView> DXIL;
    std::optional<SpirvShaderArtifactView> SPIRV;

    ShaderVariantArtifacts View(string name, bool includeDxil = true, bool includeSpirv = true) const {
        return {std::move(name),
                includeDxil && DXIL ? &*DXIL : Nullable<const DxilShaderArtifactView*>{nullptr},
                includeSpirv && SPIRV ? &*SPIRV : Nullable<const SpirvShaderArtifactView*>{nullptr}};
    }
};

std::optional<CompiledDualVariant> CompileDual(const Client& client, std::string_view sourceName, std::string_view source, vector<Define> defines = {}) {
    auto compiled = test::DiscoverAndCompile(client, {.SourceName = sourceName, .Source = test::ToBytes(source), .Targets = ShaderTargetMask::All, .Defines = std::move(defines)});
    EXPECT_TRUE(compiled.Succeeded()) << compiled.Diagnostics;
    if (!compiled.Succeeded()) return std::nullopt;
    CompiledDualVariant out;
    for (const auto& lane : *compiled.Value) {
        WireMetadataEnvelope envelope{};
        if (lane.Metadata.size() >= sizeof(envelope)) DecodeShaderMetadataEnvelope(std::span<const byte>{lane.Metadata}.first(152), envelope);
        const ShaderArtifactDecodeOptions opts{lane.Target, envelope.GpuArtifact, kShaderToolchainIdentity};
        if (lane.Target == ShaderTarget::DXIL) {
            out.DxBytes = lane.Metadata;
            auto decoded = DecodeDxilShaderArtifact(out.DxBytes, opts);
            EXPECT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
            if (!decoded.Succeeded()) return std::nullopt;
            out.DXIL = std::move(*decoded.Value);
        } else {
            out.VkBytes = lane.Metadata;
            auto decoded = DecodeSpirvShaderArtifact(out.VkBytes, opts);
            EXPECT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
            if (!decoded.Succeeded()) return std::nullopt;
            out.SPIRV = std::move(*decoded.Value);
        }
    }
    return out;
}

TEST(ShaderReflector, PromotesDxilCBufferToRootConstantsWithExactRootTypeSizeAndFixedArrays) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    constexpr std::string_view kTwoDwordsSource = R"hlsl(
#include <core/platform.hlsli>
struct TwoDwords {
    uint First;
    uint Second;
};
VK_PUSH_CONSTANT ConstantBuffer<TwoDwords> DrawPush : register(b0, space2);
VK_BINDING(0, 0) RWStructuredBuffer<uint> OutBuf : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
    OutBuf[0] = DrawPush.First + DrawPush.Second;
}
)hlsl";
    auto two = CompileDual(client, "reflector/promote_two_dwords.hlsl", kTwoDwordsSource);
    ASSERT_TRUE(two);
    ShaderReflectionPolicy twoPolicy;
    twoPolicy.PushConstants.push_back({.Name = "DrawPush", .PushId = 7, .PromoteDxilCBufferToRootConstants = true});
    const auto twoResult = ReflectShaderVariant(two->View("TwoDwords"), twoPolicy);
    ASSERT_TRUE(twoResult.Succeeded()) << twoResult.Diagnostics;
    ASSERT_EQ(twoResult.Value->SharedLayout.PushConstants.size(), 1u);
    const auto& twoPush = twoResult.Value->SharedLayout.PushConstants[0];
    EXPECT_EQ(twoPush.Name, "DrawPush");
    EXPECT_EQ(twoPush.Push.PushId, 7u);
    EXPECT_EQ(twoPush.Push.Size, 8u);
    ASSERT_TRUE(twoPush.Push.D3D12);
    EXPECT_EQ(twoPush.Push.D3D12->Register, 0u);
    EXPECT_EQ(twoPush.Push.D3D12->Space, 2u);
    EXPECT_TRUE(twoPush.Push.Vulkan);
    ASSERT_TRUE(twoPush.TypeIndex);
    for (const auto& group : twoResult.Value->SharedLayout.Groups) {
        for (const auto& binding : group.Bindings) EXPECT_NE(binding.Name, "DrawPush");
    }
    ASSERT_EQ(twoResult.Value->Variants.size(), 1u);
    EXPECT_EQ(twoResult.Value->Variants[0].DXIL->ActivePushConstants, (vector<uint32_t>{7u}));
    EXPECT_EQ(twoResult.Value->Variants[0].SPIRV->ActivePushConstants, (vector<uint32_t>{7u}));
    EXPECT_EQ(twoResult.Value->Variants[0].DXIL->ActiveBindings.size(), 1u);

    constexpr std::string_view kArraySource = R"hlsl(
#include <core/platform.hlsli>
struct PushArray {
    uint4 Raw[2];
};
VK_PUSH_CONSTANT ConstantBuffer<PushArray> ArrayPush : register(b1, space3);
VK_BINDING(0, 0) RWStructuredBuffer<uint4> OutBuf : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
    OutBuf[0] = ArrayPush.Raw[0] + ArrayPush.Raw[1];
}
)hlsl";
    auto arr = CompileDual(client, "reflector/promote_array.hlsl", kArraySource);
    ASSERT_TRUE(arr);
    ShaderReflectionPolicy arrPolicy;
    arrPolicy.PushConstants.push_back({.Name = "ArrayPush", .PushId = 9, .PromoteDxilCBufferToRootConstants = true});
    const auto arrResult = ReflectShaderVariant(arr->View("Array"), arrPolicy);
    ASSERT_TRUE(arrResult.Succeeded()) << arrResult.Diagnostics;
    ASSERT_EQ(arrResult.Value->SharedLayout.PushConstants.size(), 1u);
    const auto& arrPush = arrResult.Value->SharedLayout.PushConstants[0];
    EXPECT_EQ(arrPush.Name, "ArrayPush");
    EXPECT_EQ(arrPush.Push.PushId, 9u);
    EXPECT_EQ(arrPush.Push.Size, 32u);
    ASSERT_TRUE(arrPush.Push.D3D12);
    EXPECT_EQ(arrPush.Push.D3D12->Register, 1u);
    EXPECT_EQ(arrPush.Push.D3D12->Space, 3u);
    EXPECT_TRUE(arrPush.Push.Vulkan);
    EXPECT_EQ(arrResult.Value->Variants[0].DXIL->ActivePushConstants, (vector<uint32_t>{9u}));
    EXPECT_EQ(arrResult.Value->Variants[0].SPIRV->ActivePushConstants, (vector<uint32_t>{9u}));
    EXPECT_EQ(arrResult.Value->Variants[0].DXIL->ActiveBindings.size(), 1u);
}

TEST(ShaderReflector, AllowDxilOnlyCoversDxilOnlySpirvOnlyAndDualLaneAndRejectsDuplicates) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    constexpr std::string_view kSource = R"hlsl(
#include <core/platform.hlsli>
#if !defined(__spirv__)
struct HelperPush {
    uint BaseInstance;
    uint DrawId;
};
ConstantBuffer<HelperPush> DxHelper : register(b2, space0);
#endif
VK_BINDING(0, 0) RWStructuredBuffer<uint> OutBuf : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
#if !defined(__spirv__)
    OutBuf[0] = DxHelper.BaseInstance + DxHelper.DrawId;
#else
    OutBuf[0] = 1u;
#endif
}
)hlsl";
    auto compiled = CompileDual(client, "reflector/dxil_only_push.hlsl", kSource);
    ASSERT_TRUE(compiled);
    ShaderReflectionPolicy policy;
    policy.AllowTargetOnlyBindings = false;
    policy.PushConstants.push_back({.Name = "DxHelper", .PushId = 3, .PromoteDxilCBufferToRootConstants = true, .AllowDxilOnly = true});

    const auto dxOnly = ReflectShaderVariant(compiled->View("Main", true, false), policy);
    ASSERT_TRUE(dxOnly.Succeeded()) << dxOnly.Diagnostics;
    ASSERT_EQ(dxOnly.Value->SharedLayout.PushConstants.size(), 1u);
    EXPECT_EQ(dxOnly.Value->SharedLayout.PushConstants[0].Push.PushId, 3u);
    EXPECT_TRUE(dxOnly.Value->SharedLayout.PushConstants[0].Push.D3D12);
    EXPECT_FALSE(dxOnly.Value->SharedLayout.PushConstants[0].Push.Vulkan);

    const auto vkOnly = ReflectShaderVariant(compiled->View("Main", false, true), policy);
    ASSERT_TRUE(vkOnly.Succeeded()) << vkOnly.Diagnostics;
    EXPECT_TRUE(vkOnly.Value->SharedLayout.PushConstants.empty());

    const auto dual = ReflectShaderVariant(compiled->View("Main", true, true), policy);
    ASSERT_TRUE(dual.Succeeded()) << dual.Diagnostics;
    ASSERT_EQ(dual.Value->SharedLayout.PushConstants.size(), 1u);
    EXPECT_TRUE(dual.Value->SharedLayout.PushConstants[0].Push.D3D12);
    EXPECT_FALSE(dual.Value->SharedLayout.PushConstants[0].Push.Vulkan);
    EXPECT_EQ(dual.Value->Variants[0].DXIL->ActivePushConstants, (vector<uint32_t>{3u}));
    EXPECT_TRUE(dual.Value->Variants[0].SPIRV->ActivePushConstants.empty());

    auto dupName = policy;
    dupName.PushConstants.push_back({.Name = "DxHelper", .PushId = 4, .PromoteDxilCBufferToRootConstants = true, .AllowDxilOnly = true});
    EXPECT_FALSE(ReflectShaderVariant(compiled->View("Main", false, true), dupName).Succeeded());

    auto dupId = policy;
    dupId.PushConstants.push_back({.Name = "OtherDxHelper", .PushId = 3, .PromoteDxilCBufferToRootConstants = true, .AllowDxilOnly = true});
    EXPECT_FALSE(ReflectShaderVariant(compiled->View("Main", false, true), dupId).Succeeded());

    auto unknownOnDxil = policy;
    unknownOnDxil.PushConstants.push_back({.Name = "MissingDxHelper", .PushId = 8, .PromoteDxilCBufferToRootConstants = true, .AllowDxilOnly = true});
    EXPECT_FALSE(ReflectShaderVariant(compiled->View("Main", true, true), unknownOnDxil).Succeeded());
    EXPECT_FALSE(ReflectShaderVariant(compiled->View("Main", true, false), unknownOnDxil).Succeeded());
}

TEST(ShaderReflector, AllowDxilOnlyMergesVulkanMappingAcrossVariantsAndDropsItWhenAbsent) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    constexpr std::string_view kSource = R"hlsl(
#include <core/platform.hlsli>
struct CommonData {
    uint Value;
};
#if !defined(__spirv__) || defined(USE_VK_PUSH)
VK_PUSH_CONSTANT ConstantBuffer<CommonData> CommonPush : register(b0, space1);
#endif
VK_BINDING(0, 0) RWStructuredBuffer<uint> OutBuf : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
#if !defined(__spirv__) || defined(USE_VK_PUSH)
    OutBuf[0] = CommonPush.Value;
#else
    OutBuf[0] = 0u;
#endif
}
)hlsl";
    auto variantA = CompileDual(client, "reflector/common_push.hlsl", kSource);
    auto variantB = CompileDual(client, "reflector/common_push.hlsl", kSource, {{"USE_VK_PUSH", "1"}});
    ASSERT_TRUE(variantA && variantB);

    ShaderReflectionPolicy policy;
    policy.AllowTargetOnlyBindings = false;
    policy.PushConstants.push_back({.Name = "CommonPush", .PushId = 5, .PromoteDxilCBufferToRootConstants = true, .AllowDxilOnly = true});

    const array both{variantA->View("A"), variantB->View("B")};
    const auto merged = ReflectShaderFamily(both, policy);
    ASSERT_TRUE(merged.Succeeded()) << merged.Diagnostics;
    ASSERT_EQ(merged.Value->SharedLayout.PushConstants.size(), 1u);
    const auto& sharedPush = merged.Value->SharedLayout.PushConstants[0];
    EXPECT_EQ(sharedPush.Name, "CommonPush");
    EXPECT_EQ(sharedPush.Push.PushId, 5u);
    EXPECT_TRUE(sharedPush.Push.D3D12.has_value());
    EXPECT_TRUE(sharedPush.Push.Vulkan.has_value());
    ASSERT_EQ(merged.Value->Variants.size(), 2u);
    EXPECT_EQ(merged.Value->Variants[0].Name, "A");
    EXPECT_EQ(merged.Value->Variants[0].DXIL->ActivePushConstants, (vector<uint32_t>{5u}));
    EXPECT_TRUE(merged.Value->Variants[0].SPIRV->ActivePushConstants.empty());
    EXPECT_EQ(merged.Value->Variants[1].Name, "B");
    EXPECT_EQ(merged.Value->Variants[1].DXIL->ActivePushConstants, (vector<uint32_t>{5u}));
    EXPECT_EQ(merged.Value->Variants[1].SPIRV->ActivePushConstants, (vector<uint32_t>{5u}));

    const auto onlyA = ReflectShaderVariant(variantA->View("A"), policy);
    ASSERT_TRUE(onlyA.Succeeded()) << onlyA.Diagnostics;
    ASSERT_EQ(onlyA.Value->SharedLayout.PushConstants.size(), 1u);
    EXPECT_TRUE(onlyA.Value->SharedLayout.PushConstants[0].Push.D3D12.has_value());
    EXPECT_FALSE(onlyA.Value->SharedLayout.PushConstants[0].Push.Vulkan.has_value());
}
}  // namespace
}  // namespace radray::shader
