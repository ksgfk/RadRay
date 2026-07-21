#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include <radray/runtime/shader_binding_policy.h>
#include <radray/runtime/shader_parameters.h>

namespace radray {
namespace {

shader::ShaderValueTypeDesc FloatType(
    uint32_t columns,
    uint32_t size,
    uint32_t arrayCount = 1,
    uint32_t arrayStride = 0) {
    return shader::ShaderValueTypeDesc{
        .Scalar = shader::ShaderScalarType::Float32,
        .Rows = 1,
        .Columns = columns,
        .ArrayCount = arrayCount,
        .ArrayStride = arrayStride,
        .ByteSize = size};
}

shader::ShaderBindingDesc MakeConstants() {
    return shader::ShaderBindingDesc{
        .Name = "gMaterial",
        .BindingIndex = 0,
        .Kind = shader::ShaderBindingKind::ConstantBuffer,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = shader::ShaderStage::Pixel,
        .Buffer = shader::ShaderBufferInterfaceDesc{
            .ByteSize = 224,
            .Fields = {
                shader::ShaderInterfaceFieldDesc{
                    .Name = "BaseColor",
                    .Offset = 0,
                    .Size = 16,
                    .Type = FloatType(4, 16),
                    .Members = {}},
                shader::ShaderInterfaceFieldDesc{
                    .Name = "Roughness",
                    .Offset = 16,
                    .Size = 4,
                    .Type = FloatType(1, 4),
                    .Members = {}},
                shader::ShaderInterfaceFieldDesc{
                    .Name = "Transform",
                    .Offset = 32,
                    .Size = 64,
                    .Type = shader::ShaderValueTypeDesc{
                        .Scalar = shader::ShaderScalarType::Float32,
                        .Rows = 4,
                        .Columns = 4,
                        .ArrayCount = 1,
                        .MatrixStride = 16,
                        .ByteSize = 64},
                    .Members = {}},
                shader::ShaderInterfaceFieldDesc{.Name = "Weights", .Offset = 96, .Size = 32, .Type = FloatType(1, 32, 2, 16), .Members = {}}, shader::ShaderInterfaceFieldDesc{.Name = "Layers", .Offset = 128, .Size = 64, .Type = shader::ShaderValueTypeDesc{.Rows = 1, .Columns = 1, .ArrayCount = 2, .ArrayStride = 32, .ByteSize = 64}, .Members = {shader::ShaderInterfaceFieldDesc{.Name = "Value", .Offset = 128, .Size = 16, .Type = FloatType(4, 16), .Members = {}}}}, shader::ShaderInterfaceFieldDesc{.Name = "IntValue", .Offset = 192, .Size = 4, .Type = shader::ShaderValueTypeDesc{.Scalar = shader::ShaderScalarType::Int32, .Rows = 1, .Columns = 1, .ArrayCount = 1, .ByteSize = 4}, .Members = {}}, shader::ShaderInterfaceFieldDesc{.Name = "UIntValue", .Offset = 196, .Size = 4, .Type = shader::ShaderValueTypeDesc{.Scalar = shader::ShaderScalarType::UInt32, .Rows = 1, .Columns = 1, .ArrayCount = 1, .ByteSize = 4}, .Members = {}}, shader::ShaderInterfaceFieldDesc{.Name = "BoolValue", .Offset = 200, .Size = 4, .Type = shader::ShaderValueTypeDesc{.Scalar = shader::ShaderScalarType::Bool, .Rows = 1, .Columns = 1, .ArrayCount = 1, .ByteSize = 4}, .Members = {}}, shader::ShaderInterfaceFieldDesc{.Name = "Float2Value", .Offset = 208, .Size = 8, .Type = FloatType(2, 8), .Members = {}}}}};
}

shader::ShaderInterfaceFieldDesc MakeConstantField(
    std::string_view name,
    uint32_t offset,
    uint32_t columns = 4) {
    return shader::ShaderInterfaceFieldDesc{
        .Name = string{name},
        .Offset = offset,
        .Size = columns * 4,
        .Type = FloatType(columns, columns * 4),
        .Members = {}};
}

shader::ShaderBindingDesc MakeProjectedConstants(
    uint32_t byteSize,
    vector<shader::ShaderInterfaceFieldDesc> fields) {
    shader::ShaderBindingDesc result = MakeConstants();
    result.Buffer->ByteSize = byteSize;
    result.Buffer->Fields = std::move(fields);
    return result;
}

shader::ShaderInterfaceDesc MakeConstantInterface(shader::ShaderBindingDesc constants) {
    return shader::ShaderInterfaceDesc{
        .Kind = shader::ShaderProgramKind::Graphics,
        .BindingGroups = {shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 2,
            .Bindings = {std::move(constants)}}},
        .PushConstants = {},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {}};
}

shader::ShaderBindingDesc MakeSampler(uint32_t binding, std::string_view name, uint32_t count = 1) {
    return shader::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = shader::ShaderBindingKind::Sampler,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = count,
        .Stages = shader::ShaderStage::Pixel};
}

shader::ShaderBindingDesc MakeTexture(uint32_t binding, std::string_view name) {
    return shader::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = shader::ShaderBindingKind::SampledTexture,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = shader::ShaderStage::Pixel,
        .Texture = shader::ShaderTextureInterfaceDesc{
            .Dimension = shader::ShaderTextureDimension::Dim2D,
            .SampleType = shader::ShaderSampleType::Float}};
}

shader::ShaderBindingDesc MakeStorageTexture(uint32_t binding, std::string_view name) {
    shader::ShaderBindingDesc result = MakeTexture(binding, name);
    result.Kind = shader::ShaderBindingKind::StorageTexture;
    result.Access = shader::ShaderResourceAccess::ReadWrite;
    return result;
}

shader::ShaderBindingDesc MakeBuffer(
    uint32_t binding,
    std::string_view name,
    shader::ShaderBindingKind kind,
    shader::ShaderResourceAccess access,
    uint32_t elementStride = 0) {
    shader::ShaderBindingDesc result{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = kind,
        .Access = access,
        .Count = 1,
        .Stages = shader::ShaderStage::Pixel};
    if (kind == shader::ShaderBindingKind::TypedBuffer) {
        result.Texture = shader::ShaderTextureInterfaceDesc{
            .Dimension = shader::ShaderTextureDimension::Buffer,
            .SampleType = shader::ShaderSampleType::Float};
    } else {
        result.Buffer = shader::ShaderBufferInterfaceDesc{
            .ElementStride = elementStride,
            .Fields = {}};
    }
    return result;
}

shader::ShaderInterfaceDesc MakeResourceInterface() {
    shader::ShaderBindingDesc unbounded = MakeTexture(5, "UnboundedTextures");
    unbounded.Count = 0;
    return shader::ShaderInterfaceDesc{
        .Kind = shader::ShaderProgramKind::Graphics,
        .BindingGroups = {shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 6,
            .Bindings = {
                MakeTexture(0, "SampledTexture"),
                MakeStorageTexture(1, "StorageTexture"),
                MakeBuffer(
                    2,
                    "TypedBuffer",
                    shader::ShaderBindingKind::TypedBuffer,
                    shader::ShaderResourceAccess::ReadOnly),
                MakeBuffer(
                    3,
                    "StructuredBuffer",
                    shader::ShaderBindingKind::StructuredBuffer,
                    shader::ShaderResourceAccess::ReadOnly,
                    16),
                MakeBuffer(
                    4,
                    "RawBuffer",
                    shader::ShaderBindingKind::RawBuffer,
                    shader::ShaderResourceAccess::ReadWrite),
                std::move(unbounded)}}},
        .PushConstants = {},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {}};
}

shader::ShaderInterfaceDesc MakeInterface(bool includeExtraGroup = true) {
    shader::ShaderInterfaceDesc result{
        .Kind = shader::ShaderProgramKind::Graphics,
        .BindingGroups = {
            shader::ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 0,
                .Bindings = {MakeSampler(0, "PipelineSampler")}},
            shader::ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 2,
                .Bindings = {MakeConstants(), MakeTexture(1, "BaseMap"), MakeSampler(2, "MaterialSampler", 2)}}},
        .PushConstants = {},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {}};
    if (includeExtraGroup) {
        result.BindingGroups.emplace_back(shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 4,
            .Bindings = {MakeSampler(0, "AuxSampler")}});
    }
    return result;
}

PipelineBindingPolicy MakePolicy() {
    auto provider = make_shared<ShaderBindingSchemaProvider>(
        "Pipeline",
        vector<ShaderBindingProviderSchemaEntry>{
            ShaderBindingProviderSchemaEntry{
                .AcceptedBindings = {MakeSampler(0, "Expected")}}});
    return PipelineBindingPolicy{{PipelineBindingReservation{.GroupIndex = 0, .Provider = std::move(provider)}}};
}

float ReadFloat(std::span<const byte> data, size_t offset) {
    float value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

template <typename T>
T ReadValue(std::span<const byte> data, size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

class FakeBuffer final : public render::Buffer {
public:
    explicit FakeBuffer(render::BufferDescriptor desc) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view /*name*/) noexcept override {}
    void* Map(uint64_t /*offset*/, uint64_t /*size*/) noexcept override { return nullptr; }
    void Unmap() noexcept override {}
    void FlushMappedRange(render::BufferRange /*range*/) noexcept override {}
    void InvalidateMappedRange(render::BufferRange /*range*/) noexcept override {}
    render::BufferDescriptor GetDesc() const noexcept override { return _desc; }
    render::Device* GetDevice() const noexcept override { return nullptr; }

private:
    render::BufferDescriptor _desc;
    bool _valid{true};
};

class FakeTexture final : public render::Texture {
public:
    explicit FakeTexture(render::TextureDescriptor desc) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view /*name*/) noexcept override {}
    render::TextureDescriptor GetDesc() const noexcept override { return _desc; }

private:
    render::TextureDescriptor _desc;
    bool _valid{true};
};

class FakeTextureView final : public render::TextureView {
public:
    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view /*name*/) noexcept override {}

private:
    bool _valid{true};
};

StreamingAssetRef<TextureAsset> MakeTextureAsset(
    AssetManager& assets,
    render::TextureUses usage) {
    render::TextureDescriptor desc{
        .Dim = render::TextureDimension::Dim2D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = render::TextureFormat::RGBA8_UNORM,
        .Memory = render::MemoryType::Device,
        .Usage = usage};
    return assets.AddReady(
        Guid::NewGuid(),
        make_unique<TextureAsset>(
            nullptr,
            "parameter-test",
            make_unique<FakeTexture>(desc),
            make_unique<FakeTextureView>()));
}

}  // namespace

TEST(ShaderParameterLayoutTest, ResolvesAllUnreservedGroups) {
    const vector<shader::ShaderInterfaceDesc> interfaces{MakeInterface(), MakeInterface(false)};
    auto result = BuildShaderParameterLayout(interfaces, MakePolicy(), shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(result.Succeeded());
    ASSERT_TRUE(result.Layout->IsValid());
    ASSERT_EQ(result.Layout->Groups().size(), 2u);
    EXPECT_EQ(result.Layout->Groups()[0].GroupIndex, 2u);
    EXPECT_EQ(result.Layout->Groups()[1].GroupIndex, 4u);
    EXPECT_FALSE(result.Layout->FindBinding("PipelineSampler").HasValue());
    EXPECT_TRUE(result.Layout->FindBinding("BaseMap").HasValue());
    EXPECT_TRUE(result.Layout->FindBinding(ShaderParameterLocation{2, 0}).HasValue());
    EXPECT_TRUE(result.Layout->FindField("gMaterial.BaseColor").HasValue());
    EXPECT_TRUE(result.Layout->FindField("Roughness").HasValue());
}

TEST(ShaderParameterLayoutTest, RejectsUserGroupLayoutChanges) {
    vector<shader::ShaderInterfaceDesc> interfaces{MakeInterface(false), MakeInterface(false)};
    interfaces[1].BindingGroups[1].Bindings[1].Texture->Dimension = shader::ShaderTextureDimension::Cube;
    auto result = BuildShaderParameterLayout(interfaces, MakePolicy(), shader::ShaderProgramKind::Graphics);
    ASSERT_FALSE(result.Succeeded());
    ASSERT_EQ(result.Diagnostics.size(), 1u);
    EXPECT_EQ(result.Diagnostics.front().Context.Group, 2u);
}

TEST(ShaderParameterLayoutTest, UnionsConstantBufferFieldProjectionsDeterministically) {
    const shader::ShaderInterfaceDesc first = MakeConstantInterface(
        MakeProjectedConstants(16, {MakeConstantField("Color", 0, 1)}));
    const shader::ShaderInterfaceDesc second = MakeConstantInterface(
        MakeProjectedConstants(32, {MakeConstantField("Params", 16, 1)}));
    const vector<shader::ShaderInterfaceDesc> forward{first, second};
    const vector<shader::ShaderInterfaceDesc> reverse{second, first};

    auto forwardLayout = BuildShaderParameterLayout(
        forward,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    auto reverseLayout = BuildShaderParameterLayout(
        reverse,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(forwardLayout.Succeeded());
    ASSERT_TRUE(reverseLayout.Succeeded());
    EXPECT_EQ(*forwardLayout.Layout, *reverseLayout.Layout);
    EXPECT_EQ(forwardLayout.Layout->GetHash(), reverseLayout.Layout->GetHash());

    auto binding = forwardLayout.Layout->FindBinding({2, 0});
    ASSERT_TRUE(binding.HasValue());
    ASSERT_TRUE(binding.Get()->Interface.Buffer.has_value());
    EXPECT_EQ(binding.Get()->Interface.Buffer->ByteSize, 32u);
    ASSERT_EQ(binding.Get()->Fields.size(), 2u);
    EXPECT_EQ(binding.Get()->Fields[0].Name, "Color");
    EXPECT_EQ(binding.Get()->Fields[1].Name, "Params");
}

TEST(ShaderParameterLayoutTest, RejectsConflictingConstantBufferFieldsWithBothOrigins) {
    vector<ShaderProgramInterfaceRecord> records{
        ShaderProgramInterfaceRecord{
            .Interface = MakeConstantInterface(
                MakeProjectedConstants(16, {MakeConstantField("Value", 0)})),
            .Context = shader::ShaderDiagnosticContext{
                .PassIndex = 1,
                .VariantDefines = {"FIRST=1"}}},
        ShaderProgramInterfaceRecord{.Interface = MakeConstantInterface(MakeProjectedConstants(32, {MakeConstantField("Value", 16)})), .Context = shader::ShaderDiagnosticContext{.PassIndex = 2, .VariantDefines = {"SECOND=1"}}}};
    auto changedOffset = BuildShaderParameterLayout(
        records,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_FALSE(changedOffset.Succeeded());
    ASSERT_EQ(changedOffset.Diagnostics.size(), 1u);
    EXPECT_EQ(changedOffset.Diagnostics.front().Context.PassIndex, 2u);
    EXPECT_EQ(changedOffset.Diagnostics.front().Context.Group, 2u);
    EXPECT_EQ(changedOffset.Diagnostics.front().Context.Binding, 0u);
    ASSERT_TRUE(changedOffset.Diagnostics.front().RelatedContext.has_value());
    EXPECT_EQ(changedOffset.Diagnostics.front().RelatedContext->PassIndex, 1u);
    EXPECT_NE(changedOffset.Diagnostics.front().Message.find("Value"), string::npos);

    records[1].Interface = MakeConstantInterface(
        MakeProjectedConstants(16, {MakeConstantField("Alias", 0)}));
    auto overlappingAlias = BuildShaderParameterLayout(
        records,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_FALSE(overlappingAlias.Succeeded());
    ASSERT_EQ(overlappingAlias.Diagnostics.size(), 1u);
    EXPECT_NE(overlappingAlias.Diagnostics.front().Message.find("overlaps"), string::npos);
}

TEST(ShaderParameterLayoutTest, ReportsBothProgramOriginsForConflicts) {
    vector<ShaderProgramInterfaceRecord> interfaces{
        ShaderProgramInterfaceRecord{
            .Interface = MakeInterface(false),
            .Context = shader::ShaderDiagnosticContext{
                .Target = shader::ShaderTarget::DXIL,
                .PassIndex = 0,
                .VariantDefines = {"FIRST=1"}}},
        ShaderProgramInterfaceRecord{.Interface = MakeInterface(false), .Context = shader::ShaderDiagnosticContext{.Target = shader::ShaderTarget::SPIRV, .PassIndex = 1, .VariantDefines = {"SECOND=1"}}}};
    interfaces[1].Interface.BindingGroups[1].Bindings[1].Texture->Dimension =
        shader::ShaderTextureDimension::Cube;

    auto result = BuildShaderParameterLayout(
        interfaces,
        MakePolicy(),
        shader::ShaderProgramKind::Graphics);
    ASSERT_FALSE(result.Succeeded());
    ASSERT_EQ(result.Diagnostics.size(), 1u);
    EXPECT_EQ(result.Diagnostics.front().Context.PassIndex, 1u);
    EXPECT_EQ(result.Diagnostics.front().Context.VariantDefines, (vector<string>{"SECOND=1"}));
    EXPECT_EQ(result.Diagnostics.front().Context.Group, 2u);
    EXPECT_EQ(result.Diagnostics.front().Context.Binding, 1u);
    ASSERT_TRUE(result.Diagnostics.front().RelatedContext.has_value());
    EXPECT_EQ(result.Diagnostics.front().RelatedContext->PassIndex, 0u);
    EXPECT_EQ(result.Diagnostics.front().RelatedContext->VariantDefines, (vector<string>{"FIRST=1"}));
}

TEST(ShaderParameterLayoutTest, BuildsComputeParametersWithoutMaterialAsset) {
    shader::ShaderInterfaceDesc compute{
        .Kind = shader::ShaderProgramKind::Compute,
        .BindingGroups = {shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 3,
            .Bindings = {MakeSampler(0, "ComputeSampler")}}},
        .PushConstants = {},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {},
        .Compute = shader::ShaderComputeInterfaceDesc{8, 4, 1}};
    compute.BindingGroups.front().Bindings.front().Stages = shader::ShaderStage::Compute;
    const vector<shader::ShaderInterfaceDesc> interfaces{compute};
    auto layout = BuildShaderParameterLayout(
        interfaces,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Compute);
    ASSERT_TRUE(layout.Succeeded());
    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));
    auto plan = ResolveShaderBindings(compute, PipelineBindingPolicy{});
    ASSERT_TRUE(plan.Succeeded());
    EXPECT_TRUE(parameters.IsCompleteFor(*plan.Plan));
}

TEST(ShaderParameterLayoutTest, ExplicitlyRejectsUnsupportedPushConstants) {
    shader::ShaderInterfaceDesc interface{
        .Kind = shader::ShaderProgramKind::Graphics,
        .BindingGroups = {},
        .PushConstants = {shader::ShaderPushConstantRangeDesc{
            .Name = "DrawConstants",
            .Offset = 0,
            .Size = 16,
            .Stages = shader::ShaderStage::Pixel,
            .Fields = {MakeConstantField("Value", 0)}}},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {}};
    const vector<shader::ShaderInterfaceDesc> interfaces{interface};
    auto layout = BuildShaderParameterLayout(
        interfaces,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_FALSE(layout.Succeeded());
    ASSERT_EQ(layout.Diagnostics.size(), 1u);
    EXPECT_NE(layout.Diagnostics.front().Message.find("push constants"), string::npos);
}

TEST(ShaderParameterSetTest, PacksTypedConstantsArraysAndMatrices) {
    const vector<shader::ShaderInterfaceDesc> interfaces{MakeInterface(false)};
    auto layout = BuildShaderParameterLayout(interfaces, MakePolicy(), shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));

    const Eigen::Vector4f color{0.1f, 0.2f, 0.3f, 0.4f};
    ASSERT_TRUE(parameters.SetVector("BaseColor", color));
    ASSERT_TRUE(parameters.SetFloat("gMaterial.Roughness", 0.35f));
    const float weight = 0.75f;
    ASSERT_TRUE(parameters.SetValue(
        "Weights",
        shader::ShaderScalarType::Float32,
        1,
        std::as_bytes(std::span{&weight, 1}),
        1));
    const std::array<float, 16> matrix{
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16};
    ASSERT_TRUE(parameters.SetMatrix("Transform", matrix, 4, 4));
    const Eigen::Vector4f layerValue{4.0f, 3.0f, 2.0f, 1.0f};
    ASSERT_TRUE(parameters.SetVector("Layers[1].Value", layerValue));
    ASSERT_TRUE(parameters.SetInt("IntValue", -17));
    ASSERT_TRUE(parameters.SetUInt("UIntValue", 42));
    ASSERT_TRUE(parameters.SetBool("BoolValue", true));
    const std::array<float, 2> float2{0.25f, 0.75f};
    ASSERT_TRUE(parameters.SetValue(
        "Float2Value",
        shader::ShaderScalarType::Float32,
        2,
        std::as_bytes(std::span{float2})));
    EXPECT_FALSE(parameters.SetUInt("Roughness", 1));
    EXPECT_FALSE(parameters.SetFloat("IntValue", 1.0f));
    EXPECT_FALSE(parameters.SetFloat("Missing", 1.0f));

    const auto value = std::ranges::find(
        parameters.Values(),
        ShaderParameterLocation{2, 0},
        &ShaderParameterBindingValue::Location);
    ASSERT_NE(value, parameters.Values().end());
    ASSERT_EQ(value->ConstantData.size(), 224u);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 0), 0.1f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 16), 0.35f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 112), 0.75f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 32), 1.0f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 36), 5.0f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 48), 2.0f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 160), 4.0f);
    EXPECT_EQ(ReadValue<int32_t>(value->ConstantData, 192), -17);
    EXPECT_EQ(ReadValue<uint32_t>(value->ConstantData, 196), 42u);
    EXPECT_EQ(ReadValue<uint32_t>(value->ConstantData, 200), 1u);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 208), 0.25f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 212), 0.75f);

    vector<byte> wholeBlock(224, byte{0x5a});
    ASSERT_TRUE(parameters.SetConstantBuffer("gMaterial", wholeBlock));
    EXPECT_EQ(value->ConstantData, wholeBlock);
}

TEST(ShaderParameterSetTest, LocationSettersDisambiguateDuplicateNamesAcrossGroups) {
    shader::ShaderBindingDesc secondConstants = MakeConstants();
    shader::ShaderInterfaceDesc interface{
        .Kind = shader::ShaderProgramKind::Graphics,
        .BindingGroups = {
            shader::ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 2,
                .Bindings = {MakeConstants()}},
            shader::ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 4,
                .Bindings = {std::move(secondConstants)}}},
        .PushConstants = {},
        .VertexInputs = {},
        .VertexOutputs = {},
        .PixelInputs = {},
        .PixelOutputs = {}};
    const vector<shader::ShaderInterfaceDesc> interfaces{interface};
    auto layout = BuildShaderParameterLayout(
        interfaces,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));

    EXPECT_FALSE(parameters.SetFloat("Roughness", 0.5f));
    EXPECT_TRUE(parameters.SetFloat({2, 0}, "Roughness", 0.25f));
    EXPECT_TRUE(parameters.SetFloat({4, 0}, "Roughness", 0.75f));
    EXPECT_FALSE(parameters.SetFloat({5, 0}, "Roughness", 1.0f));

    const auto first = std::ranges::find(
        parameters.Values(),
        ShaderParameterLocation{2, 0},
        &ShaderParameterBindingValue::Location);
    const auto second = std::ranges::find(
        parameters.Values(),
        ShaderParameterLocation{4, 0},
        &ShaderParameterBindingValue::Location);
    ASSERT_NE(first, parameters.Values().end());
    ASSERT_NE(second, parameters.Values().end());
    EXPECT_FLOAT_EQ(ReadFloat(first->ConstantData, 16), 0.25f);
    EXPECT_FLOAT_EQ(ReadFloat(second->ConstantData, 16), 0.75f);
}

TEST(ShaderParameterSetTest, TracksResourceCompletenessAndTypeSafety) {
    const vector<shader::ShaderInterfaceDesc> interfaces{MakeInterface(false)};
    auto layout = BuildShaderParameterLayout(interfaces, MakePolicy(), shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));
    EXPECT_FALSE(parameters.IsComplete());
    EXPECT_TRUE(parameters.SetSampler("MaterialSampler", render::SamplerDescriptor{}, 1));
    EXPECT_FALSE(parameters.SetSampler("BaseMap", render::SamplerDescriptor{}));
    EXPECT_FALSE(parameters.SetTexture("MaterialSampler", {}));
    EXPECT_TRUE(parameters.ClearResource("MaterialSampler", 0));
    EXPECT_FALSE(parameters.IsComplete());
}

TEST(ShaderParameterSetTest, CompletenessUsesTheActualProgramProjection) {
    shader::ShaderInterfaceDesc withTexture = MakeInterface(false);
    shader::ShaderInterfaceDesc withoutTexture = withTexture;
    auto& materialBindings = withoutTexture.BindingGroups[1].Bindings;
    std::erase_if(materialBindings, [](const shader::ShaderBindingDesc& binding) {
        return binding.Kind == shader::ShaderBindingKind::SampledTexture;
    });
    const vector<shader::ShaderInterfaceDesc> interfaces{withTexture, withoutTexture};
    PipelineBindingPolicy policy = MakePolicy();
    auto layout = BuildShaderParameterLayout(
        interfaces,
        policy,
        shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));
    EXPECT_FALSE(parameters.IsComplete());

    auto inactivePlan = ResolveShaderBindings(withoutTexture, policy);
    ASSERT_TRUE(inactivePlan.Succeeded());
    EXPECT_TRUE(parameters.IsCompleteFor(*inactivePlan.Plan));

    auto activePlan = ResolveShaderBindings(withTexture, policy);
    ASSERT_TRUE(activePlan.Succeeded());
    EXPECT_FALSE(parameters.IsCompleteFor(*activePlan.Plan));
}

TEST(ShaderParameterSetTest, ConstantProjectionCompletenessAndLayoutGrowthPreserveValues) {
    const shader::ShaderInterfaceDesc first = MakeConstantInterface(
        MakeProjectedConstants(16, {MakeConstantField("Color", 0, 1)}));
    const shader::ShaderInterfaceDesc second = MakeConstantInterface(
        MakeProjectedConstants(32, {MakeConstantField("Params", 16, 1)}));
    const vector<shader::ShaderInterfaceDesc> initialInterfaces{first};
    const vector<shader::ShaderInterfaceDesc> extendedInterfaces{first, second};
    auto initial = BuildShaderParameterLayout(
        initialInterfaces,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    auto extended = BuildShaderParameterLayout(
        extendedInterfaces,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(initial.Succeeded());
    ASSERT_TRUE(extended.Succeeded());

    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*initial.Layout));
    ASSERT_TRUE(parameters.SetFloat({2, 0}, "Color", 0.75f));
    ASSERT_TRUE(parameters.Reset(*extended.Layout, true));
    const auto value = std::ranges::find(
        parameters.Values(),
        ShaderParameterLocation{2, 0},
        &ShaderParameterBindingValue::Location);
    ASSERT_NE(value, parameters.Values().end());
    ASSERT_EQ(value->ConstantData.size(), 32u);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 0), 0.75f);
    EXPECT_FLOAT_EQ(ReadFloat(value->ConstantData, 16), 0.0f);

    auto firstPlan = ResolveShaderBindings(first, PipelineBindingPolicy{});
    auto secondPlan = ResolveShaderBindings(second, PipelineBindingPolicy{});
    ASSERT_TRUE(firstPlan.Succeeded());
    ASSERT_TRUE(secondPlan.Succeeded());
    EXPECT_TRUE(parameters.IsCompleteFor(*firstPlan.Plan));
    EXPECT_TRUE(parameters.IsCompleteFor(*secondPlan.Plan));
}

TEST(ShaderParameterSetTest, ValidatesTextureBufferStorageAndUnboundedResources) {
    const shader::ShaderInterfaceDesc interface = MakeResourceInterface();
    ASSERT_TRUE(shader::IsShaderInterfaceValid(interface));
    const vector<shader::ShaderInterfaceDesc> interfaces{interface};
    auto layout = BuildShaderParameterLayout(
        interfaces,
        PipelineBindingPolicy{},
        shader::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));

    FakeBuffer readBuffer{render::BufferDescriptor{
        .Size = 256,
        .Memory = render::MemoryType::Device,
        .Usage = render::BufferUse::Resource}};
    FakeBuffer writeBuffer{render::BufferDescriptor{
        .Size = 256,
        .Memory = render::MemoryType::Device,
        .Usage = render::BufferUse::UnorderedAccess}};
    EXPECT_TRUE(parameters.SetBuffer("TypedBuffer", &readBuffer, {0, 64}));
    EXPECT_TRUE(parameters.SetBuffer("StructuredBuffer", &readBuffer, {16, 64}));
    EXPECT_FALSE(parameters.SetBuffer("StructuredBuffer", &readBuffer, {4, 64}));
    EXPECT_FALSE(parameters.SetBuffer("StructuredBuffer", &writeBuffer, {0, 64}));
    EXPECT_TRUE(parameters.SetBuffer("RawBuffer", &writeBuffer, {0, 64}));
    EXPECT_FALSE(parameters.SetBuffer("RawBuffer", &readBuffer, {0, 64}));
    EXPECT_FALSE(parameters.SetBuffer("RawBuffer", &writeBuffer, {240, 32}));

    AssetManager assets;
    StreamingAssetRef<TextureAsset> sampled = MakeTextureAsset(
        assets,
        render::TextureUse::Resource);
    StreamingAssetRef<TextureAsset> storage = MakeTextureAsset(
        assets,
        render::TextureUse::UnorderedAccess);
    StreamingAssetRef<TextureAsset> both = MakeTextureAsset(
        assets,
        render::TextureUse::Resource | render::TextureUse::UnorderedAccess);
    ASSERT_TRUE(sampled.IsReady());
    ASSERT_TRUE(storage.IsReady());
    ASSERT_TRUE(both.IsReady());
    EXPECT_TRUE(parameters.SetTexture("SampledTexture", sampled));
    EXPECT_FALSE(parameters.SetTexture("SampledTexture", storage));
    EXPECT_TRUE(parameters.SetTexture("StorageTexture", storage));
    EXPECT_FALSE(parameters.SetTexture("StorageTexture", sampled));

    auto plan = ResolveShaderBindings(interface, PipelineBindingPolicy{});
    ASSERT_TRUE(plan.Succeeded());
    EXPECT_TRUE(parameters.IsCompleteFor(*plan.Plan));

    EXPECT_TRUE(parameters.SetTexture("UnboundedTextures", both, {}, 3));
    EXPECT_FALSE(parameters.IsCompleteFor(*plan.Plan));
    EXPECT_TRUE(parameters.SetTexture("UnboundedTextures", both, {}, 0));
    EXPECT_TRUE(parameters.SetTexture("UnboundedTextures", both, {}, 1));
    EXPECT_TRUE(parameters.SetTexture("UnboundedTextures", both, {}, 2));
    EXPECT_TRUE(parameters.IsCompleteFor(*plan.Plan));

    readBuffer.Destroy();
    EXPECT_FALSE(parameters.IsCompleteFor(*plan.Plan));
}

}  // namespace radray
