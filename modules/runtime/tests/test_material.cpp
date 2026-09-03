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
        .ExpectedToolchainIdentity = 0x0000000001090212ull};
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
    ASSERT_EQ(direction, layout->Find("Constants.Data.Values.Direction"));
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
    ASSERT_TRUE(values.SetMatrix4x4("Constants.Transform", transform));
    ASSERT_TRUE(values.SetFloat3(
        "Constants.Data.Values.Direction", Eigen::Vector3f{1.0f, 2.0f, 3.0f}, 0));
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

// Binding records are deliberately swapped before layout construction. The owner index, not
// serialized position, must still pair each declaration with its payload root.
TEST(RadRayRuntimeMaterial, MultipleCBuffersUseExplicitPayloadOwners) {
    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        vector<byte> blob = ReadFixture("multiple_cbuffers", target);
        ASSERT_FALSE(blob.empty());
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, blob.data(), sizeof(envelope));
        ASSERT_EQ(
            envelope.BindingRecords.Size,
            2u * sizeof(shader::WireBindingRecord));
        vector<shader::WireBindingRecord> bindings(2);
        std::memcpy(
            bindings.data(),
            blob.data() + envelope.BindingRecords.Offset,
            envelope.BindingRecords.Size);
        ASSERT_NE(bindings[0].TypeIndex, shader::kShaderNoType);
        ASSERT_NE(bindings[1].TypeIndex, shader::kShaderNoType);
        std::swap(bindings[0], bindings[1]);
        std::memcpy(
            blob.data() + envelope.BindingRecords.Offset,
            bindings.data(),
            envelope.BindingRecords.Size);

        const auto artifact = shader::DecodeShaderArtifact(
            blob,
            DecodeOptions("multiple_cbuffers", target));
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
        EXPECT_EQ(first->Size, 80u);
        EXPECT_EQ(second->Size, 68u);
        EXPECT_EQ(first->Group, 0u);
        EXPECT_EQ(second->Group, 1u);

        const ShaderParameterInfo* transform = layout->Find("First.FirstTransform");
        const ShaderParameterInfo* tint = layout->Find("First.FirstTint");
        const ShaderParameterInfo* weight = layout->Find("Second.SecondWeight");
        ASSERT_NE(transform, nullptr);
        ASSERT_NE(tint, nullptr);
        ASSERT_NE(weight, nullptr);
        EXPECT_EQ(layout->Find("FirstTransform"), transform);
        EXPECT_EQ(layout->Find("FirstTint"), tint);
        EXPECT_EQ(layout->Find("SecondWeight"), weight);
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
        ASSERT_TRUE(values.SetMatrix4x4("First.FirstTransform", transformValue));
        ASSERT_TRUE(values.SetFloat4(
            "First.FirstTint", Eigen::Vector4f{1.0f, 2.0f, 3.0f, 4.0f}));
        ASSERT_TRUE(values.SetFloat("Second.SecondWeight", 5.0f));
        EXPECT_EQ(values.GetBufferData(firstIndex).size(), 80u);
        EXPECT_EQ(values.GetBufferData(secondIndex).size(), 68u);
        EXPECT_TRUE(
            ReadValue<Eigen::Matrix4f>(values.GetBufferData(firstIndex), 0)
                .isApprox(transformValue));
        EXPECT_TRUE(
            ReadValue<Eigen::Vector4f>(values.GetBufferData(firstIndex), 64)
                .isApprox(Eigen::Vector4f{1.0f, 2.0f, 3.0f, 4.0f}));
        EXPECT_FLOAT_EQ(ReadValue<float>(values.GetBufferData(secondIndex), 64), 5.0f);
        const std::span<const byte> otherBuffer = values.GetBufferData(secondIndex);
        EXPECT_TRUE(std::all_of(
            otherBuffer.begin(),
            otherBuffer.begin() + 64,
            [](byte value) { return value == byte{0}; }));
    }
}

TEST(RadRayRuntimeMaterial, SharedPayloadOwnerKeepsQualifiedLeavesDistinct) {
    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        const auto artifact = DecodeGeneric("shared_cbuffer_type", target);
        ASSERT_TRUE(artifact.has_value());
        const auto firstBinding = artifact->FindBinding("First");
        const auto secondBinding = artifact->FindBinding("Second");
        ASSERT_TRUE(firstBinding.has_value());
        ASSERT_TRUE(secondBinding.has_value());
        EXPECT_EQ(firstBinding->Record.TypeIndex, secondBinding->Record.TypeIndex);

        const auto layout = ShaderParameterLayout::Create(artifact.value());
        ASSERT_TRUE(layout.has_value());
        ASSERT_EQ(layout->Buffers().size(), 2u);
        const ShaderParameterInfo* first = layout->Find("First.Value");
        const ShaderParameterInfo* second = layout->Find("Second.Value");
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(layout->Find("Value"), nullptr);
        EXPECT_NE(first->BufferIndex, second->BufferIndex);

        ShaderParameterStorage values{&layout.value()};
        const Eigen::Vector4f firstValue{1.0f, 2.0f, 3.0f, 4.0f};
        const Eigen::Vector4f secondValue{5.0f, 6.0f, 7.0f, 8.0f};
        ASSERT_TRUE(values.SetFloat4("First.Value", firstValue));
        ASSERT_TRUE(values.SetFloat4("Second.Value", secondValue));
        EXPECT_TRUE(
            ReadValue<Eigen::Vector4f>(values.GetBufferData(first->BufferIndex), 0)
                .isApprox(firstValue));
        EXPECT_TRUE(
            ReadValue<Eigen::Vector4f>(values.GetBufferData(second->BufferIndex), 0)
                .isApprox(secondValue));
    }
}

TEST(RadRayRuntimeMaterial, ExactResourceNamePrecedesSameNamedLeafShorthand) {
    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        vector<byte> blob = ReadFixture("shared_cbuffer_type", target);
        ASSERT_FALSE(blob.empty());
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, blob.data(), sizeof(envelope));
        ASSERT_EQ(
            envelope.BindingRecords.Size,
            2u * sizeof(shader::WireBindingRecord));
        vector<shader::WireBindingRecord> bindings(2);
        std::memcpy(
            bindings.data(),
            blob.data() + envelope.BindingRecords.Offset,
            envelope.BindingRecords.Size);
        const uint32_t payloadRoot = bindings[0].TypeIndex;
        ASSERT_NE(payloadRoot, shader::kShaderNoType);

        const size_t typeCount =
            envelope.TypeRecords.Size / sizeof(shader::WireTypeRecord);
        vector<shader::WireTypeRecord> types(typeCount);
        std::memcpy(
            types.data(),
            blob.data() + envelope.TypeRecords.Offset,
            envelope.TypeRecords.Size);
        const auto leaf = std::find_if(
            types.begin(),
            types.end(),
            [payloadRoot](const shader::WireTypeRecord& type) {
                return type.ParentIndex == payloadRoot;
            });
        ASSERT_NE(leaf, types.end());

        bindings[1].Name = leaf->Name;
        bindings[1].Type = static_cast<uint32_t>(shader::ShaderBindingKind::Texture);
        bindings[1].TypeIndex = shader::kShaderNoType;
        std::memcpy(
            blob.data() + envelope.BindingRecords.Offset,
            bindings.data(),
            envelope.BindingRecords.Size);

        const auto artifact = shader::DecodeShaderArtifact(
            blob,
            DecodeOptions("shared_cbuffer_type", target));
        ASSERT_TRUE(artifact.has_value());
        const auto layout = ShaderParameterLayout::Create(artifact.value());
        ASSERT_TRUE(layout.has_value());
        const ShaderParameterInfo* exact = layout->Find("Value");
        const ShaderParameterInfo* qualified = layout->Find("First.Value");
        ASSERT_NE(exact, nullptr);
        ASSERT_NE(qualified, nullptr);
        EXPECT_EQ(exact->Kind, ShaderParameterKind::Texture);
        EXPECT_EQ(qualified->Kind, ShaderParameterKind::Vector);
    }
}

TEST(RadRayRuntimeMaterial, ReferencedPayloadRootCanAlsoOwnAnotherDeclaration) {
    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        const auto artifact = DecodeGeneric("nested_cbuffer_roots", target);
        ASSERT_TRUE(artifact.has_value());
        const auto innerBinding = artifact->FindBinding("Inner");
        const auto outerBinding = artifact->FindBinding("Outer");
        ASSERT_TRUE(innerBinding.has_value());
        ASSERT_TRUE(outerBinding.has_value());

        const shader::WireTypeRecord* nestedMember = nullptr;
        for (const shader::WireTypeRecord& type : artifact->Types()) {
            if (type.ParentIndex == outerBinding->Record.TypeIndex &&
                artifact->GetName(type.Name) == std::optional<std::string_view>{"Nested"}) {
                nestedMember = &type;
                break;
            }
        }
        ASSERT_NE(nestedMember, nullptr);
        EXPECT_EQ(nestedMember->TypeIndex, innerBinding->Record.TypeIndex);

        const auto layout = ShaderParameterLayout::Create(artifact.value());
        ASSERT_TRUE(layout.has_value());
        const ShaderParameterInfo* inner = layout->Find("Inner.InnerValue");
        const ShaderParameterInfo* nested = layout->Find("Outer.Nested.InnerValue");
        ASSERT_NE(inner, nullptr);
        ASSERT_NE(nested, nullptr);
        EXPECT_EQ(layout->Find("InnerValue"), nullptr);
        EXPECT_NE(inner->BufferIndex, nested->BufferIndex);

        ShaderParameterStorage values{&layout.value()};
        const Eigen::Vector4f innerValue{1.0f, 2.0f, 3.0f, 4.0f};
        const Eigen::Vector4f nestedValue{5.0f, 6.0f, 7.0f, 8.0f};
        ASSERT_TRUE(values.SetFloat4("Inner.InnerValue", innerValue));
        ASSERT_TRUE(values.SetFloat4("Outer.Nested.InnerValue", nestedValue));
        EXPECT_TRUE(
            ReadValue<Eigen::Vector4f>(values.GetBufferData(inner->BufferIndex), 0)
                .isApprox(innerValue));
        EXPECT_TRUE(
            ReadValue<Eigen::Vector4f>(values.GetBufferData(nested->BufferIndex), 0)
                .isApprox(nestedValue));
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

TEST(RadRayRuntimeMaterial, AmbiguousLeafRequiresQualifiedParameterPath) {
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
    const auto layout = ShaderParameterLayout::Create(artifact.value());
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->Find("Direction"), nullptr);
    ASSERT_NE(layout->Find("Constants.Direction"), nullptr);
    ASSERT_NE(layout->Find("Constants.Data.Values.Direction"), nullptr);

    ShaderParameterStorage values{&layout.value()};
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix(2, 3) = 11.0f;
    const Eigen::Vector3f direction{2.0f, 3.0f, 4.0f};
    ASSERT_TRUE(values.SetMatrix4x4("Constants.Direction", matrix));
    ASSERT_TRUE(values.SetFloat3("Constants.Data.Values.Direction", direction));
    EXPECT_TRUE(ReadValue<Eigen::Matrix4f>(values.GetBufferData(0), 0).isApprox(matrix));
    EXPECT_TRUE(ReadValue<Eigen::Vector3f>(values.GetBufferData(0), 64).isApprox(direction));
}

// nested_types declares exactly one constant buffer, so it is the smallest artifact that can show
// the difference between "the compiler published a table entry" and "the pipeline asked for a
// bind-time offset on this declaration".
render::ShaderLayoutSelector ConstantsSelector(
    shader::ShaderBindingKind kind = shader::ShaderBindingKind::CBuffer) {
    return render::ShaderLayoutSelector{
        .DeclarationName = "Constants",
        .ExpectedLogicalResourceKind = kind};
}

TEST(RadRayRuntimeMaterial, DeclarationModifierMakesOneCBufferDynamicOnBothTargets) {
    const vector<byte> dxilBlob = ReadFixture("nested_types", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(dxilBlob.empty());
    const auto dxil = shader::DecodeDxilShaderArtifact(
        dxilBlob, DecodeOptions("nested_types", shader::ShaderTarget::DXIL));
    ASSERT_TRUE(dxil.has_value());

    const auto plainD3D12 = render::ResolveD3D12Layout(dxil.value());
    ASSERT_TRUE(plainD3D12.has_value());
    ASSERT_EQ(plainD3D12->Bindings.size(), 1u);
    EXPECT_EQ(
        plainD3D12->Bindings[0].Placement,
        shader::ShaderBindingPlacement::Table);
    EXPECT_EQ(plainD3D12->Bindings[0].LogicalKind, shader::ShaderBindingKind::CBuffer);

    render::D3D12TargetLayoutOptions rootDescriptor;
    rootDescriptor.BufferPlacements.push_back(
        {.Selector = ConstantsSelector(),
         .Placement = render::D3D12BufferPlacement::RootDescriptor});
    const auto dynamicD3D12 = render::ResolveD3D12Layout(dxil.value(), rootDescriptor);
    ASSERT_TRUE(dynamicD3D12.has_value());
    EXPECT_EQ(
        dynamicD3D12->Bindings[0].Placement,
        shader::ShaderBindingPlacement::RootDescriptor);
    // The logical kind stays a cbuffer: only the placement moved, which is what keeps the resolved
    // layout from re-classifying the resource behind the caller's back.
    EXPECT_EQ(dynamicD3D12->Bindings[0].LogicalKind, shader::ShaderBindingKind::CBuffer);
    // The placement is part of the resolved semantics, so it has to move the identity.
    EXPECT_NE(plainD3D12->Hash, dynamicD3D12->Hash);

    const vector<byte> spirvBlob = ReadFixture("nested_types", shader::ShaderTarget::SPIRV);
    ASSERT_FALSE(spirvBlob.empty());
    const auto spirv = shader::DecodeSpirvShaderArtifact(
        spirvBlob, DecodeOptions("nested_types", shader::ShaderTarget::SPIRV));
    ASSERT_TRUE(spirv.has_value());

    const auto plainVulkan = render::ResolveVulkanLayout(spirv.value());
    ASSERT_TRUE(plainVulkan.has_value());
    ASSERT_EQ(plainVulkan->Bindings.size(), 1u);
    EXPECT_EQ(
        plainVulkan->Bindings[0].Placement,
        render::VulkanBufferDescriptorPlacement::Regular);
    EXPECT_TRUE(plainVulkan->DynamicOffsetOrder.empty());

    render::VulkanTargetLayoutOptions dynamicBuffer;
    dynamicBuffer.BufferDescriptors.push_back(
        {.Selector = ConstantsSelector(),
         .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    const auto dynamicVulkan = render::ResolveVulkanLayout(spirv.value(), dynamicBuffer);
    ASSERT_TRUE(dynamicVulkan.has_value());
    EXPECT_EQ(
        dynamicVulkan->Bindings[0].Placement,
        render::VulkanBufferDescriptorPlacement::Dynamic);
    ASSERT_EQ(dynamicVulkan->DynamicOffsetOrder.size(), 1u);
    EXPECT_EQ(dynamicVulkan->DynamicOffsetOrder[0], 0u);
    // The logical kind stays a cbuffer: only the placement moved, which is what keeps the resolved
    // layout from re-classifying the resource behind the caller's back.
    EXPECT_EQ(dynamicVulkan->Bindings[0].LogicalKind, shader::ShaderBindingKind::CBuffer);
    EXPECT_NE(plainVulkan->Hash, dynamicVulkan->Hash);

    // The two targets resolve the same intent, but they are separate layouts with separate
    // numbering, so their identities must not be interchangeable.
    EXPECT_NE(plainD3D12->Hash, plainVulkan->Hash);
}

TEST(RadRayRuntimeMaterial, LayoutResolveFailsClosedOnRecipeContradictions) {
    const vector<byte> blob = ReadFixture("nested_types", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(blob.empty());
    const auto artifact = shader::DecodeDxilShaderArtifact(
        blob, DecodeOptions("nested_types", shader::ShaderTarget::DXIL));
    ASSERT_TRUE(artifact.has_value());

    const auto resolveWith = [&](const render::D3D12TargetLayoutOptions& options,
                                 render::ShaderLayoutResolveError expected) {
        render::ShaderLayoutResolveError error = render::ShaderLayoutResolveError::None;
        EXPECT_FALSE(render::ResolveD3D12Layout(artifact.value(), options, &error).has_value());
        EXPECT_EQ(error, expected);
    };

    // A name the shader does not declare means the recipe was written against a different shader.
    render::D3D12TargetLayoutOptions missing;
    missing.BufferPlacements.push_back(
        {.Selector = {.DeclarationName = "NotDeclared",
                      .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer},
         .Placement = render::D3D12BufferPlacement::RootDescriptor});
    resolveWith(missing, render::ShaderLayoutResolveError::SelectorNotFound);

    // The expected kind is checked, not assumed: the same name with the wrong kind is a mismatch.
    render::D3D12TargetLayoutOptions wrongKind;
    wrongKind.BufferPlacements.push_back(
        {.Selector = ConstantsSelector(shader::ShaderBindingKind::StructuredBuffer),
         .Placement = render::D3D12BufferPlacement::RootDescriptor});
    resolveWith(wrongKind, render::ShaderLayoutResolveError::SelectorNotFound);

    // Two modifiers for one declaration is ambiguous, so it is rejected rather than resolved by
    // input order.
    render::D3D12TargetLayoutOptions duplicate;
    duplicate.BufferPlacements.push_back(
        {.Selector = ConstantsSelector(),
         .Placement = render::D3D12BufferPlacement::RootDescriptor});
    duplicate.BufferPlacements.push_back(
        {.Selector = ConstantsSelector(),
         .Placement = render::D3D12BufferPlacement::Table});
    resolveWith(duplicate, render::ShaderLayoutResolveError::DuplicateSelector);

    // A resolved layout belongs to one target: the other target's artifact cannot produce it.
    const vector<byte> spirvBlob = ReadFixture("nested_types", shader::ShaderTarget::SPIRV);
    ASSERT_FALSE(spirvBlob.empty());
    const auto spirv = shader::DecodeSpirvShaderArtifact(
        spirvBlob, DecodeOptions("nested_types", shader::ShaderTarget::SPIRV));
    ASSERT_TRUE(spirv.has_value());
    render::VulkanTargetLayoutOptions samplerOnCBuffer;
    samplerOnCBuffer.ImmutableSamplers.push_back(
        {.Selector = ConstantsSelector(), .State = {}});
    render::ShaderLayoutResolveError samplerError = render::ShaderLayoutResolveError::None;
    EXPECT_FALSE(
        render::ResolveVulkanLayout(spirv.value(), samplerOnCBuffer, &samplerError).has_value());
    EXPECT_EQ(samplerError, render::ShaderLayoutResolveError::IllegalPlacement);
}

TEST(RadRayRuntimeMaterial, ExplicitPolicyRefusesPlacementModifiers) {
    const vector<byte> blob = ReadFixture(
        "multiple_root_constants", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(blob.empty());
    const auto artifact = shader::DecodeDxilShaderArtifact(
        blob,
        DecodeOptions("multiple_root_constants", shader::ShaderTarget::DXIL));
    ASSERT_TRUE(artifact.has_value());
    ASSERT_FALSE(artifact->Generic().SerializedRootSignature().empty());

    // Resolving without modifiers keeps the carrier verbatim: it is what CreateRootSignature gets.
    const auto resolved = render::ResolveD3D12Layout(artifact.value());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(resolved->HasExplicitCarrier());
    EXPECT_EQ(
        resolved->SerializedRootSignature.size(),
        artifact->Generic().SerializedRootSignature().size());

    // Rewriting a destination would make the resolved layout disagree with the blob it ships.
    render::D3D12TargetLayoutOptions options;
    options.BufferPlacements.push_back(
        {.Selector = {.DeclarationName = "ObjectConstants",
                      .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer},
         .Placement = render::D3D12BufferPlacement::RootDescriptor});
    render::ShaderLayoutResolveError error = render::ShaderLayoutResolveError::None;
    EXPECT_FALSE(render::ResolveD3D12Layout(artifact.value(), options, &error).has_value());
    EXPECT_EQ(error, render::ShaderLayoutResolveError::ExplicitPolicyNotModifiable);
}

}  // namespace
}  // namespace radray
