#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/runtime/render_framework/primitive_vertex_layout.h>
#include <radray/runtime/shader_parameters.h>

#include "shader_contract_fixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace radray {
namespace {

vector<byte> ReadBinary(const std::filesystem::path& path) {
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

vector<byte> ReadFixture(std::string_view name, shader::ShaderTarget target) {
    const string suffix = target == shader::ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin";
    return ReadBinary(
        std::filesystem::path{RADRAY_PROJECT_DIR} /
        "modules/render/tests/data/shader_artifacts" /
        (string{name} + suffix));
}

std::optional<size_t> FindFixture(std::string_view name) {
    const auto fixtures = render::test::GetShaderContractFixtures();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        if (fixtures[index].Name == name) {
            return index;
        }
    }
    return std::nullopt;
}

shader::ShaderArtifactDecodeOptions DecodeOptions(
    std::string_view name,
    shader::ShaderTarget target) {
    const std::optional<size_t> index = FindFixture(name);
    EXPECT_TRUE(index.has_value());
    return shader::ShaderArtifactDecodeOptions{
        .Target = target,
        .ExpectedGpuArtifact = render::test::ExpectedGpuArtifact(index.value_or(0), target),
        .ExpectedToolchainIdentity = 0x0000000001090210ull};
}

std::optional<shader::ShaderArtifactView> DecodeGeneric(
    std::string_view name,
    shader::ShaderTarget target = shader::ShaderTarget::DXIL) {
    const vector<byte> blob = ReadFixture(name, target);
    if (blob.empty()) {
        return std::nullopt;
    }
    return shader::DecodeShaderArtifact(blob, DecodeOptions(name, target));
}

template <typename T>
T ReadValue(std::span<const byte> data, size_t offset) {
    EXPECT_LE(offset + sizeof(T), data.size());
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

TEST(RadRayRuntimeMaterial, PrimitiveVertexLayoutResolvesAgainstArtifact) {
    const auto artifact = DecodeGeneric("nested_types");
    ASSERT_TRUE(artifact.has_value());

    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = "POSITION",
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 3,
        .Offset = 0,
        .Stride = 12});
    EXPECT_EQ(primitive.Topology, PrimitiveTopology::TriangleList);

    const auto layout = PrimitiveVertexLayout::FromMeshPrimitive(primitive);
    ASSERT_TRUE(layout.has_value());
    const auto resolved = ResolvePrimitiveVertexLayout(layout.value(), artifact.value());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(render::ValidateVertexInputStateAgainstArtifact(
        resolved->GetState(), artifact.value()));

    PrimitiveVertexLayout missingSemantic = layout.value();
    missingSemantic.Attributes[0].Semantic = "NORMAL";
    EXPECT_FALSE(ResolvePrimitiveVertexLayout(missingSemantic, artifact.value()).has_value());

    PrimitiveVertexLayout unknownFormat = layout.value();
    unknownFormat.Attributes[0].Format = render::VertexFormat::UNKNOWN;
    EXPECT_FALSE(ResolvePrimitiveVertexLayout(unknownFormat, artifact.value()).has_value());

    PrimitiveVertexLayout undeclaredSlot = layout.value();
    undeclaredSlot.Attributes[0].BufferBinding = 1;
    EXPECT_FALSE(ResolvePrimitiveVertexLayout(undeclaredSlot, artifact.value()).has_value());

    MeshPrimitive invalidOffset = primitive;
    invalidOffset.VertexBuffers[0].Offset = 8;
    EXPECT_FALSE(PrimitiveVertexLayout::FromMeshPrimitive(invalidOffset).has_value());

    MeshPrimitive multipleStreams = primitive;
    multipleStreams.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = "NORMAL",
        .SemanticIndex = 0,
        .BufferIndex = 1,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 3,
        .Offset = 0,
        .Stride = 12});
    EXPECT_FALSE(PrimitiveVertexLayout::FromMeshPrimitive(multipleStreams).has_value());
}

TEST(RadRayRuntimeMaterial, TypeTreePacksNestedArraysAndMatrices) {
    const auto artifact = DecodeGeneric("nested_types");
    ASSERT_TRUE(artifact.has_value());
    const auto layout = ShaderParameterLayout::Create(artifact.value());
    ASSERT_TRUE(layout.has_value());
    ASSERT_EQ(layout->Buffers().size(), 1u);
    EXPECT_EQ(layout->Buffers()[0].Size, 96u);
    EXPECT_EQ(layout->ParameterCount(), 3u);

    const ShaderParameterInfo* direction = layout->Find("Direction");
    ASSERT_NE(direction, nullptr);
    EXPECT_EQ(direction->ByteOffset, 64u);
    EXPECT_EQ(direction->Size, 12u);
    EXPECT_EQ(direction->Stride, 16u);
    EXPECT_EQ(direction->ElementCount, 2u);

    ShaderParameterStorage values{&layout.value()};
    const std::span<const byte> initial = values.GetBufferData(0);
    EXPECT_TRUE(std::all_of(initial.begin(), initial.end(), [](byte value) {
        return value == byte{0};
    }));

    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 7.0f;
    ASSERT_TRUE(values.SetMatrix4x4("Transform", transform));
    ASSERT_TRUE(values.SetFloat3("Direction", Eigen::Vector3f{1.0f, 2.0f, 3.0f}, 0));
    ASSERT_TRUE(values.SetFloat("Weight", 4.0f, 0));
    ASSERT_TRUE(values.SetFloat3("Direction", Eigen::Vector3f{5.0f, 6.0f, 7.0f}, 1));
    ASSERT_TRUE(values.SetFloat("Weight", 8.0f, 1));

    const std::span<const byte> packed = values.GetBufferData(0);
    EXPECT_TRUE(ReadValue<Eigen::Matrix4f>(packed, 0).isApprox(transform));
    EXPECT_TRUE(ReadValue<Eigen::Vector3f>(packed, 64).isApprox(Eigen::Vector3f{1.0f, 2.0f, 3.0f}));
    EXPECT_FLOAT_EQ(ReadValue<float>(packed, 76), 4.0f);
    EXPECT_TRUE(ReadValue<Eigen::Vector3f>(packed, 80).isApprox(Eigen::Vector3f{5.0f, 6.0f, 7.0f}));
    EXPECT_FLOAT_EQ(ReadValue<float>(packed, 92), 8.0f);
}

// Pins the positional pairing between cbuffer bindings and cbuffer root types in
// ShaderParameterLayout::Create. A WireBindingRecord carries no TypeIndex, so a swap
// or a reorder on either sequence can only be caught by asserting that each buffer
// reports the group and size of the root that belongs to it.
TEST(RadRayRuntimeMaterial, MultipleCBuffersPairRootTypesWithBindings) {
    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        const auto artifact = DecodeGeneric("multiple_cbuffers", target);
        ASSERT_TRUE(artifact.has_value());
        const auto layout = ShaderParameterLayout::Create(artifact.value());
        ASSERT_TRUE(layout.has_value());
        ASSERT_EQ(layout->Buffers().size(), 2u);

        const auto findBuffer = [&](std::string_view name) -> const ShaderParameterBufferLayout* {
            for (const ShaderParameterBufferLayout& buffer : layout->Buffers()) {
                if (buffer.Name == name) {
                    return &buffer;
                }
            }
            return nullptr;
        };
        const ShaderParameterBufferLayout* first = findBuffer("First");
        const ShaderParameterBufferLayout* second = findBuffer("Second");
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        // FirstRoot is float4x4 + float4; SecondRoot is float4[4] + float.
        EXPECT_EQ(first->Size, 80u);
        EXPECT_EQ(second->Size, 68u);
        EXPECT_EQ(first->Group, 0u);
        EXPECT_EQ(second->Group, 1u);

        const ShaderParameterInfo* transform = layout->Find("FirstTransform");
        const ShaderParameterInfo* tint = layout->Find("FirstTint");
        const ShaderParameterInfo* weight = layout->Find("SecondWeight");
        ASSERT_NE(transform, nullptr);
        ASSERT_NE(tint, nullptr);
        ASSERT_NE(weight, nullptr);
        // Every parameter must land in the buffer whose root actually declares it.
        const uint32_t firstIndex = static_cast<uint32_t>(first - layout->Buffers().data());
        const uint32_t secondIndex = static_cast<uint32_t>(second - layout->Buffers().data());
        EXPECT_EQ(transform->BufferIndex, firstIndex);
        EXPECT_EQ(tint->BufferIndex, firstIndex);
        EXPECT_EQ(weight->BufferIndex, secondIndex);
        EXPECT_EQ(transform->Group, 0u);
        EXPECT_EQ(weight->Group, 1u);
        EXPECT_EQ(weight->ByteOffset, 64u);

        ShaderParameterStorage values{&layout.value()};
        Eigen::Matrix4f transformValue = Eigen::Matrix4f::Identity();
        transformValue(1, 3) = 9.0f;
        ASSERT_TRUE(values.SetMatrix4x4("FirstTransform", transformValue));
        ASSERT_TRUE(values.SetFloat4("FirstTint", Eigen::Vector4f{1.0f, 2.0f, 3.0f, 4.0f}));
        ASSERT_TRUE(values.SetFloat("SecondWeight", 5.0f));
        EXPECT_EQ(values.GetBufferData(firstIndex).size(), 80u);
        EXPECT_EQ(values.GetBufferData(secondIndex).size(), 68u);
        EXPECT_TRUE(
            ReadValue<Eigen::Matrix4f>(values.GetBufferData(firstIndex), 0)
                .isApprox(transformValue));
        EXPECT_TRUE(
            ReadValue<Eigen::Vector4f>(values.GetBufferData(firstIndex), 64)
                .isApprox(Eigen::Vector4f{1.0f, 2.0f, 3.0f, 4.0f}));
        EXPECT_FLOAT_EQ(ReadValue<float>(values.GetBufferData(secondIndex), 64), 5.0f);
        // Writing into one buffer must not touch the other.
        const std::span<const byte> otherBuffer = values.GetBufferData(secondIndex);
        EXPECT_TRUE(std::all_of(
            otherBuffer.begin(),
            otherBuffer.begin() + 64,
            [](byte value) { return value == byte{0}; }));
    }
}

// float4 SecondOffsets[4] is an array of a non-struct element. The wire contract cannot
// name the element type, so the layout exposes it as Raw rather than failing the whole
// program or guessing a kind.
TEST(RadRayRuntimeMaterial, LeafArrayIsExposedAsRawParameter) {
    const auto artifact = DecodeGeneric("multiple_cbuffers");
    ASSERT_TRUE(artifact.has_value());
    const auto layout = ShaderParameterLayout::Create(artifact.value());
    ASSERT_TRUE(layout.has_value());

    const ShaderParameterInfo* offsets = layout->Find("SecondOffsets");
    ASSERT_NE(offsets, nullptr);
    EXPECT_EQ(offsets->Kind, ShaderParameterKind::Raw);
    EXPECT_EQ(offsets->ByteOffset, 0u);
    EXPECT_EQ(offsets->Stride, 16u);
    EXPECT_EQ(offsets->Size, 16u);
    EXPECT_EQ(offsets->ElementCount, 4u);

    ShaderParameterStorage values{&layout.value()};
    const Eigen::Vector4f payload{1.0f, 2.0f, 3.0f, 4.0f};
    const std::span<const byte> payloadBytes{
        reinterpret_cast<const byte*>(payload.data()), sizeof(payload)};
    ASSERT_TRUE(values.SetRaw("SecondOffsets", payloadBytes, 2));
    const std::span<const byte> packed = values.GetBufferData(offsets->BufferIndex);
    EXPECT_TRUE(ReadValue<Eigen::Vector4f>(packed, 32).isApprox(payload));

    // Typed setters must refuse a Raw slot, and Raw must refuse a typed slot.
    EXPECT_FALSE(values.SetFloat4("SecondOffsets", payload, 0));
    EXPECT_FALSE(values.SetRaw("SecondWeight", payloadBytes, 0));
    // Out of range element and oversized payload are both rejected.
    EXPECT_FALSE(values.SetRaw("SecondOffsets", payloadBytes, 4));
    const vector<byte> oversized(17, byte{1});
    EXPECT_FALSE(values.SetRaw("SecondOffsets", oversized, 0));
    EXPECT_FALSE(values.SetRaw("SecondOffsets", {}, 0));
}

TEST(RadRayRuntimeMaterial, InvalidParameterWritesAreTransactional) {
    const auto artifact = DecodeGeneric("nested_types");
    ASSERT_TRUE(artifact.has_value());
    const auto layout = ShaderParameterLayout::Create(artifact.value());
    ASSERT_TRUE(layout.has_value());
    ShaderParameterStorage values{&layout.value()};
    const vector<byte> before{
        values.GetBufferData(0).begin(), values.GetBufferData(0).end()};

    EXPECT_FALSE(values.SetFloat("Unknown", 1.0f));
    EXPECT_FALSE(values.SetFloat4("Direction", Eigen::Vector4f::Ones()));
    EXPECT_FALSE(values.SetFloat3("Direction", Eigen::Vector3f::Ones(), 2));
    EXPECT_EQ(
        vector<byte>(values.GetBufferData(0).begin(), values.GetBufferData(0).end()),
        before);
}

TEST(RadRayRuntimeMaterial, DuplicateFlatParameterNameRejectsLayout) {
    vector<byte> blob = ReadFixture("nested_types", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(blob.empty());
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, blob.data(), sizeof(envelope));
    ASSERT_EQ(envelope.TypeRecords.Size, 8u * sizeof(shader::WireTypeRecord));
    vector<shader::WireTypeRecord> types(8);
    std::memcpy(
        types.data(),
        blob.data() + envelope.TypeRecords.Offset,
        envelope.TypeRecords.Size);
    types[6].Name = types[1].Name;
    std::memcpy(
        blob.data() + envelope.TypeRecords.Offset,
        types.data(),
        envelope.TypeRecords.Size);

    const auto artifact = shader::DecodeShaderArtifact(
        blob,
        DecodeOptions("nested_types", shader::ShaderTarget::DXIL));
    ASSERT_TRUE(artifact.has_value());
    EXPECT_FALSE(ShaderParameterLayout::Create(artifact.value()).has_value());
}

template <typename TArtifact>
void CheckResidencyPolicy(const TArtifact& artifact) {
    const auto regular = render::MakeBackendPipelineLayoutInput(artifact);
    ASSERT_TRUE(regular.has_value());
    ASSERT_EQ(regular->GroupEntries.size(), 1u);
    ASSERT_EQ(regular->GroupEntries[0].size(), 1u);
    EXPECT_EQ(
        regular->GroupEntries[0][0].Type,
        render::ShaderParameterBindingType::CBuffer);

    const uint32_t dynamicGroup = 0;
    const render::ShaderLayoutPolicy dynamicPolicy{
        .DynamicBufferGroups = std::span{&dynamicGroup, 1}};
    const auto dynamic = render::MakeBackendPipelineLayoutInput(artifact, dynamicPolicy);
    ASSERT_TRUE(dynamic.has_value());
    EXPECT_EQ(
        dynamic->GroupEntries[0][0].Type,
        render::ShaderParameterBindingType::DynamicCBuffer);

    const uint32_t missingGroup = 9;
    const render::ShaderLayoutPolicy missingPolicy{
        .DynamicBufferGroups = std::span{&missingGroup, 1}};
    EXPECT_FALSE(render::MakeBackendPipelineLayoutInput(artifact, missingPolicy).has_value());
}

TEST(RadRayRuntimeMaterial, ResidencyPolicyMapsCBufferForBothTargets) {
    for (const shader::ShaderTarget target : {
             shader::ShaderTarget::DXIL,
             shader::ShaderTarget::SPIRV}) {
        const vector<byte> blob = ReadFixture("nested_types", target);
        ASSERT_FALSE(blob.empty());
        if (target == shader::ShaderTarget::DXIL) {
            const auto artifact = shader::DecodeDxilShaderArtifact(
                blob, DecodeOptions("nested_types", target));
            ASSERT_TRUE(artifact.has_value());
            CheckResidencyPolicy(artifact.value());
        } else {
            const auto artifact = shader::DecodeSpirvShaderArtifact(
                blob, DecodeOptions("nested_types", target));
            ASSERT_TRUE(artifact.has_value());
            CheckResidencyPolicy(artifact.value());
        }
    }
}

TEST(RadRayRuntimeMaterial, ExplicitRootSignatureRejectsResidencyPolicy) {
    const vector<byte> blob = ReadFixture(
        "multiple_root_constants", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(blob.empty());
    const auto artifact = shader::DecodeDxilShaderArtifact(
        blob,
        DecodeOptions("multiple_root_constants", shader::ShaderTarget::DXIL));
    ASSERT_TRUE(artifact.has_value());
    ASSERT_FALSE(artifact->Generic().SerializedRootSignature().empty());
    const uint32_t group = 0;
    const render::ShaderLayoutPolicy policy{
        .DynamicBufferGroups = std::span{&group, 1}};
    EXPECT_FALSE(render::MakeBackendPipelineLayoutInput(artifact.value(), policy).has_value());
}

}  // namespace
}  // namespace radray
