#include <gtest/gtest.h>

#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/render_utility.h>

const char* SHADER_CODE = R"(
struct DirLight
{
    float4 lightDirW;
    float4 lightColor;
};

struct GlobalData
{
    DirLight dirLight;
    float time;
};

struct PreCameraData
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    DirLight temp;
    float3 posW;
};

struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 nor : NORMAL0;
    [[vk::location(2)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv  : TEXCOORD0;
};

struct PreObjectData
{
    float4x4 model;
    float4x4 mvp;
    float4x4 modelInv;
};

[[vk::push_constant]] ConstantBuffer<PreObjectData> _Obj : register(b0);
[[vk::binding(0)]] ConstantBuffer<PreCameraData> _Camera : register(b1);
[[vk::binding(1)]] ConstantBuffer<GlobalData> _Global : register(b2);
[[vk::binding(2)]] cbuffer TestCBuffer : register(b3)
{
    float4 _SomeValue;
};

[[vk::binding(3)]] Texture2D _AlbedoTex : register(t0);
[[vk::binding(5)]] SamplerState _AlbedoSampler : register(s0);
[[vk::binding(4)]] Texture2D _MetallicRoughnessTex : register(t1);
[[vk::binding(6)]] SamplerState _MetallicRoughnessSampler : register(s1);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(float4(vsIn.pos + _Camera.posW, 1.0f), _Obj.mvp);
    psIn.posW = mul(float4(vsIn.pos + _Global.time, 1.0f), _Obj.model).xyz;
    psIn.norW = normalize(mul(float4(vsIn.nor, 0.0f), _Obj.modelInv).xyz);
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    float3 albedo = _AlbedoTex.Sample(_AlbedoSampler, psIn.uv).xyz;
    float3 mr = _MetallicRoughnessTex.Sample(_MetallicRoughnessSampler, psIn.uv).xyz;
    float3 t = albedo * mr * ((float3)1.0 / _Global.dirLight.lightDirW.xyz) + ((float3)1.0 / + _Camera.posW);
    return float4(t, 1.0f) + _SomeValue;
}
)";

using namespace radray;
using namespace radray::render;

TEST(SPVC, BasicReflection) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_CODE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true).value();
    auto ps = dxc->Compile(SHADER_CODE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();

    SpirvBytecodeView bytecodes[] = {
        {.Data = vs.Data, .EntryPointName = "VSMain", .Stage = ShaderStage::Vertex},
        {.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* extInfos[] = {
        &vs.ReflExt,
        &ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, extInfos);
    ASSERT_TRUE(desc.has_value());

    auto storageOpt = CreateCBufferStorage(*desc);
    ASSERT_TRUE(storageOpt.has_value());
}

const char* SRC2 = R"(
struct PointLight
{
    float3 posW;
    float radius;
    float3 lightColor;
    float intensity;
    float2 _test;
};

struct DirectionalLight
{
    float3 lightDirW;
    float _pad1;
    float3 lightColor;
    float _pad2;
};

struct Lights
{
    DirectionalLight dl;
    PointLight pl[4];
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv : TEXCOORD0;
};

ConstantBuffer<Lights> _Lights : register(b0);

float4 PSMain(PS_INPUT psIn) : SV_Target
{
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

TEST(SPVC, CBufferArrayBasic) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SRC2, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* extInfos[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, extInfos);
    ASSERT_TRUE(desc.has_value());
    auto storageOpt = CreateCBufferStorage(*desc);
    ASSERT_TRUE(storageOpt.has_value());

    auto lightsVar = storageOpt->GetVar("_Lights");
    ASSERT_TRUE(lightsVar.IsValid());
    auto plArrayVar = lightsVar.GetVar("pl");
    ASSERT_TRUE(plArrayVar.IsValid());
    const auto& plArrayVarSelf = plArrayVar.GetSelf();
    EXPECT_EQ(plArrayVarSelf.GetArraySize(), 4);
}

const char* SRC3 = R"(
struct PointLight
{
    float3 posW;
    float radius;
    float3 lightColor;
    float intensity;
    float2 _test;
};

struct DirectionalLight
{
    float3 lightDirW;
    float _pad1;
    float3 lightColor;
    float _pad2;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv : TEXCOORD0;
};

ConstantBuffer<DirectionalLight> _DirectionalLight : register(b0);
ConstantBuffer<PointLight> _PointLights[4] : register(b1);

float4 PSMain(PS_INPUT psIn) : SV_Target
{
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
TEST(SPVC, CBufferArrayBasic3) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SRC3, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* extInfos[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, extInfos);
    ASSERT_TRUE(desc.has_value());
    auto storage = CreateCBufferStorage(*desc);

    auto plArrayVar = storage->GetVar("_PointLights");
    ASSERT_TRUE(plArrayVar.IsValid());
    const auto& plArrayVarSelf = plArrayVar.GetSelf();
    EXPECT_EQ(plArrayVarSelf.GetArraySize(), 4);

    auto plArrayVarElement = plArrayVar.GetArrayElement(2);
    ASSERT_TRUE(plArrayVarElement.IsValid());
    // sizeof(DirectionalLight) == 32
    // sizeof(PointLight) == 48
    // _PointLights[2] offset == 32 + 48 * 2 == 128
    EXPECT_EQ(plArrayVarElement.GetGlobalOffset(), 128);
    // all 32 + 48 * 4 = 224 bytes
    EXPECT_EQ(storage->GetData().size(), 224);
}

const char* SRC4 = R"(
struct PointLight
{
    float3 posW;
    float radius;
    float3 lightColor;
    float intensity;
    float2 _test;
};

struct DirectionalLight
{
    float3 lightDirW;
    float _pad1;
    float3 lightColor;
    float _pad2;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv : TEXCOORD0;
};

ConstantBuffer<DirectionalLight> _DirectionalLight : register(b0);
cbuffer PointLightsCB : register(b3)
{
    PointLight _PointLights[4];
};

float4 PSMain(PS_INPUT psIn) : SV_Target
{
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
TEST(SPVC, CBufferArrayBasic4) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SRC4, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();
    SpirvBytecodeView bytecodes[] = {{.Data = ps.Data, .EntryPointName = "PSMain", .Stage = ShaderStage::Pixel}};
    const DxcReflectionRadrayExt* extInfos[] = {&ps.ReflExt};
    auto desc = ReflectSpirv(bytecodes, extInfos);
    ASSERT_TRUE(desc.has_value());
    auto storage = CreateCBufferStorage(*desc);

    auto plArrayVar = storage->GetVar("_PointLights");
    ASSERT_TRUE(plArrayVar.IsValid());
    const auto& plArrayVarSelf = plArrayVar.GetSelf();
    EXPECT_EQ(plArrayVarSelf.GetArraySize(), 4);

    auto plArrayVarElement = plArrayVar.GetArrayElement(2);
    ASSERT_TRUE(plArrayVarElement.IsValid());
    // sizeof(DirectionalLight) == 32
    // sizeof(PointLight) == 48
    // _PointLights[2] offset == 32 + 48 * 2 == 128
    EXPECT_EQ(plArrayVarElement.GetGlobalOffset(), 128);
    // all 32 + 48 * 4 = 224 bytes
    EXPECT_EQ(storage->GetData().size(), 224);
}
