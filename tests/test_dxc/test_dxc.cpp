#include <gtest/gtest.h>

#include <radray/render/dxc.h>
#include <radray/render/utility.h>
#include <radray/render/root_signature_helper.h>
#include <radray/render/shader_cbuffer_helper.h>

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

TEST(DXC, BasicReflection) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_CODE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true).value();
    auto ps = dxc->Compile(SHADER_CODE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true).value();
    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs.Refl).value();
    auto psDesc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps.Refl).value();

    const HlslShaderDesc* descs[] = {&vsDesc, &psDesc};
    auto rootSig = CreateRootSignatureDescriptor(descs);
    ASSERT_TRUE(rootSig.has_value());

    auto storage = CreateCBufferStorage(descs);
    ASSERT_TRUE(storage.has_value());

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    auto id = storage->GetVar("_Obj").GetVar("_Obj").GetVar("model").GetId();
    storage->GetVar(id).SetValue(model);

    auto dst = storage->GetData().subspan(storage->GetVar(id).GetOffset(), storage->GetVar(id).GetType()->GetSizeInBytes());
    ASSERT_TRUE(std::memcmp(dst.data(), model.data(), sizeof(Eigen::Matrix4f)) == 0);
}

TEST(DXC, CBufferStorageTests) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_CODE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true).value();
    auto ps = dxc->Compile(SHADER_CODE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true).value();
    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs.Refl).value();
    auto psDesc = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, ps.Refl).value();

    const HlslShaderDesc* descs[] = {&vsDesc, &psDesc};
    auto storageOpt = CreateCBufferStorage(descs);
    ASSERT_TRUE(storageOpt.has_value());
    auto& storage = *storageOpt;

    // 1. Test Global Variable (Struct)
    {
        auto globalVar = storage.GetVar("_Global").GetVar("_Global");
        ASSERT_TRUE(globalVar.IsValid());

        auto dirLightVar = globalVar.GetVar("dirLight");
        ASSERT_TRUE(dirLightVar.IsValid());

        auto lightDirWVar = dirLightVar.GetVar("lightDirW");
        ASSERT_TRUE(lightDirWVar.IsValid());

        Eigen::Vector4f lightDir(1.0f, 0.5f, 0.2f, 1.0f);
        lightDirWVar.SetValue(lightDir);

        auto offset = lightDirWVar.GetOffset();
        // Verify data in buffer
        auto data = storage.GetData().subspan(offset, sizeof(Eigen::Vector4f));
        ASSERT_EQ(std::memcmp(data.data(), lightDir.data(), sizeof(Eigen::Vector4f)), 0);
    }

    // 2. Test Camera Data (float3)
    {
        auto cameraVar = storage.GetVar("_Camera").GetVar("_Camera");
        ASSERT_TRUE(cameraVar.IsValid());

        auto posWVar = cameraVar.GetVar("posW");
        ASSERT_TRUE(posWVar.IsValid());

        Eigen::Vector3f pos(10.0f, 20.0f, 30.0f);
        posWVar.SetValue(pos);

        auto offset = posWVar.GetOffset();
        auto data = storage.GetData().subspan(offset, sizeof(Eigen::Vector3f));
        ASSERT_EQ(std::memcmp(data.data(), pos.data(), sizeof(Eigen::Vector3f)), 0);
    }

    // 3. Test Scalar (float)
    {
        auto globalVar = storage.GetVar("_Global").GetVar("_Global");
        auto timeVar = globalVar.GetVar("time");
        ASSERT_TRUE(timeVar.IsValid());

        float time = 123.456f;
        timeVar.SetValue(time);

        auto offset = timeVar.GetOffset();
        auto data = storage.GetData().subspan(offset, sizeof(float));
        ASSERT_EQ(*reinterpret_cast<const float*>(data.data()), time);
    }

    // 4. Test Invalid Access
    {
        auto invalidVar = storage.GetVar("_NonExistent");
        ASSERT_FALSE(invalidVar.IsValid());

        auto globalVar = storage.GetVar("_Global").GetVar("_Global");
        auto invalidMember = globalVar.GetVar("nonExistentMember");
        ASSERT_FALSE(invalidMember.IsValid());
    }

    // 5. Test cbuffer namespace
    {
        auto testCBuffer = storage.GetVar("TestCBuffer");
        ASSERT_TRUE(testCBuffer.IsValid());

        auto someValueVar = testCBuffer.GetVar("_SomeValue");
        ASSERT_TRUE(someValueVar.IsValid());

        Eigen::Vector4f val(1.0f, 2.0f, 3.0f, 4.0f);
        someValueVar.SetValue(val);

        auto offset = someValueVar.GetOffset();
        auto data = storage.GetData().subspan(offset, sizeof(Eigen::Vector4f));
        ASSERT_EQ(std::memcmp(data.data(), val.data(), sizeof(Eigen::Vector4f)), 0);
    }
}

TEST(DXC, NamingCollision) {
    const char* COLLISION_SHADER = R"(
struct SameName {
    float4 SameName;
};

ConstantBuffer<SameName> _Root : register(b0);

cbuffer _Inner : register(b1) {
    SameName _Inner;
};

float4 VSMain() : SV_Position {
    return _Root.SameName + _Inner.SameName;
}
)";

    auto dxc = CreateDxc();
    auto vs = dxc->Compile(COLLISION_SHADER, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true).value();
    auto vsDesc = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vs.Refl).value();

    const HlslShaderDesc* descs[] = {&vsDesc};
    auto storageOpt = CreateCBufferStorage(descs);
    ASSERT_TRUE(storageOpt.has_value());
    auto& storage = *storageOpt;

    // 1. Test Struct with member of same name
    {
        auto rootVar = storage.GetVar("_Root");
        ASSERT_TRUE(rootVar.IsValid());

        auto memberVar = rootVar.GetVar("_Root").GetVar("SameName");
        ASSERT_TRUE(memberVar.IsValid());

        // Verify type size (float4 = 16 bytes)
        ASSERT_EQ(memberVar.GetType()->GetSizeInBytes(), 16);
    }

    // 2. Test usage in cbuffer
    {
        auto cbVar = storage.GetVar("_Inner");
        ASSERT_TRUE(cbVar.IsValid());

        auto innerVar = cbVar.GetVar("_Inner");
        ASSERT_TRUE(innerVar.IsValid());

        auto memberVar = innerVar.GetVar("SameName");
        ASSERT_TRUE(memberVar.IsValid());
    }
}
