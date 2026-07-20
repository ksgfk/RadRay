#include <algorithm>
#include <optional>

#include <gtest/gtest.h>

#include <fmt/format.h>

#include <radray/shader/shader_interface.h>
#if defined(RADRAY_ENABLE_DXC)
#include <radray/shader/dxc.h>
#endif
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/shader/spvc.h>
#endif

namespace radray::shader {
namespace {

void AppendFields(string& output, const vector<ShaderInterfaceFieldDesc>& fields, uint32_t depth) {
    for (const ShaderInterfaceFieldDesc& field : fields) {
        output += fmt::format(
            "{}field {} offset={} size={} scalar={} rows={} columns={} array={} arrayStride={} matrixStride={} typeBytes={} rowMajor={}\n",
            string(depth * 2, ' '),
            field.Name,
            field.Offset,
            field.Size,
            static_cast<uint32_t>(field.Type.Scalar),
            field.Type.Rows,
            field.Type.Columns,
            field.Type.ArrayCount,
            field.Type.ArrayStride,
            field.Type.MatrixStride,
            field.Type.ByteSize,
            field.Type.RowMajor);
        AppendFields(output, field.Members, depth + 1);
    }
}

string Describe(const ShaderStageInterfaceDesc& value) {
    string output = fmt::format("stage={}\n", value.Stage);
    for (const ShaderBindingGroupInterfaceDesc& group : value.BindingGroups) {
        output += fmt::format("group {}\n", group.GroupIndex);
        for (const ShaderBindingDesc& binding : group.Bindings) {
            output += fmt::format(
                " binding {} {} kind={} access={} count={} stages={}\n",
                binding.BindingIndex,
                binding.Name,
                static_cast<uint32_t>(binding.Kind),
                static_cast<uint32_t>(binding.Access),
                binding.Count,
                binding.Stages.value());
            if (binding.Buffer.has_value()) {
                output += fmt::format(
                    "  buffer bytes={} stride={}\n",
                    binding.Buffer->ByteSize,
                    binding.Buffer->ElementStride);
                AppendFields(output, binding.Buffer->Fields, 2);
            }
            if (binding.Texture.has_value()) {
                output += fmt::format(
                    "  texture dim={} sample={} array={} ms={} depth={}\n",
                    static_cast<uint32_t>(binding.Texture->Dimension),
                    static_cast<uint32_t>(binding.Texture->SampleType),
                    binding.Texture->Arrayed,
                    binding.Texture->Multisampled,
                    binding.Texture->Depth);
            }
        }
    }
    const auto appendIo = [&](std::string_view label, const vector<ShaderStageIoDesc>& values) {
        for (const ShaderStageIoDesc& io : values) {
            output += fmt::format(
                "{} {}{} location={} builtin={} scalar={} rows={} columns={} array={} arrayStride={} matrixStride={} bytes={} rowMajor={}\n",
                label,
                io.SemanticName,
                io.SemanticIndex,
                io.Location,
                static_cast<uint32_t>(io.Builtin),
                static_cast<uint32_t>(io.Type.Scalar),
                io.Type.Rows,
                io.Type.Columns,
                io.Type.ArrayCount,
                io.Type.ArrayStride,
                io.Type.MatrixStride,
                io.Type.ByteSize,
                io.Type.RowMajor);
        }
    };
    appendIo("input", value.Inputs);
    appendIo("output", value.Outputs);
    return output;
}

string DescribeByteDifference(const vector<byte>& lhs, const vector<byte>& rhs) {
    const size_t count = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < count; ++i) {
        if (lhs[i] != rhs[i]) {
            return fmt::format(
                "first byte difference at {}: {} != {} (sizes {} and {})",
                i,
                std::to_integer<uint32_t>(lhs[i]),
                std::to_integer<uint32_t>(rhs[i]),
                lhs.size(),
                rhs.size());
        }
    }
    return fmt::format("common prefix has {} bytes; sizes {} and {}", count, lhs.size(), rhs.size());
}

ShaderBindingDesc MakeSampler(std::string_view name, uint32_t binding, ShaderStage stage) {
    return ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = ShaderBindingKind::Sampler,
        .Access = ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = stage};
}

ShaderInterfaceFieldDesc MakeFloatField(
    std::string_view name,
    uint32_t offset,
    uint32_t columns = 4) {
    return ShaderInterfaceFieldDesc{
        .Name = string{name},
        .Offset = offset,
        .Size = columns * 4,
        .Type = ShaderValueTypeDesc{
            .Scalar = ShaderScalarType::Float32,
            .Rows = 1,
            .Columns = columns,
            .ArrayCount = 1,
            .ByteSize = columns * 4}};
}

ShaderBindingDesc MakeConstantBuffer(
    std::string_view name,
    uint32_t byteSize,
    vector<ShaderInterfaceFieldDesc> fields) {
    return ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = 0,
        .Kind = ShaderBindingKind::ConstantBuffer,
        .Access = ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = ShaderStage::Pixel,
        .Buffer = ShaderBufferInterfaceDesc{
            .ByteSize = byteSize,
            .Fields = std::move(fields)}};
}

TEST(ShaderInterfaceTest, ConstantBufferAbiProjectionIsDirectional) {
    const ShaderBindingDesc complete = MakeConstantBuffer(
        "Complete",
        32,
        {MakeFloatField("Color", 0), MakeFloatField("Params", 16)});
    ShaderBindingDesc projection = MakeConstantBuffer(
        "Projection",
        16,
        {MakeFloatField("RenamedByReflection", 0)});
    projection.Stages = ShaderStage::Vertex;

    EXPECT_FALSE(AreShaderBindingsAbiCompatible(projection, complete));
    EXPECT_TRUE(IsShaderBindingAbiProjectionOf(projection, complete));
    EXPECT_FALSE(IsShaderBindingAbiProjectionOf(complete, projection));

    ShaderBindingDesc wrongOffset = projection;
    wrongOffset.Buffer->ByteSize = 32;
    wrongOffset.Buffer->Fields.front().Offset = 8;
    EXPECT_FALSE(IsShaderBindingAbiProjectionOf(wrongOffset, complete));

    ShaderBindingDesc wrongType = projection;
    wrongType.Buffer->Fields.front().Type.Scalar = ShaderScalarType::UInt32;
    EXPECT_FALSE(IsShaderBindingAbiProjectionOf(wrongType, complete));
}

TEST(ShaderInterfaceTest, ConstantBufferAbiProjectionExpandsStructArrays) {
    const ShaderInterfaceFieldDesc element = MakeFloatField("Value", 0);
    const ShaderInterfaceFieldDesc array{
        .Name = "Items",
        .Offset = 0,
        .Size = 32,
        .Type = ShaderValueTypeDesc{
            .Rows = 1,
            .Columns = 1,
            .ArrayCount = 2,
            .ArrayStride = 16,
            .ByteSize = 32},
        .Members = {element}};
    const ShaderBindingDesc complete = MakeConstantBuffer("Complete", 32, {array});

    ShaderInterfaceFieldDesc second = element;
    second.Offset = 16;
    const ShaderBindingDesc projection = MakeConstantBuffer("Projection", 32, {second});
    EXPECT_TRUE(IsShaderBindingAbiProjectionOf(projection, complete));

    const ShaderBindingDesc empty = MakeConstantBuffer("Empty", 16, {});
    EXPECT_TRUE(IsShaderBindingAbiProjectionOf(empty, complete));
}

TEST(ShaderInterfaceTest, NonConstantBufferProjectionRequiresStrictAbi) {
    ShaderBindingDesc complete = MakeSampler("Complete", 3, ShaderStage::Pixel);
    ShaderBindingDesc projection = complete;
    projection.Name = "Projection";
    projection.Stages = ShaderStage::Vertex;
    EXPECT_TRUE(IsShaderBindingAbiProjectionOf(projection, complete));

    projection.Count = 2;
    EXPECT_FALSE(IsShaderBindingAbiProjectionOf(projection, complete));
}

TEST(ShaderInterfaceTest, HashIsIndependentOfDeclarationOrder) {
    ShaderStageInterfaceDesc first{
        .Stage = ShaderStage::Pixel,
        .BindingGroups = {
            ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 4,
                .Bindings = {MakeSampler("Second", 8, ShaderStage::Pixel)}},
            ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 2,
                .Bindings = {MakeSampler("First", 1, ShaderStage::Pixel)}}}};
    ShaderStageInterfaceDesc second{
        .Stage = ShaderStage::Pixel,
        .BindingGroups = {
            ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 2,
                .Bindings = {MakeSampler("First", 1, ShaderStage::Pixel)}},
            ShaderBindingGroupInterfaceDesc{
                .GroupIndex = 4,
                .Bindings = {MakeSampler("Second", 8, ShaderStage::Pixel)}}}};

    ASSERT_TRUE(IsShaderStageInterfaceValid(first));
    ASSERT_TRUE(IsShaderStageInterfaceValid(second));
    EXPECT_EQ(HashShaderStageInterface(first), HashShaderStageInterface(second));
    const auto serialized = SerializeShaderStageInterface(first);
    ASSERT_TRUE(serialized.has_value());
    const auto deserialized = DeserializeShaderStageInterface(*serialized);
    ASSERT_TRUE(deserialized.has_value());
    EXPECT_EQ(HashShaderStageInterface(*deserialized), HashShaderStageInterface(first));

    vector<byte> corrupted = *serialized;
    corrupted.back() ^= byte{0x1};
    EXPECT_FALSE(DeserializeShaderStageInterface(corrupted).has_value());
}

TEST(ShaderInterfaceTest, GraphicsMergeRejectsConflictingBindings) {
    ShaderStageInterfaceDesc vertex{
        .Stage = ShaderStage::Vertex,
        .BindingGroups = {ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 3,
            .Bindings = {MakeSampler("Shared", 0, ShaderStage::Vertex)}}}};
    ShaderBindingDesc texture{
        .Name = "Shared",
        .BindingIndex = 0,
        .Kind = ShaderBindingKind::SampledTexture,
        .Access = ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = ShaderStage::Pixel,
        .Texture = ShaderTextureInterfaceDesc{
            .Dimension = ShaderTextureDimension::Dim2D,
            .SampleType = ShaderSampleType::Float}};
    ShaderStageInterfaceDesc pixel{
        .Stage = ShaderStage::Pixel,
        .BindingGroups = {ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 3,
            .Bindings = {std::move(texture)}}}};

    const ShaderDiagnosticContext context{
        .Target = ShaderTarget::SPIRV,
        .PassIndex = 3,
        .VariantDefines = {"TEST=1"}};
    ShaderInterfaceBuildResult result = MergeGraphicsStageInterfaces(vertex, pixel, context);
    ASSERT_FALSE(result.Succeeded());
    ASSERT_EQ(result.Diagnostics.size(), 1u);
    EXPECT_EQ(result.Diagnostics[0].Code, ShaderDiagnosticCode::IncompatibleBinding);
    EXPECT_EQ(result.Diagnostics[0].Context.Target, ShaderTarget::SPIRV);
    EXPECT_EQ(result.Diagnostics[0].Context.PassIndex, 3u);
    EXPECT_EQ(result.Diagnostics[0].Context.VariantDefines, (vector<string>{"TEST=1"}));
    EXPECT_EQ(result.Diagnostics[0].Context.Group, 3u);
    EXPECT_EQ(result.Diagnostics[0].Context.Binding, 0u);
}

TEST(ShaderInterfaceTest, BuildsComputeProgramInterface) {
    ShaderStageInterfaceDesc stage{
        .Stage = ShaderStage::Compute,
        .Compute = ShaderComputeInterfaceDesc{8, 4, 1}};
    ShaderInterfaceBuildResult result = BuildComputeShaderInterface(stage);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.Interface->Kind, ShaderProgramKind::Compute);
    ASSERT_TRUE(result.Interface->Compute.has_value());
    EXPECT_EQ(result.Interface->Compute->GroupSizeX, 8u);
    const auto serialized = SerializeShaderInterface(*result.Interface);
    ASSERT_TRUE(serialized.has_value());
    const auto deserialized = DeserializeShaderInterface(*serialized);
    ASSERT_TRUE(deserialized.has_value());
    EXPECT_EQ(*deserialized, *result.Interface);
}

TEST(ShaderInterfaceTest, RejectsMalformedProgrammaticAbiDescriptors) {
    ShaderInterfaceDesc valid{
        .Kind = ShaderProgramKind::Graphics,
        .BindingGroups = {ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 0,
            .Bindings = {MakeSampler("Sampler", 0, ShaderStage::Pixel)}}}};
    ASSERT_TRUE(IsShaderInterfaceValid(valid));

    ShaderInterfaceDesc invalidKind = valid;
    invalidKind.Kind = static_cast<ShaderProgramKind>(0xff);
    EXPECT_FALSE(IsShaderInterfaceValid(invalidKind));

    ShaderInterfaceDesc invalidStage = valid;
    invalidStage.BindingGroups[0].Bindings[0].Stages = ShaderStages{0x80};
    EXPECT_FALSE(IsShaderInterfaceValid(invalidStage));

    ShaderInterfaceDesc invalidAccess = valid;
    invalidAccess.BindingGroups[0].Bindings[0].Access =
        static_cast<ShaderResourceAccess>(0xff);
    EXPECT_FALSE(IsShaderInterfaceValid(invalidAccess));

    ShaderInterfaceDesc invalidStructured = valid;
    invalidStructured.BindingGroups[0].Bindings[0] = ShaderBindingDesc{
        .Name = "Structured",
        .BindingIndex = 0,
        .Kind = ShaderBindingKind::StructuredBuffer,
        .Access = ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = ShaderStage::Pixel,
        .Buffer = ShaderBufferInterfaceDesc{}};
    EXPECT_FALSE(IsShaderInterfaceValid(invalidStructured));
    invalidStructured.BindingGroups[0].Bindings[0].Buffer->ElementStride = 16;
    EXPECT_TRUE(IsShaderInterfaceValid(invalidStructured));

    ShaderInterfaceDesc invalidTyped = valid;
    invalidTyped.BindingGroups[0].Bindings[0] = ShaderBindingDesc{
        .Name = "Typed",
        .BindingIndex = 0,
        .Kind = ShaderBindingKind::TypedBuffer,
        .Access = ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = ShaderStage::Pixel,
        .Texture = ShaderTextureInterfaceDesc{
            .Dimension = ShaderTextureDimension::Dim2D,
            .SampleType = ShaderSampleType::Float}};
    EXPECT_FALSE(IsShaderInterfaceValid(invalidTyped));
    invalidTyped.BindingGroups[0].Bindings[0].Texture->Dimension =
        ShaderTextureDimension::Buffer;
    EXPECT_TRUE(IsShaderInterfaceValid(invalidTyped));

    ShaderStageInterfaceDesc wrongStageVisibility{
        .Stage = ShaderStage::Vertex,
        .BindingGroups = {ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 0,
            .Bindings = {MakeSampler("PixelSampler", 0, ShaderStage::Pixel)}}}};
    EXPECT_FALSE(IsShaderStageInterfaceValid(wrongStageVisibility));
}

#if defined(RADRAY_ENABLE_DXC) && defined(RADRAY_ENABLE_SPIRV_CROSS)

constexpr std::string_view kPortableGraphicsShader = R"hlsl(
#ifdef VULKAN
#define RR_BINDING(b, s) [[vk::binding(b, s)]]
#else
#define RR_BINDING(b, s)
#endif

struct MaterialData {
    float4 Color;
    float Roughness;
    float3 Padding;
    float4x4 Transform;
    float4 Weights[2];
};

RR_BINDING(0, 2) ConstantBuffer<MaterialData> gMaterial : register(b0, space2);
RR_BINDING(1, 2) Texture2D<float4> gTexture : register(t1, space2);
RR_BINDING(2, 2) SamplerState gSampler : register(s2, space2);

struct VertexInput {
    float3 Position : POSITION0;
    float2 UV : TEXCOORD0;
};

struct VertexOutput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.Position = mul(gMaterial.Transform, float4(input.Position, 1.0)) + gMaterial.Weights[0];
    output.UV = input.UV;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    return gTexture.Sample(gSampler, input.UV) * gMaterial.Color;
}
)hlsl";

constexpr std::string_view kPortableComputeShader = R"hlsl(
#ifdef VULKAN
#define RR_BINDING(b, s) [[vk::binding(b, s)]]
#else
#define RR_BINDING(b, s)
#endif

struct Item {
    float3 Position;
    uint Id;
};

RR_BINDING(0, 4) StructuredBuffer<Item> gInput : register(t0, space4);
RR_BINDING(1, 4) RWStructuredBuffer<Item> gOutput : register(u1, space4);
RR_BINDING(2, 4) ByteAddressBuffer gRaw : register(t2, space4);
RR_BINDING(3, 4) RWTexture2D<float4> gImage : register(u3, space4);
RR_BINDING(4, 4) Buffer<float4> gTyped : register(t4, space4);
RR_BINDING(5, 4) RWBuffer<uint> gTypedOutput : register(u5, space4);

[numthreads(8, 4, 2)]
void CSMain(uint3 threadId : SV_DispatchThreadID) {
    Item value = gInput[threadId.x];
    value.Id += gRaw.Load(threadId.x * 4);
    value.Position += gTyped[threadId.x].xyz;
    gOutput[threadId.x] = value;
    gImage[threadId.xy] = float4(value.Position, 1.0);
    gTypedOutput[threadId.x] = value.Id;
}
)hlsl";

std::optional<ShaderStageInterfaceDesc> CompileAndNormalize(
    Dxc& dxc,
    std::string_view code,
    ShaderTarget target,
    ShaderStage stage,
    std::string_view entryPoint) {
    DxcCompileOptions options{
        .EntryPoint = entryPoint,
        .Stage = stage,
        .SM = HlslShaderModel::SM60,
        .IsOptimize = true,
        .IsSpirv = target == ShaderTarget::SPIRV,
        .EnableUnbounded = false};
    auto compiled = dxc.CompileMemory(code, "portable_interface.hlsl", options);
    if (!compiled.has_value()) return std::nullopt;

    ShaderInterfaceNormalizationOptions normalization;
    normalization.Context.Target = target;
    normalization.Context.Stage = stage;
    if (target == ShaderTarget::DXIL) {
        auto reflection = dxc.GetShaderDescFromOutput(compiled->Refl);
        if (!reflection.has_value()) return std::nullopt;
        auto result = NormalizeHlslInterface(*reflection, stage, normalization);
        if (!result.Succeeded()) {
            ADD_FAILURE() << (result.Diagnostics.empty() ? "HLSL normalization failed without a diagnostic" : result.Diagnostics[0].Message);
        }
        return result.Succeeded() ? std::move(result.Interface) : std::nullopt;
    }
    auto reflection = ReflectSpirv(SpirvBytecodeView{
        .Data = compiled->Data,
        .EntryPointName = entryPoint,
        .Stage = stage});
    if (!reflection.has_value()) return std::nullopt;
    auto result = NormalizeSpirvInterface(*reflection, stage, normalization);
    if (!result.Succeeded()) {
        ADD_FAILURE() << (result.Diagnostics.empty() ? "SPIR-V normalization failed without a diagnostic" : result.Diagnostics[0].Message);
    }
    return result.Succeeded() ? std::move(result.Interface) : std::nullopt;
}

TEST(ShaderInterfaceTest, DxilAndSpirvProduceEquivalentGraphicsInterface) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto dxilVertex = CompileAndNormalize(*dxc, kPortableGraphicsShader, ShaderTarget::DXIL, ShaderStage::Vertex, "VSMain");
    auto dxilPixel = CompileAndNormalize(*dxc, kPortableGraphicsShader, ShaderTarget::DXIL, ShaderStage::Pixel, "PSMain");
    auto spirvVertex = CompileAndNormalize(*dxc, kPortableGraphicsShader, ShaderTarget::SPIRV, ShaderStage::Vertex, "VSMain");
    auto spirvPixel = CompileAndNormalize(*dxc, kPortableGraphicsShader, ShaderTarget::SPIRV, ShaderStage::Pixel, "PSMain");
    ASSERT_TRUE(dxilVertex.has_value());
    ASSERT_TRUE(dxilPixel.has_value());
    ASSERT_TRUE(spirvVertex.has_value());
    ASSERT_TRUE(spirvPixel.has_value());

    const auto dxilVertexBytes = SerializeShaderStageInterface(*dxilVertex);
    const auto spirvVertexBytes = SerializeShaderStageInterface(*spirvVertex);
    const auto dxilPixelBytes = SerializeShaderStageInterface(*dxilPixel);
    const auto spirvPixelBytes = SerializeShaderStageInterface(*spirvPixel);
    ASSERT_TRUE(dxilVertexBytes.has_value());
    ASSERT_TRUE(spirvVertexBytes.has_value());
    ASSERT_TRUE(dxilPixelBytes.has_value());
    ASSERT_TRUE(spirvPixelBytes.has_value());
    EXPECT_EQ(HashShaderStageInterface(*dxilVertex), HashShaderStageInterface(*spirvVertex))
        << DescribeByteDifference(*dxilVertexBytes, *spirvVertexBytes)
        << "\nDXIL:\n"
        << Describe(*dxilVertex) << "SPIR-V:\n"
        << Describe(*spirvVertex);
    EXPECT_EQ(HashShaderStageInterface(*dxilPixel), HashShaderStageInterface(*spirvPixel))
        << DescribeByteDifference(*dxilPixelBytes, *spirvPixelBytes)
        << "\nDXIL:\n"
        << Describe(*dxilPixel) << "SPIR-V:\n"
        << Describe(*spirvPixel);

    auto dxilProgram = MergeGraphicsStageInterfaces(*dxilVertex, *dxilPixel);
    auto spirvProgram = MergeGraphicsStageInterfaces(*spirvVertex, *spirvPixel);
    ASSERT_TRUE(dxilProgram.Succeeded());
    ASSERT_TRUE(spirvProgram.Succeeded())
        << (spirvProgram.Diagnostics.empty() ? "no diagnostic" : spirvProgram.Diagnostics[0].Message);
    EXPECT_EQ(*dxilProgram.Interface, *spirvProgram.Interface);
    EXPECT_EQ(HashShaderInterface(*dxilProgram.Interface), HashShaderInterface(*spirvProgram.Interface));
}

TEST(ShaderInterfaceTest, DxilAndSpirvProduceEquivalentComputeInterface) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto dxil = CompileAndNormalize(*dxc, kPortableComputeShader, ShaderTarget::DXIL, ShaderStage::Compute, "CSMain");
    auto spirv = CompileAndNormalize(*dxc, kPortableComputeShader, ShaderTarget::SPIRV, ShaderStage::Compute, "CSMain");
    ASSERT_TRUE(dxil.has_value());
    ASSERT_TRUE(spirv.has_value());
    const auto dxilBytes = SerializeShaderStageInterface(*dxil);
    const auto spirvBytes = SerializeShaderStageInterface(*spirv);
    ASSERT_TRUE(dxilBytes.has_value());
    ASSERT_TRUE(spirvBytes.has_value());
    EXPECT_EQ(HashShaderStageInterface(*dxil), HashShaderStageInterface(*spirv))
        << DescribeByteDifference(*dxilBytes, *spirvBytes)
        << "\nDXIL:\n"
        << Describe(*dxil) << "SPIR-V:\n"
        << Describe(*spirv);

    ASSERT_EQ(dxil->BindingGroups.size(), 1u);
    ASSERT_EQ(dxil->BindingGroups.front().GroupIndex, 4u);
    const auto& bindings = dxil->BindingGroups.front().Bindings;
    ASSERT_EQ(bindings.size(), 6u);
    EXPECT_EQ(bindings[0].Kind, ShaderBindingKind::StructuredBuffer);
    ASSERT_TRUE(bindings[0].Buffer.has_value());
    EXPECT_EQ(bindings[0].Buffer->ElementStride, 16u);
    EXPECT_EQ(bindings[0].Access, ShaderResourceAccess::ReadOnly);
    EXPECT_EQ(bindings[1].Kind, ShaderBindingKind::StructuredBuffer);
    ASSERT_TRUE(bindings[1].Buffer.has_value());
    EXPECT_EQ(bindings[1].Buffer->ElementStride, 16u);
    EXPECT_EQ(bindings[1].Access, ShaderResourceAccess::ReadWrite);
    EXPECT_EQ(bindings[2].Kind, ShaderBindingKind::RawBuffer);
    EXPECT_EQ(bindings[3].Kind, ShaderBindingKind::StorageTexture);
    EXPECT_EQ(bindings[3].Access, ShaderResourceAccess::ReadWrite);
    EXPECT_EQ(bindings[4].Kind, ShaderBindingKind::TypedBuffer);
    ASSERT_TRUE(bindings[4].Texture.has_value());
    EXPECT_EQ(bindings[4].Texture->Dimension, ShaderTextureDimension::Buffer);
    EXPECT_EQ(bindings[4].Texture->SampleType, ShaderSampleType::Float);
    EXPECT_EQ(bindings[4].Access, ShaderResourceAccess::ReadOnly);
    EXPECT_EQ(bindings[5].Kind, ShaderBindingKind::TypedBuffer);
    ASSERT_TRUE(bindings[5].Texture.has_value());
    EXPECT_EQ(bindings[5].Texture->Dimension, ShaderTextureDimension::Buffer);
    EXPECT_EQ(bindings[5].Texture->SampleType, ShaderSampleType::UInt);
    EXPECT_EQ(bindings[5].Access, ShaderResourceAccess::ReadWrite);

    auto dxilProgram = BuildComputeShaderInterface(*dxil);
    auto spirvProgram = BuildComputeShaderInterface(*spirv);
    ASSERT_TRUE(dxilProgram.Succeeded());
    ASSERT_TRUE(spirvProgram.Succeeded());
    EXPECT_EQ(HashShaderInterface(*dxilProgram.Interface), HashShaderInterface(*spirvProgram.Interface));
}

#endif

}  // namespace
}  // namespace radray::shader
