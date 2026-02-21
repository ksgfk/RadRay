#include <gtest/gtest.h>

#include <radray/render/dxc.h>

using namespace radray;
using namespace radray::render;

// ============================================================
// Shader sources for testing
// ============================================================

static const char* SIMPLE_VS = R"(
struct VS_INPUT {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
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
RWBuffer<float4> _Output : register(u0);
[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    _Output[dtid.x] = float4(dtid.x, 0, 0, 1);
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
    float3 pos : POSITION;
    float3 nor : NORMAL0;
    float2 uv  : TEXCOORD0;
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

ConstantBuffer<PreObjectData> _Obj : register(b0);
ConstantBuffer<PreCameraData> _Camera : register(b1);
ConstantBuffer<GlobalData> _Global : register(b2);
cbuffer TestCBuffer : register(b3) {
    float4 _SomeValue;
};

Texture2D _AlbedoTex : register(t0);
SamplerState _AlbedoSampler : register(s0);

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

static const char* SHADER_WITH_DEFINE = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
};
float4 PSMain(PS_INPUT psIn) : SV_Target {
#ifdef USE_RED
    return float4(1, 0, 0, 1);
#else
    return float4(0, 1, 0, 1);
#endif
}
)";

static const char* SHADER_WITH_SPACES = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
};
float4 PSMain(PS_INPUT psIn) : SV_Target {
    return float4(0, 0, 0, 1);
}
)";

static const char* SHADER_MULTI_REGISTER_SPACE = R"(
struct DataA { float4 value; };
struct DataB { float4 value; };
ConstantBuffer<DataA> _A : register(b0, space0);
ConstantBuffer<DataB> _B : register(b0, space1);
Texture2D _Tex0 : register(t0, space0);
Texture2D _Tex1 : register(t0, space1);
SamplerState _Samp : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(PS_INPUT psIn) : SV_Target {
    return _A.value + _B.value
        + _Tex0.Sample(_Samp, psIn.uv)
        + _Tex1.Sample(_Samp, psIn.uv);
}
)";

static const char* INVALID_SHADER = R"(
float4 PSMain() : SV_Target {
    this is not valid hlsl code !!!
}
)";

// ============================================================
// Test: CreateDxc
// ============================================================

TEST(DXC, CreateDxc) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    EXPECT_TRUE(dxc->IsValid());
}

// ============================================================
// Test: Basic compilation — vertex, pixel, compute (DXIL)
// ============================================================

TEST(DXC, CompileVertexShader) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_FALSE(result->Refl.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::DXIL);
}

TEST(DXC, CompilePixelShader) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::DXIL);
}

TEST(DXC, CompileComputeShader) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_CS, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::DXIL);
}

// ============================================================
// Test: SPIR-V compilation
// ============================================================

TEST(DXC, CompileToSpirv) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::SPIRV);
}

TEST(DXC, CompileComputeToSpirv) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_CS, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::SPIRV);
}

// ============================================================
// Test: DxcCompileParams overload
// ============================================================

TEST(DXC, CompileWithParams) {
    auto dxc = CreateDxc();
    DxcCompileParams params{};
    params.Code = SIMPLE_PS;
    params.EntryPoint = "PSMain";
    params.Stage = ShaderStage::Pixel;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = true;
    params.IsSpirv = false;
    auto result = dxc->Compile(params);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::DXIL);
}

TEST(DXC, CompileWithParamsSpirv) {
    auto dxc = CreateDxc();
    DxcCompileParams params{};
    params.Code = SIMPLE_PS;
    params.EntryPoint = "PSMain";
    params.Stage = ShaderStage::Pixel;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = true;
    auto result = dxc->Compile(params);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
    EXPECT_EQ(result->Category, ShaderBlobCategory::SPIRV);
}

// ============================================================
// Test: Optimization on/off both produce valid output
// ============================================================

TEST(DXC, CompileOptimized) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
}

TEST(DXC, CompileUnoptimized) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
}

// ============================================================
// Test: Preprocessor defines
// ============================================================

TEST(DXC, CompileWithDefine) {
    auto dxc = CreateDxc();
    std::string_view defines[] = {"USE_RED"};
    auto result = dxc->Compile(SHADER_WITH_DEFINE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, defines);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
}

TEST(DXC, CompileWithDefineViaParams) {
    auto dxc = CreateDxc();
    std::string_view defines[] = {"USE_RED"};
    DxcCompileParams params{};
    params.Code = SHADER_WITH_DEFINE;
    params.EntryPoint = "PSMain";
    params.Stage = ShaderStage::Pixel;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = true;
    params.Defines = defines;
    auto result = dxc->Compile(params);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
}

// ============================================================
// Test: Shader model variants
// ============================================================

TEST(DXC, CompileShaderModel61) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM61, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
}

TEST(DXC, CompileShaderModel66) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM66, true);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->Data.empty());
}

// ============================================================
// Test: Compilation failure — invalid code
// ============================================================

TEST(DXC, CompileInvalidShaderFails) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(INVALID_SHADER, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    EXPECT_FALSE(result.has_value());
}

TEST(DXC, CompileWrongEntryPointFails) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile(SIMPLE_VS, "NonExistentEntry", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    EXPECT_FALSE(result.has_value());
}

TEST(DXC, CompileEmptyCodeFails) {
    auto dxc = CreateDxc();
    auto result = dxc->Compile("", "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// Test: Reflection — vertex shader input/output parameters
// ============================================================

TEST(DXC, ReflectionVertexInputParameters) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    // VS_INPUT has: float3 pos : POSITION, float2 uv : TEXCOORD0
    EXPECT_EQ(desc->InputParameters.size(), 2u);
    bool hasPosition = false;
    bool hasTexcoord = false;
    for (const auto& p : desc->InputParameters) {
        if (p.SemanticName == "POSITION") hasPosition = true;
        if (p.SemanticName == "TEXCOORD") hasTexcoord = true;
    }
    EXPECT_TRUE(hasPosition);
    EXPECT_TRUE(hasTexcoord);

    // PS_INPUT has: SV_POSITION, TEXCOORD0
    EXPECT_EQ(desc->OutputParameters.size(), 2u);
}

TEST(DXC, ReflectionVertexStageFlag) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->Stages, ShaderStages{ShaderStage::Vertex});
}

// ============================================================
// Test: Reflection — pixel shader input/output parameters
// ============================================================

TEST(DXC, ReflectionPixelInputParameters) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(desc.has_value());

    // PS_INPUT has: SV_POSITION, TEXCOORD0
    EXPECT_EQ(desc->InputParameters.size(), 2u);
    // SV_Target output
    EXPECT_EQ(desc->OutputParameters.size(), 1u);
    EXPECT_EQ(desc->Stages, ShaderStages{ShaderStage::Pixel});
}

// ============================================================
// Test: Reflection — compute shader thread group size
// ============================================================

TEST(DXC, ReflectionComputeThreadGroupSize) {
    auto dxc = CreateDxc();
    auto cs = dxc->Compile(SIMPLE_CS, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true);
    ASSERT_TRUE(cs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Compute, cs->Refl, cs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    EXPECT_EQ(desc->GroupSizeX, 64u);
    EXPECT_EQ(desc->GroupSizeY, 1u);
    EXPECT_EQ(desc->GroupSizeZ, 1u);
    EXPECT_EQ(desc->Stages, ShaderStages{ShaderStage::Compute});
}

// ============================================================
// Test: Reflection — bound resources
// ============================================================

TEST(DXC, ReflectionBoundResources) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(desc.has_value());

    // PSMain uses: _Obj(b0), _Camera(b1), _Global(b2), TestCBuffer(b3), _AlbedoTex(t0), _AlbedoSampler(s0)
    // Compiler may optimize out unused resources, but at minimum _Global, TestCBuffer, _AlbedoTex, _AlbedoSampler should be present
    EXPECT_GE(desc->BoundResources.size(), 4u);

    bool hasTexture = false;
    bool hasSampler = false;
    bool hasCBuffer = false;
    for (const auto& res : desc->BoundResources) {
        if (res.Type == HlslShaderInputType::TEXTURE) hasTexture = true;
        if (res.Type == HlslShaderInputType::SAMPLER) hasSampler = true;
        if (res.Type == HlslShaderInputType::CBUFFER) hasCBuffer = true;
    }
    EXPECT_TRUE(hasTexture);
    EXPECT_TRUE(hasSampler);
    EXPECT_TRUE(hasCBuffer);
}

TEST(DXC, ReflectionComputeUAV) {
    auto dxc = CreateDxc();
    auto cs = dxc->Compile(SIMPLE_CS, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true);
    ASSERT_TRUE(cs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Compute, cs->Refl, cs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    ASSERT_GE(desc->BoundResources.size(), 1u);
    bool hasUAV = false;
    for (const auto& res : desc->BoundResources) {
        if (res.Type == HlslShaderInputType::UAV_RWTYPED) {
            hasUAV = true;
            EXPECT_EQ(res.Name, "_Output");
            EXPECT_EQ(res.BindPoint, 0u);
        }
    }
    EXPECT_TRUE(hasUAV);
}

// ============================================================
// Test: Reflection — constant buffer details
// ============================================================

TEST(DXC, ReflectionConstantBufferDetails) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    // VSMain uses _Obj, _Camera, _Global
    EXPECT_GE(desc->ConstantBuffers.size(), 3u);

    auto objCB = desc->FindCBufferByName("_Obj");
    ASSERT_TRUE(objCB.has_value());
    // PreObjectData: 3 float4x4 = 3 * 64 = 192 bytes
    EXPECT_EQ(objCB->get().Size, 192u);

    auto cameraCB = desc->FindCBufferByName("_Camera");
    ASSERT_TRUE(cameraCB.has_value());
    // PreCameraData: 3 float4x4 (192) + float3 padded to 16 = 208 bytes
    EXPECT_EQ(cameraCB->get().Size, 208u);
}

TEST(DXC, ReflectionCBufferIsViewFlag) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(desc.has_value());

    // ConstantBuffer<T> should have IsViewInHlsl = true
    // cbuffer should have IsViewInHlsl = false
    for (const auto& cb : desc->ConstantBuffers) {
        if (cb.Name == "TestCBuffer") {
            EXPECT_FALSE(cb.IsViewInHlsl);
        } else {
            EXPECT_TRUE(cb.IsViewInHlsl);
        }
    }
}

// ============================================================
// Test: Reflection — type information
// ============================================================

TEST(DXC, ReflectionTypeInfo) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    EXPECT_FALSE(desc->Types.empty());
    // Check that we can find matrix types (float4x4)
    bool hasMatrixType = false;
    for (const auto& t : desc->Types) {
        if (t.Class == HlslShaderVariableClass::MATRIX_ROWS || t.Class == HlslShaderVariableClass::MATRIX_COLUMNS) {
            hasMatrixType = true;
            EXPECT_EQ(t.Rows, 4u);
            EXPECT_EQ(t.Columns, 4u);
        }
    }
    EXPECT_TRUE(hasMatrixType);
}

// ============================================================
// Test: Reflection — register spaces
// ============================================================

TEST(DXC, ReflectionRegisterSpaces) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SHADER_MULTI_REGISTER_SPACE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(desc.has_value());

    // Verify resources in different spaces
    bool hasSpace0CB = false;
    bool hasSpace1CB = false;
    bool hasSpace0Tex = false;
    bool hasSpace1Tex = false;
    for (const auto& res : desc->BoundResources) {
        if (res.Type == HlslShaderInputType::CBUFFER && res.Space == 0) hasSpace0CB = true;
        if (res.Type == HlslShaderInputType::CBUFFER && res.Space == 1) hasSpace1CB = true;
        if (res.Type == HlslShaderInputType::TEXTURE && res.Space == 0) hasSpace0Tex = true;
        if (res.Type == HlslShaderInputType::TEXTURE && res.Space == 1) hasSpace1Tex = true;
    }
    EXPECT_TRUE(hasSpace0CB);
    EXPECT_TRUE(hasSpace1CB);
    EXPECT_TRUE(hasSpace0Tex);
    EXPECT_TRUE(hasSpace1Tex);
}

// ============================================================
// Test: MergeHlslShaderDesc
// ============================================================

TEST(DXC, MergeShaderDescs) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto ps = dxc->Compile(SHADER_WITH_RESOURCES, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());

    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(vsDesc.has_value());
    auto psDesc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(psDesc.has_value());

    const HlslShaderDesc* descs[] = {&*vsDesc, &*psDesc};
    auto merged = MergeHlslShaderDesc(descs);
    ASSERT_TRUE(merged.has_value());

    // Merged should have Graphics stage
    EXPECT_EQ(merged->Stages, ShaderStages{ShaderStage::Graphics});

    // Merged should contain all bound resources from both stages
    EXPECT_GE(merged->BoundResources.size(), vsDesc->BoundResources.size());
    EXPECT_GE(merged->BoundResources.size(), psDesc->BoundResources.size());

    // Each merged resource should have correct stage flags
    for (const auto& res : merged->BoundResources) {
        EXPECT_FALSE(res.Stages == ShaderStages{ShaderStage::UNKNOWN});
    }
}

TEST(DXC, MergeSingleDesc) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(vsDesc.has_value());

    const HlslShaderDesc* descs[] = {&*vsDesc};
    auto merged = MergeHlslShaderDesc(descs);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->Stages, ShaderStages{ShaderStage::Vertex});
}

// ============================================================
// Test: DxcReflectionRadrayExt
// ============================================================

TEST(DXC, ReflectionExtCBufferInfo) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());

    // ReflExt should contain CBuffer metadata
    EXPECT_FALSE(vs->ReflExt.CBuffers.empty());
    for (const auto& cb : vs->ReflExt.CBuffers) {
        EXPECT_FALSE(cb.Name.empty());
    }
}

TEST(DXC, ReflectionExtTargetType) {
    auto dxc = CreateDxc();

    // DXIL compilation
    auto dxil = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(dxil.has_value());
    EXPECT_EQ(dxil->ReflExt.TargetType, 0u);  // 0 = DXIL

    // SPIR-V compilation
    auto spirv = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(spirv.has_value());
    EXPECT_EQ(spirv->ReflExt.TargetType, 1u);  // 1 = SPIR-V
}

// ============================================================
// Test: DeserializeDxcReflectionRadrayExt round-trip
// ============================================================

TEST(DXC, DeserializeReflectionExt) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    EXPECT_FALSE(vs->Refl.empty());

    // The Refl blob should be deserializable
    auto ext = DeserializeDxcReflectionRadrayExt(vs->Refl);
    // Note: DeserializeDxcReflectionRadrayExt may or may not succeed on the raw Refl blob
    // depending on the format. The ReflExt is already populated by Compile().
    // This test verifies the function doesn't crash on valid data.
}

TEST(DXC, DeserializeEmptyDataReturnsNullopt) {
    std::span<const byte> empty{};
    auto result = DeserializeDxcReflectionRadrayExt(empty);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// Test: Dxc::Destroy and IsValid
// ============================================================

TEST(DXC, DestroyInvalidatesCompiler) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc->IsValid());
    dxc->Destroy();
    EXPECT_FALSE(dxc->IsValid());
}

// ============================================================
// Test: Reuse compiler for multiple compilations
// ============================================================

TEST(DXC, ReuseCompilerInstance) {
    auto dxc = CreateDxc();

    auto vs = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());

    auto ps = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());

    auto cs = dxc->Compile(SIMPLE_CS, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true);
    ASSERT_TRUE(cs.has_value());

    // All outputs should be independent
    EXPECT_NE(vs->Data.size(), 0u);
    EXPECT_NE(ps->Data.size(), 0u);
    EXPECT_NE(cs->Data.size(), 0u);
}

// ============================================================
// Test: DXIL and SPIR-V produce different bytecode
// ============================================================

TEST(DXC, DxilAndSpirvDifferentOutput) {
    auto dxc = CreateDxc();
    auto dxil = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(dxil.has_value());
    auto spirv = dxc->Compile(SIMPLE_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(spirv.has_value());

    EXPECT_EQ(dxil->Category, ShaderBlobCategory::DXIL);
    EXPECT_EQ(spirv->Category, ShaderBlobCategory::SPIRV);
    EXPECT_NE(dxil->Data, spirv->Data);
}

// ============================================================
// Test: Reflection from SPIR-V compilation
// ============================================================

TEST(DXC, ReflectionFromSpirvCompile) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    // SPIR-V output should still have reflection extension data
    EXPECT_FALSE(vs->ReflExt.CBuffers.empty());
    EXPECT_EQ(vs->ReflExt.TargetType, 1u);
}

// ============================================================
// Test: FindCBufferByName
// ============================================================

TEST(DXC, FindCBufferByNameFound) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_WITH_RESOURCES, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    auto found = desc->FindCBufferByName("_Obj");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->get().Name, "_Obj");
}

TEST(DXC, FindCBufferByNameNotFound) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SIMPLE_VS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto desc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(desc.has_value());

    auto found = desc->FindCBufferByName("NonExistent");
    EXPECT_FALSE(found.has_value());
}
