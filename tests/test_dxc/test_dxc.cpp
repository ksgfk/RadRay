#include <gtest/gtest.h>

#include <radray/render/render_utility.h>
#include <radray/render/dxc.h>

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
    float3 pos : POSITION;
    float3 nor : NORMAL0;
    float2 uv  : TEXCOORD0;
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

ConstantBuffer<PreObjectData> _Obj : register(b0);
ConstantBuffer<PreCameraData> _Camera : register(b1);
ConstantBuffer<GlobalData> _Global : register(b2);
cbuffer TestCBuffer : register(b3)
{
    float4 _SomeValue;
};

Texture2D _AlbedoTex : register(t0);
SamplerState _AlbedoSampler : register(s0);
Texture2D _MetallicRoughnessTex : register(t1);
SamplerState _MetallicRoughnessSampler : register(s1);

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

TEST(DXC, Basic) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_CODE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true);
    ASSERT_TRUE(vs.has_value());
    auto ps = dxc->Compile(SHADER_CODE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());
    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs->Refl, vs->ReflExt);
    ASSERT_TRUE(vsDesc.has_value());
    auto psDesc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(psDesc.has_value());
    const HlslShaderDesc* descs[] = {&*vsDesc, &*psDesc};
    auto mergedDesc = MergeHlslShaderDesc(descs);
    ASSERT_TRUE(mergedDesc.has_value());
}

TEST(DXC, CBufferStorage) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_CODE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true).value();
    auto ps = dxc->Compile(SHADER_CODE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs.Refl, vs.ReflExt).value();
    auto psDesc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt).value();
    const HlslShaderDesc* descs[] = {&vsDesc, &psDesc};
    auto mergedDesc = MergeHlslShaderDesc(descs).value();
    auto storageOpt = CreateCBufferStorage(mergedDesc);
    ASSERT_TRUE(storageOpt.has_value());
    auto& storage = storageOpt.value();

    // Check _Obj (ConstantBuffer<PreObjectData>)
    auto obj = storage.GetVar("_Obj");
    ASSERT_TRUE(obj.IsValid());
    EXPECT_TRUE(obj.GetVar("model").IsValid());
    EXPECT_TRUE(obj.GetVar("mvp").IsValid());
    EXPECT_TRUE(obj.GetVar("modelInv").IsValid());

    // Check _Camera (ConstantBuffer<PreCameraData>)
    auto cam = storage.GetVar("_Camera");
    ASSERT_TRUE(cam.IsValid());
    EXPECT_TRUE(cam.GetVar("view").IsValid());
    EXPECT_TRUE(cam.GetVar("proj").IsValid());
    EXPECT_TRUE(cam.GetVar("viewProj").IsValid());
    EXPECT_TRUE(cam.GetVar("posW").IsValid());
    auto camTemp = cam.GetVar("temp");
    ASSERT_TRUE(camTemp.IsValid());
    EXPECT_TRUE(camTemp.GetVar("lightDirW").IsValid());

    // Check _Global (ConstantBuffer<GlobalData>)
    auto global = storage.GetVar("_Global");
    ASSERT_TRUE(global.IsValid());
    EXPECT_TRUE(global.GetVar("time").IsValid());
    auto dirLight = global.GetVar("dirLight");
    ASSERT_TRUE(dirLight.IsValid());
    EXPECT_TRUE(dirLight.GetVar("lightColor").IsValid());

    // Check TestCBuffer (cbuffer)
    // For `cbuffer TestCBuffer { float4 _SomeValue; }`, the variable is `_SomeValue`.
    auto someValue = storage.GetVar("_SomeValue");
    ASSERT_TRUE(someValue.IsValid());

    // Test writing and reading
    float timeVal = 42.0f;
    global.GetVar("time").SetValue(timeVal);

    auto buffer = storage.GetData();
    float readTime;
    std::memcpy(&readTime, buffer.data() + global.GetVar("time").GetOffset(), sizeof(float));
    EXPECT_EQ(readTime, timeVal);
}

const char* SRC2 = R"(
struct PointLight
{
    float3 posW;
    float radius;
    float3 lightColor;
    float intensity;
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

TEST(DXC, CBufferArrayBasic) {
    auto dxc = CreateDxc();
    auto ps = dxc->Compile(SRC2, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true);
    ASSERT_TRUE(ps.has_value());
    auto psDesc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps->Refl, ps->ReflExt);
    ASSERT_TRUE(psDesc.has_value());
    auto storage = CreateCBufferStorage(psDesc.value());
    ASSERT_TRUE(storage.has_value());

    auto lightsVar = storage->GetVar("_Lights");
    ASSERT_TRUE(lightsVar.IsValid());
    auto plArrayVar = lightsVar.GetVar("pl");
    ASSERT_TRUE(plArrayVar.IsValid());
    const auto& plArrayVarSelf = plArrayVar.GetSelf();
    EXPECT_EQ(plArrayVarSelf.GetArraySize(), 4);
}
