#include <gtest/gtest.h>

#include <cstring>

#include <radray/render/common.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/material_instance.h>
#include <radray/runtime/material_parameter_layout.h>
#include <radray/runtime/material_render_proxy.h>

using namespace radray;
using namespace radray::render;

namespace {

// 与 gltf_viewer 的 per-material cbuffer 同形:5 个 float4。
constexpr std::string_view kMaterialShader = R"(
struct MaterialConstants {
    float4 BaseColorFactor;
    float4 EmissiveFactorAlphaCutoff;
    float4 Principled0;
    float4 Principled1;
    float4 Principled2;
};

[[vk::binding(0, 1)]] ConstantBuffer<MaterialConstants> gMaterial : register(b0, space1);

struct VsOutput {
    float4 Position : SV_Position;
};

float4 PSMain(VsOutput input) : SV_Target0 {
    return gMaterial.BaseColorFactor + gMaterial.EmissiveFactorAlphaCutoff +
           gMaterial.Principled0 + gMaterial.Principled1 + gMaterial.Principled2;
}
)";

std::optional<MaterialParameterLayout> BuildLayout(Dxc& dxc) {
    DxcCompileParams params{};
    params.Code = kMaterialShader;
    params.EntryPoint = "PSMain";
    params.Stage = ShaderStage::Pixel;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = false;
    auto outputOpt = dxc.Compile(params);
    if (!outputOpt.has_value()) {
        return std::nullopt;
    }
    auto reflOpt = dxc.GetShaderDescFromOutput(outputOpt->Refl);
    if (!reflOpt.has_value()) {
        return std::nullopt;
    }
    ShaderReflectionDesc reflection = std::move(reflOpt.value());
    return MaterialParameterLayout::CreateFromReflection(reflection, 1, "gMaterial");
}

}  // namespace

// MaterialInstance(脱离 Material)按名写值后,打包字节流的对应 offset 反映写入值。
// 这里直接用 layout 建一个 storage 模板,模拟 instance 的写值路径,验证字节落位。
TEST(MaterialInstanceTest, WritesPackIntoConstantDataAtReflectedOffsets) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto layoutOpt = BuildLayout(*dxc);
    ASSERT_TRUE(layoutOpt.has_value());
    const MaterialParameterLayout& layout = layoutOpt.value();
    ASSERT_TRUE(layout.HasConstantBuffer());

    auto storageOpt = layout.CreateStorageTemplate();
    ASSERT_TRUE(storageOpt.has_value());
    StructuredBufferStorage& storage = storageOpt.value();

    auto root = storage.GetVar(string{layout.GetConstantBufferName()});
    ASSERT_TRUE(root.IsValid());

    const Eigen::Vector4f baseColor{0.25f, 0.5f, 0.75f, 1.0f};
    const Eigen::Vector4f principled0{0.1f, 0.2f, 0.3f, 0.4f};
    {
        auto field = root.GetVar("BaseColorFactor");
        ASSERT_TRUE(field.IsValid());
        field.SetValue(baseColor);
    }
    {
        auto field = root.GetVar("Principled0");
        ASSERT_TRUE(field.IsValid());
        field.SetValue(principled0);
    }

    // 读回字节流,按反射 offset 校验。
    std::span<const byte> data = storage.GetData();
    const auto* baseField = layout.FindField("BaseColorFactor").Get();
    const auto* p0Field = layout.FindField("Principled0").Get();
    ASSERT_NE(baseField, nullptr);
    ASSERT_NE(p0Field, nullptr);
    ASSERT_GE(data.size(), p0Field->Offset + sizeof(principled0));

    Eigen::Vector4f readBase{};
    Eigen::Vector4f readP0{};
    std::memcpy(readBase.data(), data.data() + baseField->Offset, sizeof(readBase));
    std::memcpy(readP0.data(), data.data() + p0Field->Offset, sizeof(readP0));
    EXPECT_TRUE(readBase.isApprox(baseColor));
    EXPECT_TRUE(readP0.isApprox(principled0));
}

// MaterialRenderProxy::Build 在 device 为空时安全失败(不崩溃,IsBuilt()==false)。
TEST(MaterialRenderProxyTest, BuildWithNullDeviceFailsSafely) {
    MaterialRenderProxy proxy{};
    MaterialInstance emptyInstance{};
    EXPECT_FALSE(proxy.Build(nullptr, emptyInstance));
    EXPECT_FALSE(proxy.IsBuilt());
    EXPECT_EQ(proxy.GetDescriptorSet(), nullptr);
}
