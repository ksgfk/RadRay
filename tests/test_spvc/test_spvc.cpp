#include <gtest/gtest.h>

#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

using namespace radray;
using namespace radray::render;

// ============================================================
// Helper: compile HLSL to SPIR-V via DXC
// ============================================================

static std::pair<shared_ptr<Dxc>, DxcOutput> CompileSpirv(
    const char* code, const char* entry, ShaderStage stage) {
    auto dxc = CreateDxc();
    auto out = dxc->Compile(code, entry, stage, HlslShaderModel::SM60, true, {}, {}, true);
    return {dxc.Unwrap(), std::move(out.value())};
}

// ============================================================
// Shader sources
// ============================================================

static const char* SIMPLE_VS = R"(
struct VS_INPUT {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float2 uv  : TEXCOORD0;
};
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
PS_INPUT VSMain(VS_INPUT vsIn) {
    PS_INPUT o;
    o.pos = float4(vsIn.pos, 1.0f);
    o.uv = vsIn.uv;
    return o;
}
)";

static const char* SIMPLE_PS = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
float4 PSMain(PS_INPUT psIn) : SV_Target {
    return float4(psIn.uv, 0.0f, 1.0f);
}
)";

static const char* SIMPLE_CS = R"(
RWStructuredBuffer<float4> _Output : register(u0);
[numthreads(64, 2, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    _Output[dtid.x] = float4(dtid.x, dtid.y, dtid.z, 1.0f);
}
)";

static const char* SHADER_WITH_RESOURCES = R"(
struct DirLight {
    float4 lightDirW;
    float4 lightColor;
};

struct GlobalData {
    DirLight dirLight;
    float time;
};

struct PreCameraData {
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float3 posW;
};

struct VS_INPUT {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 nor : NORMAL0;
    [[vk::location(2)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv  : TEXCOORD0;
};

struct PreObjectData {
    float4x4 model;
    float4x4 mvp;
    float4x4 modelInv;
};

[[vk::push_constant]] ConstantBuffer<PreObjectData> _Obj : register(b0);
[[vk::binding(0)]] ConstantBuffer<PreCameraData> _Camera : register(b1);
[[vk::binding(1)]] ConstantBuffer<GlobalData> _Global : register(b2);
[[vk::binding(2)]] cbuffer TestCBuffer : register(b3) {
    float4 _SomeValue;
};

[[vk::binding(3)]] Texture2D _AlbedoTex : register(t0);
[[vk::binding(5)]] SamplerState _AlbedoSampler : register(s0);

PS_INPUT VSMain(VS_INPUT vsIn) {
    PS_INPUT psIn;
    psIn.pos = mul(float4(vsIn.pos + _Camera.posW, 1.0f), _Obj.mvp);
    psIn.posW = mul(float4(vsIn.pos + _Global.time, 1.0f), _Obj.model).xyz;
    psIn.norW = normalize(mul(float4(vsIn.nor, 0.0f), _Obj.modelInv).xyz);
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target {
    float3 albedo = _AlbedoTex.Sample(_AlbedoSampler, psIn.uv).xyz;
    float3 t = albedo * ((float3)1.0 / _Global.dirLight.lightDirW.xyz) + ((float3)1.0 / _Camera.posW);
    return float4(t, 1.0f) + _SomeValue;
}
)";

static const char* SHADER_CBUFFER_ARRAY = R"(
struct PointLight {
    float3 posW;
    float radius;
    float3 lightColor;
    float intensity;
    float2 _test;
};

struct DirectionalLight {
    float3 lightDirW;
    float _pad1;
    float3 lightColor;
    float _pad2;
};

struct Lights {
    DirectionalLight dl;
    PointLight pl[4];
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv : TEXCOORD0;
};

ConstantBuffer<Lights> _Lights : register(b0);

float4 PSMain(PS_INPUT psIn) : SV_Target {
    float3 color = float3(0, 0, 0);
    float3 n = normalize(psIn.norW);
    float3 l_dir = normalize(-_Lights.dl.lightDirW);
    float n_dot_l = max(dot(n, l_dir), 0.0f);
    color += _Lights.dl.lightColor * n_dot_l;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float3 l_vec = _Lights.pl[i].posW - psIn.posW;
        float dist = length(l_vec);
        float3 l = l_vec / max(dist, 1e-4f);
        float att = saturate(1.0f - dist / _Lights.pl[i].radius);
        float n_dot_l_pl = max(dot(n, l), 0.0f);
        color += _Lights.pl[i].lightColor * _Lights.pl[i].intensity * n_dot_l_pl * att;
    }
    return float4(color, 1.0f);
}
)";

static const char* SHADER_CBUFFER_IN_CBUFFER = R"(
struct PointLight {
    float3 posW;
    float radius;
    float3 lightColor;
    float intensity;
    float2 _test;
};

struct DirectionalLight {
    float3 lightDirW;
    float _pad1;
    float3 lightColor;
    float _pad2;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv : TEXCOORD0;
};

ConstantBuffer<DirectionalLight> _DirectionalLight : register(b0);
cbuffer PointLightsCB : register(b3) {
    PointLight _PointLights[4];
};

float4 PSMain(PS_INPUT psIn) : SV_Target {
    float3 color = float3(0, 0, 0);
    float3 n = normalize(psIn.norW);
    float3 l_dir = normalize(-_DirectionalLight.lightDirW);
    float n_dot_l = max(dot(n, l_dir), 0.0f);
    color += _DirectionalLight.lightColor * n_dot_l;
    [unroll]
    for (int i = 0; i < 4; i++) {
        float3 l_vec = _PointLights[i].posW - psIn.posW;
        float dist = length(l_vec);
        float3 l = l_vec / max(dist, 1e-4f);
        float att = saturate(1.0f - dist / _PointLights[i].radius);
        float n_dot_l_pl = max(dot(n, l), 0.0f);
        color += _PointLights[i].lightColor * _PointLights[i].intensity * n_dot_l_pl * att;
    }
    return float4(color, 1.0f);
}
)";

static const char* SHADER_STORAGE_BUFFER = R"(
struct Data {
    float4 value;
};
StructuredBuffer<Data> _Input : register(t0);
RWStructuredBuffer<Data> _Output : register(u0);
[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    _Output[dtid.x].value = _Input[dtid.x].value * 2.0f;
}
)";

// ============================================================
// Test: Basic single-stage reflection
// ============================================================

TEST(SPVC, ReflectSingleVertexStage) {
    auto [dxc, vs] = CompileSpirv(SIMPLE_VS, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->UsedStages, ShaderStage::Vertex);
}

TEST(SPVC, ReflectSinglePixelStage) {
    auto [dxc, ps] = CompileSpirv(SIMPLE_PS, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->UsedStages, ShaderStage::Pixel);
}

TEST(SPVC, ReflectSingleComputeStage) {
    auto [dxc, cs] = CompileSpirv(SIMPLE_CS, "CSMain", ShaderStage::Compute);
    SpirvBytecodeView bytecodes[] = {{.Data = cs.Data, .EntryPointName = "CSMain", .Stage = ShaderStage::Compute}};
    const DxcReflectionRadrayExt* exts[] = {&cs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->UsedStages, ShaderStage::Compute);
}

// ============================================================
// Test: Multi-stage reflection and stage merging
// ============================================================

TEST(SPVC, ReflectMultiStage) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();

    SpirvBytecodeView bytecodes[] = {
        {.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex},
        {.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt, &ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // UsedStages should combine both
    EXPECT_EQ(desc->UsedStages, ShaderStage::Graphics);
}

TEST(SPVC, MergedResourceStageFlags) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();

    SpirvBytecodeView bytecodes[] = {
        {.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex},
        {.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt, &ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // Resources used in both stages should have merged stage flags
    for (const auto& res : desc->ResourceBindings) {
        if (res.Name == "_Camera" || res.Name == "_Global") {
            // Used in both VS and PS
            EXPECT_EQ(res.Stages, ShaderStage::Graphics);
        }
    }
}

// ============================================================
// Test: Vertex input reflection
// ============================================================

TEST(SPVC, VertexInputs) {
    auto [dxc, vs] = CompileSpirv(SIMPLE_VS, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // VS_INPUT: float3 pos (location 0), float2 uv (location 1)
    ASSERT_EQ(desc->VertexInputs.size(), 2u);
    // Sorted by location
    EXPECT_EQ(desc->VertexInputs[0].Location, 0u);
    EXPECT_EQ(desc->VertexInputs[0].Format, VertexFormat::FLOAT32X3);
    EXPECT_EQ(desc->VertexInputs[1].Location, 1u);
    EXPECT_EQ(desc->VertexInputs[1].Format, VertexFormat::FLOAT32X2);
}

TEST(SPVC, VertexInputsThreeAttributes) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // VS_INPUT: float3 pos (0), float3 nor (1), float2 uv (2)
    ASSERT_EQ(desc->VertexInputs.size(), 3u);
    EXPECT_EQ(desc->VertexInputs[0].Location, 0u);
    EXPECT_EQ(desc->VertexInputs[0].Format, VertexFormat::FLOAT32X3);
    EXPECT_EQ(desc->VertexInputs[1].Location, 1u);
    EXPECT_EQ(desc->VertexInputs[1].Format, VertexFormat::FLOAT32X3);
    EXPECT_EQ(desc->VertexInputs[2].Location, 2u);
    EXPECT_EQ(desc->VertexInputs[2].Format, VertexFormat::FLOAT32X2);
}

TEST(SPVC, PixelShaderHasNoVertexInputs) {
    auto [dxc, ps] = CompileSpirv(SIMPLE_PS, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());
    EXPECT_TRUE(desc->VertexInputs.empty());
}

// ============================================================
// Test: Compute info
// ============================================================

TEST(SPVC, ComputeThreadGroupSize) {
    auto [dxc, cs] = CompileSpirv(SIMPLE_CS, "CSMain", ShaderStage::Compute);
    SpirvBytecodeView bytecodes[] = {{.Data = cs.Data, .EntryPointName = "CSMain", .Stage = ShaderStage::Compute}};
    const DxcReflectionRadrayExt* exts[] = {&cs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    ASSERT_TRUE(desc->ComputeInfo.has_value());
    EXPECT_EQ(desc->ComputeInfo->LocalSizeX, 64u);
    EXPECT_EQ(desc->ComputeInfo->LocalSizeY, 2u);
    EXPECT_EQ(desc->ComputeInfo->LocalSizeZ, 1u);
}

TEST(SPVC, NonComputeHasNoComputeInfo) {
    auto [dxc, vs] = CompileSpirv(SIMPLE_VS, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());
    EXPECT_FALSE(desc->ComputeInfo.has_value());
}

// ============================================================
// Test: Resource binding reflection
// ============================================================

TEST(SPVC, ResourceBindingKinds) {
    auto [dxc, ps] = CompileSpirv(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    bool hasUniformBuffer = false;
    bool hasSeparateImage = false;
    bool hasSeparateSampler = false;
    for (const auto& res : desc->ResourceBindings) {
        if (res.Kind == SpirvResourceKind::UniformBuffer) hasUniformBuffer = true;
        if (res.Kind == SpirvResourceKind::SeparateImage) hasSeparateImage = true;
        if (res.Kind == SpirvResourceKind::SeparateSampler) hasSeparateSampler = true;
    }
    EXPECT_TRUE(hasUniformBuffer);
    EXPECT_TRUE(hasSeparateImage);
    EXPECT_TRUE(hasSeparateSampler);
}

TEST(SPVC, StorageBufferReflection) {
    auto [dxc, cs] = CompileSpirv(SHADER_STORAGE_BUFFER, "CSMain", ShaderStage::Compute);
    SpirvBytecodeView bytecodes[] = {{.Data = cs.Data, .EntryPointName = "CSMain", .Stage = ShaderStage::Compute}};
    const DxcReflectionRadrayExt* exts[] = {&cs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // Should have two storage buffers: _Input (read-only) and _Output (read-write)
    int storageBufferCount = 0;
    bool hasInput = false;
    bool hasOutput = false;
    for (const auto& res : desc->ResourceBindings) {
        if (res.Kind == SpirvResourceKind::StorageBuffer) {
            storageBufferCount++;
            if (res.Name == "_Input") hasInput = true;
            if (res.Name == "_Output") hasOutput = true;
        }
    }
    EXPECT_EQ(storageBufferCount, 2);
    EXPECT_TRUE(hasInput);
    EXPECT_TRUE(hasOutput);
}

TEST(SPVC, ImageInfoForTextures) {
    auto [dxc, ps] = CompileSpirv(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        if (res.Kind == SpirvResourceKind::SeparateImage) {
            ASSERT_TRUE(res.ImageInfo.has_value());
            EXPECT_EQ(res.ImageInfo->Dim, SpirvImageDim::Dim2D);
            EXPECT_FALSE(res.ImageInfo->Arrayed);
            EXPECT_FALSE(res.ImageInfo->Multisampled);
        }
    }
}

// ============================================================
// Test: Push constant reflection
// ============================================================

TEST(SPVC, PushConstantReflection) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    ASSERT_FALSE(desc->PushConstants.empty());
    // PreObjectData: 3 float4x4 = 192 bytes
    EXPECT_EQ(desc->PushConstants[0].Size, 192u);
    EXPECT_EQ(desc->PushConstants[0].Stages, ShaderStage::Vertex);
}

TEST(SPVC, PushConstantMergedStages) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();

    SpirvBytecodeView bytecodes[] = {
        {.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex},
        {.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt, &ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // Push constants used in both stages should be merged
    // (only if PSMain also uses _Obj — it doesn't in this shader, so VS only)
    for (const auto& pc : desc->PushConstants) {
        // At minimum, vertex stage should be present
        EXPECT_TRUE(static_cast<uint32_t>(pc.Stages) & static_cast<uint32_t>(ShaderStage::Vertex));
    }
}

// ============================================================
// Test: IsViewInHlsl flag
// ============================================================

TEST(SPVC, IsViewInHlslFlag) {
    auto [dxc, ps] = CompileSpirv(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        if (res.Kind == SpirvResourceKind::UniformBuffer) {
            if (res.Name == "TestCBuffer") {
                // cbuffer should NOT be a view
                EXPECT_FALSE(res.IsViewInHlsl);
            } else {
                // ConstantBuffer<T> should be a view
                EXPECT_TRUE(res.IsViewInHlsl);
            }
        }
    }
}

// ============================================================
// Test: Type information
// ============================================================

TEST(SPVC, TypeInfoStruct) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    EXPECT_FALSE(desc->Types.empty());
    // Should have struct types
    bool hasStruct = false;
    for (const auto& t : desc->Types) {
        if (t.BaseType == SpirvBaseType::Struct) {
            hasStruct = true;
            EXPECT_FALSE(t.Members.empty());
        }
    }
    EXPECT_TRUE(hasStruct);
}

TEST(SPVC, TypeInfoMatrix) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // Should have float4x4 type (4 columns, 4 vector size)
    bool hasMatrix = false;
    for (const auto& t : desc->Types) {
        if (t.Columns == 4 && t.VectorSize == 4 && t.BaseType == SpirvBaseType::Float32) {
            hasMatrix = true;
        }
    }
    EXPECT_TRUE(hasMatrix);
}

// ============================================================
// Test: CBuffer with struct array member
// ============================================================

TEST(SPVC, CBufferArrayMember) {
    auto [dxc, ps] = CompileSpirv(SHADER_CBUFFER_ARRAY, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // Should have _Lights uniform buffer
    bool hasLights = false;
    for (const auto& res : desc->ResourceBindings) {
        if (res.Name == "_Lights" && res.Kind == SpirvResourceKind::UniformBuffer) {
            hasLights = true;
            EXPECT_GT(res.UniformBufferSize, 0u);
        }
    }
    EXPECT_TRUE(hasLights);
}

// ============================================================
// Test: cbuffer with array variable
// ============================================================

TEST(SPVC, CBufferNotViewWithArrayVariable) {
    auto [dxc, ps] = CompileSpirv(SHADER_CBUFFER_IN_CBUFFER, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    // _DirectionalLight is ConstantBuffer<T> (view), PointLightsCB is cbuffer (not view)
    for (const auto& res : desc->ResourceBindings) {
        if (res.Kind == SpirvResourceKind::UniformBuffer) {
            if (res.Name == "_DirectionalLight") {
                EXPECT_TRUE(res.IsViewInHlsl);
            } else if (res.Name == "PointLightsCB") {
                EXPECT_FALSE(res.IsViewInHlsl);
            }
        }
    }
}

// ============================================================
// Test: Resource names should NOT contain file paths
// When DXC compiles HLSL to SPIR-V, debug info may embed
// the internal source filename (e.g. "hlsl.hlsl"). Reflection
// names must be clean variable/buffer names.
// ============================================================

static bool ContainsPathLikeString(const std::string& name) {
    // Check for common path artifacts from DXC compilation
    if (name.find("hlsl.hlsl") != std::string::npos) return true;
    if (name.find(".hlsl") != std::string::npos) return true;
    if (name.find('/') != std::string::npos) return true;
    if (name.find('\\') != std::string::npos) return true;
    if (name.find("type.") == 0) return true;  // spirv-cross type prefix fallback
    return false;
}

TEST(SPVC, ResourceNamesNoFilePaths) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();

    SpirvBytecodeView bytecodes[] = {
        {.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex},
        {.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt, &ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        EXPECT_FALSE(ContainsPathLikeString(res.Name))
            << "Resource name contains path-like string: " << res.Name;
    }
    for (const auto& pc : desc->PushConstants) {
        EXPECT_FALSE(ContainsPathLikeString(pc.Name))
            << "Push constant name contains path-like string: " << pc.Name;
    }
}

TEST(SPVC, ResourceNamesNoFilePathsCompute) {
    auto [dxc, cs] = CompileSpirv(SHADER_STORAGE_BUFFER, "CSMain", ShaderStage::Compute);
    SpirvBytecodeView bytecodes[] = {{.Data = cs.Data, .EntryPointName = "CSMain", .Stage = ShaderStage::Compute}};
    const DxcReflectionRadrayExt* exts[] = {&cs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        EXPECT_FALSE(ContainsPathLikeString(res.Name))
            << "Resource name contains path-like string: " << res.Name;
    }
}

TEST(SPVC, ResourceNamesNoFilePathsCBufferArray) {
    auto [dxc, ps] = CompileSpirv(SHADER_CBUFFER_ARRAY, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        EXPECT_FALSE(ContainsPathLikeString(res.Name))
            << "Resource name contains path-like string: " << res.Name;
    }
}

// ============================================================
// Test: MapResourceBindType
// ============================================================

TEST(SPVC, MapResourceBindType) {
    auto [dxc, ps] = CompileSpirv(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        auto mapped = res.MapResourceBindType();
        if (res.Kind == SpirvResourceKind::UniformBuffer) {
            EXPECT_EQ(mapped, ResourceBindType::CBuffer);
        } else if (res.Kind == SpirvResourceKind::SeparateImage) {
            EXPECT_EQ(mapped, ResourceBindType::Texture);
        } else if (res.Kind == SpirvResourceKind::SeparateSampler) {
            EXPECT_EQ(mapped, ResourceBindType::Sampler);
        }
    }
}

// ============================================================
// Test: Error cases
// ============================================================

TEST(SPVC, InvalidSpirvDataFails) {
    std::vector<byte> garbage(16, byte{0xFF});
    SpirvBytecodeView bytecodes[] = {{.Data = garbage, .EntryPointName = "main", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt ext{};
    const DxcReflectionRadrayExt* exts[] = {&ext};
    auto desc = ReflectSpirv(bytecodes, exts);
    EXPECT_FALSE(desc.has_value());
}

TEST(SPVC, NonAlignedSpirvDataFails) {
    // SPIR-V must be 4-byte aligned
    std::vector<byte> bad(13, byte{0x00});
    SpirvBytecodeView bytecodes[] = {{.Data = bad, .EntryPointName = "main", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt ext{};
    const DxcReflectionRadrayExt* exts[] = {&ext};
    auto desc = ReflectSpirv(bytecodes, exts);
    EXPECT_FALSE(desc.has_value());
}

TEST(SPVC, EmptySpirvDataFails) {
    std::span<const byte> empty{};
    SpirvBytecodeView bytecodes[] = {{.Data = empty, .EntryPointName = "main", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt ext{};
    const DxcReflectionRadrayExt* exts[] = {&ext};
    auto desc = ReflectSpirv(bytecodes, exts);
    // Empty data is 0 bytes, which is 4-byte aligned (0 % 4 == 0), but spirv-cross should fail
    EXPECT_FALSE(desc.has_value());
}

// ============================================================
// Test: UniformBufferSize is populated
// ============================================================

TEST(SPVC, UniformBufferSizePopulated) {
    auto [dxc, vs] = CompileSpirv(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex);
    SpirvBytecodeView bytecodes[] = {{.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex}};
    const DxcReflectionRadrayExt* exts[] = {&vs.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        if (res.Kind == SpirvResourceKind::UniformBuffer) {
            EXPECT_GT(res.UniformBufferSize, 0u)
                << "UniformBuffer " << res.Name << " has zero size";
        }
    }
}

// ============================================================
// Test: Binding numbers are correct
// ============================================================

TEST(SPVC, BindingNumbers) {
    auto [dxc, ps] = CompileSpirv(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel);
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* exts[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, exts);
    ASSERT_TRUE(desc.has_value());

    for (const auto& res : desc->ResourceBindings) {
        if (res.Name == "_Camera") EXPECT_EQ(res.Binding, 0u);
        if (res.Name == "_Global") EXPECT_EQ(res.Binding, 1u);
        if (res.Name == "_AlbedoTex") EXPECT_EQ(res.Binding, 3u);
        if (res.Name == "_AlbedoSampler") EXPECT_EQ(res.Binding, 5u);
    }
}
