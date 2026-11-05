#include <radray/shader_compiler/client.h>

#include "compiler_test_utils.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace radray::shader_compiler {
namespace {

using namespace test;
using shader::ShaderBindingKind;
using shader::ShaderTarget;
using shader::ShaderTargetMask;

constexpr ShaderTarget kTargets[] = {ShaderTarget::DXIL, ShaderTarget::SPIRV};

VariantInput Source(std::string_view name, std::string_view text, ShaderTargetMask targets = ShaderTargetMask::All) {
    return {.SourceName = name, .Source = ToBytes(text), .Targets = targets};
}

string TargetName(ShaderTarget target) {
    return target == ShaderTarget::DXIL ? "DXIL" : "SPIR-V";
}

// --- Envelope ---

TEST(RadRayDxcMetadata, LaneMetadataIdentifiesContractAndBytecode) {
    const Client client;
    const VariantInput input = Source("metadata/identity.hlsl", R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }
)hlsl");
    const auto discovery = client.DiscoverSourceContract(
        shader::SourceContractRequest{.SourceName = string{input.SourceName}, .RootSource = input.Source, .Defines = {}, .Targets = input.Targets},
        {});
    ASSERT_TRUE(discovery.Succeeded()) << discovery.Diagnostics;

    const auto result = DiscoverAndCompile(client, input);

    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    for (const shader::CompileTargetLane& lane : *result.Value) {
        SCOPED_TRACE(TargetName(lane.Target));
        const auto artifact = DecodeLane(result, lane.Target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        EXPECT_EQ(artifact.Value->Envelope().Contract, discovery.Value->Hash);
        EXPECT_EQ(artifact.Value->Envelope().ToolchainIdentity, shader::kShaderToolchainIdentity);
        EXPECT_TRUE(std::ranges::equal(artifact.Value->Bytecode(), lane.Bytecode));
    }
}

// --- Bindings ---

constexpr std::string_view kTextureSamplerSource = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(7, 2) Texture2D<float4> Albedo : register(t3, space1);
VK_BINDING(8, 2) SamplerState Linear : register(s3, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 { return Albedo.SampleLevel(Linear, uv, 0.0); }
)hlsl";

TEST(RadRayDxcMetadata, BindingsUseTargetSpecificCoordinates) {
    struct Expected {
        std::string_view Name;
        ShaderBindingKind Kind;
        uint32_t DxilSpace, DxilRegister, VulkanSet, VulkanBinding;
    };
    constexpr Expected expected[] = {
        {"Albedo", ShaderBindingKind::Texture, 1, 3, 2, 7},
        {"Linear", ShaderBindingKind::Sampler, 1, 3, 2, 8}};
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/texture_sampler.hlsl", kTextureSamplerSource));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        ASSERT_EQ(artifact.Value->Bindings().size(), std::size(expected));
        for (const Expected& want : expected) {
            const auto binding = artifact.Value->FindBinding(want.Name);
            ASSERT_TRUE(binding.has_value()) << want.Name;
            const shader::WireBindingRecord& record = binding->Record;
            EXPECT_EQ(record.Type, static_cast<uint32_t>(want.Kind)) << want.Name;
            EXPECT_EQ(record.Group, target == ShaderTarget::DXIL ? want.DxilSpace : want.VulkanSet) << want.Name;
            EXPECT_EQ(record.Binding, target == ShaderTarget::DXIL ? want.DxilRegister : want.VulkanBinding) << want.Name;
            EXPECT_EQ(record.Count, 1u) << want.Name;
            EXPECT_EQ(record.StageMask, kPixelBit) << want.Name;
            EXPECT_EQ(record.Placement, static_cast<uint32_t>(shader::ShaderBindingPlacement::Table)) << want.Name;
            EXPECT_EQ(record.TypeIndex, shader::kShaderNoType) << want.Name;
        }
    }
}

TEST(RadRayDxcMetadata, NoRootSignaturePolicyLeavesCarrierEmpty) {
    const Client client;

    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/texture_sampler.hlsl", kTextureSamplerSource)), ShaderTarget::DXIL);

    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    EXPECT_TRUE(artifact.Value->SerializedRootSignature().empty());
}

TEST(RadRayDxcMetadata, StageMaskFollowsActualUse) {
    const Client client;
    const auto result = DiscoverAndCompile(client, Source("metadata/stage_use.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct Data { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Data> VertexOnly : register(b0);
VK_BINDING(1, 0) ConstantBuffer<Data> PixelOnly : register(b1);
VK_BINDING(2, 0) ConstantBuffer<Data> Both : register(b2);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0) + VertexOnly.Value + Both.Value; }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return PixelOnly.Value + Both.Value; }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        const auto vertex = artifact.Value->FindBinding("VertexOnly");
        const auto pixel = artifact.Value->FindBinding("PixelOnly");
        const auto both = artifact.Value->FindBinding("Both");
        ASSERT_TRUE(vertex.has_value() && pixel.has_value() && both.has_value());
        EXPECT_EQ(vertex->Record.StageMask, kVertexBit);
        EXPECT_EQ(pixel->Record.StageMask, kPixelBit);
        EXPECT_EQ(both->Record.StageMask, kVertexBit | kPixelBit);
    }
}

TEST(RadRayDxcMetadata, UnusedDeclarationIsNotPublishedOrChecked) {
    const Client client;
    // The dead texture has neither register() nor VK_BINDING; binding checks run after dead-strip.
    const auto result = DiscoverAndCompile(client, Source("metadata/unused.hlsl", R"hlsl(
Texture2D<float4> DeadTexture;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        EXPECT_TRUE(artifact.Value->Bindings().empty());
    }
}

TEST(RadRayDxcMetadata, ResourceArrayKeepsCount) {
    const Client client;
    const auto result = DiscoverAndCompile(client, Source("metadata/resource_array.hlsl", R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(4, 1) Texture2D<float4> Textures[2] : register(t3, space2);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return Textures[0].Load(int3(0, 0, 0)) + float4(position, 1.0); }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return Textures[1].Load(int3(0, 0, 0)); }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        const auto textures = artifact.Value->FindBinding("Textures");
        ASSERT_TRUE(textures.has_value());
        EXPECT_EQ(textures->Record.Count, 2u);
        EXPECT_EQ(textures->Record.StageMask, kVertexBit | kPixelBit);
    }
}

TEST(RadRayDxcMetadata, DxilLaneRejectsMissingRegister) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/missing_register.hlsl", R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(3, 0) Texture2D<float4> Implicit;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return Implicit.Load(int3(0, 0, 0)) + float4(position, 1.0); }
)hlsl", ShaderTargetMask::DXIL));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, DxilLaneRejectsMissingRegisterOnArray) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/missing_array_register.hlsl", R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(3, 0) Texture2D<float4> Implicit[2];

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return Implicit[1].Load(int3(0, 0, 0)) + float4(position, 1.0); }
)hlsl", ShaderTargetMask::DXIL));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, SpirvLaneRejectsMissingVkBinding) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/missing_vk_binding.hlsl", R"hlsl(
Texture2D<float4> Implicit : register(t4);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return Implicit.Load(int3(0, 0, 0)) + float4(position, 1.0); }
)hlsl", ShaderTargetMask::SPIRV));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, SpirvLaneRejectsRegisterThatDiffersBetweenStages) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/register_drift.hlsl", R"hlsl(
#include <core/platform.hlsli>

#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
#define SHARED_REGISTER t0
#else
#define SHARED_REGISTER t1
#endif

VK_BINDING(0, 0) Texture2D<float4> Shared : register(SHARED_REGISTER);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return Shared.Load(int3(0, 0, 0)) + float4(position, 1.0); }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return Shared.Load(int3(0, 0, 0)); }
)hlsl", ShaderTargetMask::SPIRV));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, ActiveDeclarationOutsideContractFails) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/texture_buffer.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct Data { float4 Value; };
VK_BINDING(0, 0) TextureBuffer<Data> Unsupported : register(t0);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return Unsupported.Value + float4(position, 1.0); }
)hlsl"));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

// --- CBuffer type payload ---

TEST(RadRayDxcMetadata, CBufferTypePayloadPinsScalarKindShapeAndOffsets) {
    using Kind = shader::ShaderTypeKind;
    using Scalar = shader::ShaderScalarKind;
    struct Field {
        std::string_view Name;
        Kind Kind;
        Scalar Scalar;
        uint32_t Rows, Columns, ElementCount, Offset, Size, Stride;
        std::string_view Element;
    };
    // HLSL cbuffer packing; every stage reads every field so none is dead-stripped.
    constexpr Field expected[] = {
        {"Transform", Kind::Matrix, Scalar::Float, 4, 4, 1, 0, 64, 64, ""},
        {"Uv", Kind::Vector, Scalar::Float, 1, 2, 1, 64, 8, 8, ""},
        {"Scale", Kind::Scalar, Scalar::Float, 1, 1, 1, 72, 4, 4, ""},
        {"Signed", Kind::Scalar, Scalar::SignedInteger, 1, 1, 1, 76, 4, 4, ""},
        {"Unsigned", Kind::Scalar, Scalar::UnsignedInteger, 1, 1, 1, 80, 4, 4, ""},
        {"Bias", Kind::Vector, Scalar::Float, 1, 3, 1, 84, 12, 12, ""},
        {"Corners", Kind::Array, Scalar::Float, 1, 4, 4, 96, 64, 16, ""},
        {"Leaves", Kind::Array, Scalar::None, 1, 1, 2, 160, 64, 32, "Leaf"}};
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/cbuffer_payload.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct Leaf {
    float4 Color;
    uint4 Codes;
};

struct Shared {
    float4x4 Transform;
    float2 Uv;
    float Scale;
    int Signed;
    uint Unsigned;
    float3 Bias;
    float4 Corners[4];
    Leaf Leaves[2];
};

VK_BINDING(0, 0) ConstantBuffer<Shared> SharedData : register(b0, space0);

float4 ReadEverything() {
    float4 sum = mul(SharedData.Transform, SharedData.Corners[1]);
    sum += SharedData.Uv.x * SharedData.Scale + SharedData.Uv.y;
    sum += float(SharedData.Signed + int(SharedData.Unsigned) + int(SharedData.Leaves[0].Codes.w));
    sum += float4(SharedData.Bias, 0) + SharedData.Leaves[1].Color + SharedData.Corners[3];
    return sum;
}

[shader("vertex")]
float4 VSMain() : SV_Position { return ReadEverything(); }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return ReadEverything(); }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        const auto types = artifact.Value->Types();
        const auto binding = artifact.Value->FindBinding("SharedData");
        ASSERT_TRUE(binding.has_value());
        const uint32_t root = binding->Record.TypeIndex;
        ASSERT_LT(root, types.size());
        EXPECT_EQ(NameOf(*artifact.Value, types[root].Name), "Shared");
        EXPECT_EQ(types[root].Size, 224u);
        for (const Field& want : expected) {
            SCOPED_TRACE(want.Name);
            const auto index = FindType(*artifact.Value, root, want.Name);
            ASSERT_TRUE(index.has_value());
            const shader::WireTypeRecord& field = types[*index];
            EXPECT_EQ(field.Kind, static_cast<uint32_t>(want.Kind));
            EXPECT_EQ(field.ScalarKind, static_cast<uint32_t>(want.Scalar));
            EXPECT_EQ(field.RowCount, want.Rows);
            EXPECT_EQ(field.ColumnCount, want.Columns);
            EXPECT_EQ(field.ElementCount, want.ElementCount);
            EXPECT_EQ(field.Offset, want.Offset);
            EXPECT_EQ(field.Size, want.Size);
            EXPECT_EQ(field.Stride, want.Stride);
            EXPECT_EQ(field.Flags, 0u);
            if (want.Element.empty()) {
                EXPECT_EQ(field.TypeIndex, shader::kShaderNoType);
            } else {
                ASSERT_LT(field.TypeIndex, types.size());
                EXPECT_EQ(NameOf(*artifact.Value, types[field.TypeIndex].Name), want.Element);
            }
        }
        const auto leaf = FindType(*artifact.Value, shader::kShaderNoType, "Leaf");
        ASSERT_TRUE(leaf.has_value());
        EXPECT_EQ(types[*leaf].Size, 32u);
        const auto codes = FindType(*artifact.Value, *leaf, "Codes");
        ASSERT_TRUE(codes.has_value());
        EXPECT_EQ(types[*codes].ScalarKind, static_cast<uint32_t>(Scalar::UnsignedInteger));
        EXPECT_EQ(types[*codes].ColumnCount, 4u);
        EXPECT_EQ(types[*codes].Offset, 16u);
    }
}

TEST(RadRayDxcMetadata, CrossLaneTypePayloadDisagreementFailsClosed) {
    struct Disputed {
        std::string_view Declarations;
        std::string_view Read;
    };
    // Non-square matrix, bool, and a struct array whose element is not a multiple of 16 bytes.
    constexpr Disputed cases[] = {
        {"float3x4 Disputed;", "SharedData.Disputed[0][0]"},
        {"bool Disputed;", "SharedData.Disputed ? 1.0 : 0.0"},
        {"Pair Disputed[2];", "SharedData.Disputed[1].B"}};
    const Client client;

    for (const Disputed& disputed : cases) {
        SCOPED_TRACE(disputed.Declarations);
        const string source = fmt::format(R"hlsl(
#include <core/platform.hlsli>

struct Pair {{ float A; float B; }};
struct Shared {{
    float4x4 Transform;
    {}
}};

VK_BINDING(0, 0) ConstantBuffer<Shared> SharedData : register(b0, space0);

[shader("vertex")]
float4 VSMain() : SV_Position {{ return mul(SharedData.Transform, float4(1, 1, 1, 1)) + float({}); }}
)hlsl",
                                          disputed.Declarations, disputed.Read);

        const auto result = DiscoverAndCompile(client, Source("metadata/cbuffer_disputed.hlsl", source));

        EXPECT_FALSE(result.Succeeded());
        EXPECT_NE(result.Diagnostics.find("Shared"), string::npos) << result.Diagnostics;
    }
}

TEST(RadRayDxcMetadata, CBuffersOfSameStructShareOnePayloadRoot) {
    const Client client;
    const auto result = DiscoverAndCompile(client, Source("metadata/shared_root.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct SharedRoot { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<SharedRoot> First : register(b0, space0);
VK_BINDING(0, 1) ConstantBuffer<SharedRoot> Second : register(b0, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0) + First.Value; }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return First.Value + Second.Value; }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        const auto first = artifact.Value->FindBinding("First");
        const auto second = artifact.Value->FindBinding("Second");
        ASSERT_TRUE(first.has_value() && second.has_value());
        EXPECT_EQ(first->Record.TypeIndex, second->Record.TypeIndex);
        EXPECT_EQ(FindType(*artifact.Value, shader::kShaderNoType, "SharedRoot"), first->Record.TypeIndex);
    }
}

TEST(RadRayDxcMetadata, NestedStructMemberReferencesOwnedRoot) {
    const Client client;
    const auto result = DiscoverAndCompile(client, Source("metadata/nested_roots.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct InnerRoot { float4 InnerValue; };
struct OuterRoot { InnerRoot Nested; };
VK_BINDING(0, 0) ConstantBuffer<InnerRoot> Inner : register(b0, space0);
VK_BINDING(0, 1) ConstantBuffer<OuterRoot> Outer : register(b0, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0) + Inner.InnerValue; }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return Outer.Nested.InnerValue; }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        const auto inner = artifact.Value->FindBinding("Inner");
        const auto outer = artifact.Value->FindBinding("Outer");
        ASSERT_TRUE(inner.has_value() && outer.has_value());
        const auto nested = FindType(*artifact.Value, outer->Record.TypeIndex, "Nested");
        ASSERT_TRUE(nested.has_value());
        EXPECT_EQ(artifact.Value->Types()[*nested].TypeIndex, inner->Record.TypeIndex);
    }
}

// Type record of a member of the DXIL cbuffer named Lighting.
std::optional<shader::WireTypeRecord> DxilLightingMember(const Client& client, std::string_view source, std::string_view member) {
    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/lighting.hlsl", source, ShaderTargetMask::DXIL), {}), ShaderTarget::DXIL);
    if (!artifact.Succeeded()) {
        ADD_FAILURE() << artifact.Diagnostics;
        return std::nullopt;
    }
    const auto lighting = artifact.Value->FindBinding("Lighting");
    if (!lighting.has_value()) {
        return std::nullopt;
    }
    const auto index = FindType(*artifact.Value, lighting->Record.TypeIndex, member);
    if (!index.has_value()) {
        return std::nullopt;
    }
    return artifact.Value->Types()[*index];
}

TEST(RadRayDxcMetadata, PaddedDxilCbufferLeavesDecodeWithoutChangingCompilerMetadata) {
    const Client client;

    const auto direction = DxilLightingMember(client, R"hlsl(
cbuffer Lighting : register(b0) {
    float3 Direction0 : packoffset(c0);
    float3 Direction1 : packoffset(c1);
};
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position + Direction0 + Direction1, 1); }
)hlsl",
                                              "Direction0");

    ASSERT_TRUE(direction.has_value());
    EXPECT_EQ(direction->Offset, 0u);
    EXPECT_EQ(direction->Size, 16u);
    EXPECT_EQ(direction->Stride, 12u);
}

TEST(RadRayDxcMetadata, PaddedDxilVectorSpansSkippedRegister) {
    const Client client;

    const auto direction = DxilLightingMember(client, R"hlsl(
cbuffer Lighting : register(b0) {
    float3 Direction0 : packoffset(c0);
    float3 Direction1 : packoffset(c2);
};
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position + Direction0 + Direction1, 1); }
)hlsl",
                                              "Direction0");

    ASSERT_TRUE(direction.has_value());
    EXPECT_EQ(direction->Size, 32u);
    EXPECT_EQ(direction->Stride, 12u);
}

TEST(RadRayDxcMetadata, PaddedDxilScalarSpansGapToNextField) {
    const Client client;

    const auto brightness = DxilLightingMember(client, R"hlsl(
cbuffer Lighting : register(b0) {
    float Anchor : packoffset(c1.x);
    float Brightness : packoffset(c1.y);
    float3 Tint : packoffset(c2);
};
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position + Tint + float3(Anchor, Brightness, 0), 1); }
)hlsl",
                                               "Brightness");

    ASSERT_TRUE(brightness.has_value());
    EXPECT_EQ(brightness->Offset, 20u);
    EXPECT_EQ(brightness->Size, 12u);
    EXPECT_EQ(brightness->Stride, 4u);
}

// --- Root constants and push blocks ---

std::optional<shader::WireRootConstantRecord> FindRootConstant(const shader::ShaderArtifactView& artifact, std::string_view name) {
    for (const shader::WireRootConstantRecord& record : artifact.RootConstants()) {
        if (NameOf(artifact, record.Name) == name) {
            return record;
        }
    }
    return std::nullopt;
}

TEST(RadRayDxcMetadata, PolicyRootConstantsPublishOneRecordPerParameter) {
    const Client client;

    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/root_constants.hlsl", R"hlsl(
#define RS \
    "RootConstants(num32BitConstants=16, b0, space=0)," \
    "RootConstants(num32BitConstants=4, b1, space=0)"

struct ObjectData { float4x4 Transform; };
struct MaterialData { float4 Tint; };
ConstantBuffer<ObjectData> ObjectConstants : register(b0, space0);
ConstantBuffer<MaterialData> MaterialConstants : register(b1, space0);

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
[RootSignature(RS)]
float4 PSMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
)hlsl", ShaderTargetMask::DXIL), {}), ShaderTarget::DXIL);

    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    EXPECT_FALSE(artifact.Value->SerializedRootSignature().empty());
    ASSERT_EQ(artifact.Value->RootConstants().size(), 2u);
    const auto object = FindRootConstant(*artifact.Value, "ObjectConstants");
    const auto material = FindRootConstant(*artifact.Value, "MaterialConstants");
    ASSERT_TRUE(object.has_value() && material.has_value());
    EXPECT_EQ(object->RegisterSpace, 0u);
    EXPECT_EQ(object->Register, 0u);
    EXPECT_EQ(object->Size, 64u);
    EXPECT_EQ(object->StageMask, kVertexBit | kPixelBit);
    EXPECT_EQ(material->RegisterSpace, 0u);
    EXPECT_EQ(material->Register, 1u);
    EXPECT_EQ(material->Size, 16u);
}

TEST(RadRayDxcMetadata, RootConstantStageMaskFollowsAuthoredVisibility) {
    const Client client;

    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/vertex_root_constant.hlsl", R"hlsl(
#define RS "RootConstants(num32BitConstants=4, b0, space=0, visibility=SHADER_VISIBILITY_VERTEX)"

struct VertexData { float4 Offset; };
ConstantBuffer<VertexData> VertexConstants : register(b0, space0);

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
[RootSignature(RS)]
float4 PSMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
)hlsl", ShaderTargetMask::DXIL), {}), ShaderTarget::DXIL);

    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    const auto constants = FindRootConstant(*artifact.Value, "VertexConstants");
    ASSERT_TRUE(constants.has_value());
    EXPECT_EQ(constants->StageMask, kVertexBit);
}

TEST(RadRayDxcMetadata, SpirvPushBlockOwnsPayloadAndUsesActiveStages) {
    const Client client;

    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/push.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct PushData { float4 Tint; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> PushConstants : register(b0);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return PushConstants.Tint; }
)hlsl", ShaderTargetMask::SPIRV)), ShaderTarget::SPIRV);

    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    EXPECT_FALSE(artifact.Value->FindBinding("PushConstants").has_value());
    const auto push = FindRootConstant(*artifact.Value, "PushConstants");
    ASSERT_TRUE(push.has_value());
    EXPECT_EQ(push->Size, 16u);
    EXPECT_EQ(push->StageMask, kPixelBit);
    ASSERT_LT(push->TypeIndex, artifact.Value->Types().size());
    EXPECT_EQ(NameOf(*artifact.Value, artifact.Value->Types()[push->TypeIndex].Name), "PushData");
}

TEST(RadRayDxcMetadata, MultipleActiveSpirvPushBlocksFail) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/two_push_blocks.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct FirstData { float4 Value; };
struct SecondData { float4 Value; };
VK_PUSH_CONSTANT ConstantBuffer<FirstData> First : register(b0);
VK_PUSH_CONSTANT ConstantBuffer<SecondData> Second : register(b1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0) + First.Value; }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return Second.Value; }
)hlsl", ShaderTargetMask::SPIRV));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, PushDeclarationWithVkBindingFails) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/push_with_binding.hlsl", R"hlsl(
#include <core/platform.hlsli>

struct PushData { float4 Tint; };
VK_BINDING(0, 0) VK_PUSH_CONSTANT ConstantBuffer<PushData> PushConstants : register(b0);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0) + PushConstants.Tint; }
)hlsl", ShaderTargetMask::SPIRV));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

// --- RootSignature policy ---

constexpr std::string_view kStaticSamplerSource = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(1, 4) Texture2D<float4> ColorTexture : register(t0);
VK_BINDING(2, 4) SamplerState ColorSampler : register(s0);

#define RS "DescriptorTable(SRV(t0)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)"

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
[RootSignature(RS)]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 { return ColorTexture.SampleLevel(ColorSampler, uv, 0.0); }
)hlsl";

TEST(RadRayDxcMetadata, DxilStaticSamplerOwnsNoTableSlot) {
    const Client client;

    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/static_sampler.hlsl", kStaticSamplerSource)), ShaderTarget::DXIL);

    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    const auto sampler = artifact.Value->FindBinding("ColorSampler");
    ASSERT_TRUE(sampler.has_value());
    EXPECT_EQ(sampler->Record.Placement, static_cast<uint32_t>(shader::ShaderBindingPlacement::StaticSampler));
    EXPECT_EQ(sampler->Record.SamplerIndex, shader::kShaderNoSampler);
    EXPECT_TRUE(artifact.Value->Samplers().empty());
    EXPECT_FALSE(artifact.Value->SerializedRootSignature().empty());
}

TEST(RadRayDxcMetadata, SpirvStaticSamplerPublishesImmutableState) {
    const Client client;

    const auto artifact = DecodeLane(DiscoverAndCompile(client, Source("metadata/static_sampler.hlsl", kStaticSamplerSource)), ShaderTarget::SPIRV);

    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    const auto sampler = artifact.Value->FindBinding("ColorSampler");
    ASSERT_TRUE(sampler.has_value());
    EXPECT_EQ(sampler->Record.Placement, static_cast<uint32_t>(shader::ShaderBindingPlacement::Table));
    EXPECT_EQ(sampler->Record.Group, 4u);
    EXPECT_EQ(sampler->Record.Binding, 2u);
    ASSERT_LT(sampler->Record.SamplerIndex, artifact.Value->Samplers().size());
    const shader::WireSamplerRecord& state = artifact.Value->Samplers()[sampler->Record.SamplerIndex];
    // FILTER_MIN_MAG_LINEAR_MIP_POINT in Vulkan terms.
    EXPECT_EQ(state.MagFilter, 1u);
    EXPECT_EQ(state.MinFilter, 1u);
    EXPECT_EQ(state.MipmapMode, 0u);
    EXPECT_EQ(state.CompareEnable, 0u);
}

TEST(RadRayDxcMetadata, DxilBytecodeDoesNotEmbedRootSignature) {
    const Client client;
    constexpr std::string_view rts0 = "RTS0";

    const auto result = DiscoverAndCompile(client, Source("metadata/static_sampler.hlsl", kStaticSamplerSource, ShaderTargetMask::DXIL));

    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    const vector<byte>& bytecode = result.Value->front().Bytecode;
    const vector<byte> marker = ToBytes(rts0);
    EXPECT_EQ(std::search(bytecode.begin(), bytecode.end(), marker.begin(), marker.end()), bytecode.end());
}

constexpr std::string_view kSingleTextureWithPolicy = R"hlsl(
Texture2D<float4> ActiveTexture : register(t0);

[RootSignature(RS)]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return ActiveTexture.Load(int3(0, 0, 0)) + float4(position, 1.0); }
)hlsl";

TEST(RadRayDxcMetadata, RootSignatureMayDeclareStableSuperset) {
    const Client client;
    const string source = string{"#define RS \"DescriptorTable(SRV(t0), SRV(t1))\"\n"} + string{kSingleTextureWithPolicy};

    const auto result = DiscoverAndCompile(client, Source("metadata/superset.hlsl", source, ShaderTargetMask::DXIL), {});

    EXPECT_TRUE(result.Succeeded()) << result.Diagnostics;
}

TEST(RadRayDxcMetadata, RootSignatureMustCoverActiveResource) {
    const Client client;
    const string source = string{"#define RS \"DescriptorTable(SRV(t1))\"\n"} + string{kSingleTextureWithPolicy};

    const auto result = DiscoverAndCompile(client, Source("metadata/uncovered.hlsl", source, ShaderTargetMask::DXIL), {});

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, ActiveSamplerOutsidePolicyFails) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/uncovered_sampler.hlsl", R"hlsl(
Texture2D<float4> ActiveTexture : register(t0);
SamplerState ActiveSampler : register(s0);

#define RS "DescriptorTable(SRV(t0)), StaticSampler(s1, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)"

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
[RootSignature(RS)]
float4 PSMain() : SV_Target0 { return ActiveTexture.SampleLevel(ActiveSampler, float2(0.0, 0.0), 0.0); }
)hlsl", ShaderTargetMask::DXIL), {});

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, UnserializableRootSignatureFailsDiscovery) {
    const Client client;

    const auto discovery = client.DiscoverSourceContract("metadata/duplicate_static_sampler.hlsl", ToBytes(R"hlsl(
#define RS \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)"

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }
)hlsl"),
                                                         ShaderTarget::DXIL, {});

    EXPECT_FALSE(discovery.Succeeded());
    EXPECT_FALSE(discovery.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, EntriesWithDifferentRootSignaturesFailDiscovery) {
    const Client client;
    const vector<byte> source = ToBytes(R"hlsl(
[RootSignature("CBV(b0)")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[RootSignature("CBV(b1)")]
[shader("pixel")]
float4 PSMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
)hlsl");

    for (const ShaderTarget target : kTargets) {
        const auto discovery = client.DiscoverSourceContract("metadata/mismatched_policy.hlsl", source, target, {});

        EXPECT_FALSE(discovery.Succeeded()) << TargetName(target);
        EXPECT_FALSE(discovery.Diagnostics.empty()) << TargetName(target);
    }
}

TEST(RadRayDxcMetadata, RootSignatureOnOnlySomeEntriesFails) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/partial_policy.hlsl", R"hlsl(
#define RS "RootConstants(num32BitConstants=4, b0)"

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
float4 PSMain() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
)hlsl", ShaderTargetMask::DXIL), {});

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

// --- Vertex inputs ---

std::optional<shader::WireVertexInputRecord> FindVertexInput(const shader::ShaderArtifactView& artifact, std::string_view semantic, uint32_t index) {
    for (const shader::WireVertexInputRecord& input : artifact.VertexInputs()) {
        if (NameOf(artifact, input.Semantic) == semantic && input.SemanticIndex == index) {
            return input;
        }
    }
    return std::nullopt;
}

TEST(RadRayDxcMetadata, StructVertexInputsAgreeAcrossTargets) {
    struct Expected {
        std::string_view Semantic;
        uint32_t Location;
        uint32_t ComponentCount;
    };
    constexpr Expected expected[] = {{"POSITION", 0, 3}, {"NORMAL", 1, 3}, {"TEXCOORD", 2, 2}};
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/struct_vertex_input.hlsl", R"hlsl(
struct VertexInput {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
float4 VSMain(VertexInput input) : SV_Position { return float4(input.Position + input.Normal, input.UV.x); }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        ASSERT_EQ(artifact.Value->VertexInputs().size(), std::size(expected));
        for (const Expected& want : expected) {
            const auto input = FindVertexInput(*artifact.Value, want.Semantic, 0);
            ASSERT_TRUE(input.has_value()) << want.Semantic;
            EXPECT_EQ(input->Location, want.Location) << want.Semantic;
            EXPECT_EQ(input->ComponentType, static_cast<uint32_t>(shader::ShaderVertexComponentType::Float)) << want.Semantic;
            EXPECT_EQ(input->ComponentCount, want.ComponentCount) << want.Semantic;
        }
    }
}

TEST(RadRayDxcMetadata, MatrixVertexInputExpandsToLocationRows) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Source("metadata/matrix_vertex_input.hlsl", R"hlsl(
struct VertexInput {
    float3 Position : POSITION;
    float3x3 Basis : TEXCOORD0;
};

[shader("vertex")]
float4 VSMain(VertexInput input) : SV_Position { return float4(mul(input.Position, input.Basis), 1.0); }
)hlsl"));

    for (const ShaderTarget target : kTargets) {
        SCOPED_TRACE(TargetName(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        ASSERT_EQ(artifact.Value->VertexInputs().size(), 4u);
        for (uint32_t row = 0; row < 3; ++row) {
            const auto input = FindVertexInput(*artifact.Value, "TEXCOORD", row);
            ASSERT_TRUE(input.has_value()) << row;
            EXPECT_EQ(input->Location, row + 1) << row;
            EXPECT_EQ(input->ComponentCount, 3u) << row;
        }
    }
}

// --- SPIR-V bytecode policy ---

TEST(RadRayDxcMetadata, SpirvBytecodeDeclaresNoGoogleExtensions) {
    constexpr uint32_t kSpirvMagic = 0x07230203u;
    constexpr uint32_t kOpExtension = 10;
    const Client client;
    for (const uint8_t debugInfo : {uint8_t{0}, uint8_t{1}}) {
        SCOPED_TRACE(fmt::format("debugInfo={}", debugInfo));
        VariantInput input = Source("metadata/texture_sampler.hlsl", kTextureSamplerSource, ShaderTargetMask::SPIRV);
        input.Policy.DebugInfo = debugInfo;

        const auto result = DiscoverAndCompile(client, input);

        ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
        for (const shader::CompileStageArtifact& stage : result.Value->front().Stages) {
            ASSERT_EQ(stage.Bytecode.size() % sizeof(uint32_t), 0u);
            vector<uint32_t> words(stage.Bytecode.size() / sizeof(uint32_t));
            std::memcpy(words.data(), stage.Bytecode.data(), stage.Bytecode.size());
            ASSERT_GE(words.size(), 5u);
            ASSERT_EQ(words[0], kSpirvMagic);
            for (size_t offset = 5; offset < words.size();) {
                const uint32_t count = words[offset] >> 16;
                ASSERT_GT(count, 0u);
                ASSERT_LE(count, words.size() - offset);
                if ((words[offset] & 0xffffu) == kOpExtension) {
                    const std::string_view operand{reinterpret_cast<const char*>(words.data() + offset + 1), (count - 1) * sizeof(uint32_t)};
                    const std::string_view name = operand.substr(0, operand.find('\0'));
                    EXPECT_FALSE(name.starts_with("SPV_GOOGLE_")) << name;
                }
                offset += count;
            }
        }
    }
}

}  // namespace
}  // namespace radray::shader_compiler

