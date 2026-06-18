#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string_view>

#include <radray/render/common.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>
#include <radray/runtime/material_parameter_layout.h>

using namespace radray;
using namespace radray::render;

namespace {

// per-material set(space1)上一个含 float3 + 标量交错、矩阵、数组的 cbuffer。
// 这正是 HLSL/SPIR-V cbuffer packing 差异最容易暴露的形状:
//   - float3 紧跟标量(占满 16 字节槽)
//   - float2 跨/不跨 16 字节边界
//   - float4x4(矩阵 stride)
//   - float4 数组(数组 stride)
// 同时挂一张贴图 + 采样器,验证资源槽抽取。
constexpr std::string_view kMaterialShader = R"(
struct MaterialParams {
    float4 BaseColorFactor;   // offset 0
    float3 EmissiveFactor;    // offset 16
    float  Metallic;          // offset 28 (fills the float3's 16B slot)
    float2 Tiling;            // offset 32
    float  Roughness;         // offset 40
    float  AlphaCutoff;       // offset 44
    float4x4 UvTransform;     // offset 48
    float4 Extra[2];          // offset 112
};

[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> gMaterial : register(b0, space1);
[[vk::binding(1, 1)]] Texture2D<float4> gBaseColor : register(t0, space1);
[[vk::binding(2, 1)]] SamplerState gBaseColorSampler : register(s0, space1);

struct VsOutput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

float4 PSMain(VsOutput input) : SV_Target0 {
    float4 base = gBaseColor.Sample(gBaseColorSampler, input.UV) * gMaterial.BaseColorFactor;
    base.rgb += gMaterial.EmissiveFactor * gMaterial.Metallic;
    base.rg *= gMaterial.Tiling + gMaterial.Roughness + gMaterial.AlphaCutoff;
    base = mul(gMaterial.UvTransform, base) + gMaterial.Extra[0] + gMaterial.Extra[1];
    return base;
}
)";

constexpr uint32_t kMaterialSetIndex = 1;

std::optional<MaterialParameterLayout> BuildLayout(Dxc& dxc, bool spirv) {
    DxcCompileParams params{};
    params.Code = kMaterialShader;
    params.EntryPoint = "PSMain";
    params.Stage = ShaderStage::Pixel;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = spirv;
    params.EnableUnbounded = false;
    auto outputOpt = dxc.Compile(params);
    if (!outputOpt.has_value()) {
        return std::nullopt;
    }
    auto& output = outputOpt.value();

    ShaderReflectionDesc reflection{};
    if (!spirv) {
        auto reflOpt = dxc.GetShaderDescFromOutput(output.Refl);
        if (!reflOpt.has_value()) {
            return std::nullopt;
        }
        reflection = std::move(reflOpt.value());
    } else {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        auto reflOpt = ReflectSpirv(SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = "PSMain",
            .Stage = ShaderStage::Pixel,
        });
        if (!reflOpt.has_value()) {
            return std::nullopt;
        }
        reflection = std::move(reflOpt.value());
#else
        return std::nullopt;
#endif
    }
    return MaterialParameterLayout::CreateFromReflection(reflection, kMaterialSetIndex, "gMaterial");
}

const MaterialParameterLayout::Field* FindField(
    const MaterialParameterLayout& layout, std::string_view name) {
    auto fields = layout.GetFields();
    auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& f) {
        return f.Name == name;
    });
    return it == fields.end() ? nullptr : &(*it);
}

}  // namespace

// 单后端(D3D12 / DXIL):字段表偏移与手算的 HLSL packing 一致,
// 且 StructuredBufferStorage 的根成员偏移与反射偏移逐字段相等。
TEST(MaterialParameterLayoutTest, HlslFieldOffsetsMatchPacking) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto layoutOpt = BuildLayout(*dxc, /*spirv=*/false);
    ASSERT_TRUE(layoutOpt.has_value());
    const MaterialParameterLayout& layout = layoutOpt.value();

    EXPECT_EQ(layout.GetSetIndex(), kMaterialSetIndex);
    ASSERT_TRUE(layout.HasConstantBuffer());
    EXPECT_EQ(layout.GetConstantBufferName(), "gMaterial");

    struct Expect {
        const char* Name;
        uint32_t Offset;
    };
    const Expect expects[] = {
        {"BaseColorFactor", 0},
        {"EmissiveFactor", 16},
        {"Metallic", 28},
        {"Tiling", 32},
        {"Roughness", 40},
        {"AlphaCutoff", 44},
        {"UvTransform", 48},
        {"Extra", 112},
    };
    for (const auto& e : expects) {
        const auto* f = FindField(layout, e.Name);
        ASSERT_NE(f, nullptr) << "missing field " << e.Name;
        EXPECT_EQ(f->Offset, e.Offset) << "offset mismatch for " << e.Name;
    }

    // 资源槽:一张贴图 + 一个采样器。
    bool sawTexture = false;
    bool sawSampler = false;
    for (const auto& slot : layout.GetResourceSlots()) {
        if (slot.Kind == MaterialParameterLayout::ResourceKind::Texture && slot.Name == "gBaseColor") {
            sawTexture = true;
        }
        if (slot.Kind == MaterialParameterLayout::ResourceKind::Sampler && slot.Name == "gBaseColorSampler") {
            sawSampler = true;
        }
    }
    EXPECT_TRUE(sawTexture);
    EXPECT_TRUE(sawSampler);

    // 存储模板根成员偏移逐字段与反射一致。
    auto storageOpt = layout.CreateStorageTemplate();
    ASSERT_TRUE(storageOpt.has_value());
    StructuredBufferStorage& storage = storageOpt.value();
    auto root = storage.GetVar("gMaterial");
    ASSERT_TRUE(root.IsValid());
    for (const auto& e : expects) {
        auto member = root.GetVar(e.Name);
        ASSERT_TRUE(member.IsValid()) << "storage missing member " << e.Name;
        EXPECT_EQ(member.GetGlobalOffset(), e.Offset) << "storage offset mismatch for " << e.Name;
    }
}

// 跨后端钉死:同一份 HLSL 同编 DXIL + SPIR-V,两边抽出的字段偏移表必须逐字段相等。
// 这把 HLSL/SPIR-V cbuffer packing 差异(float3+标量、矩阵 stride、数组 stride)
// 的脆弱点钉在测试里。
TEST(MaterialParameterLayoutTest, HlslAndSpirvFieldOffsetsAgree) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto hlslOpt = BuildLayout(*dxc, /*spirv=*/false);
    auto spirvOpt = BuildLayout(*dxc, /*spirv=*/true);
    ASSERT_TRUE(hlslOpt.has_value());
    ASSERT_TRUE(spirvOpt.has_value());
    const MaterialParameterLayout& hlsl = hlslOpt.value();
    const MaterialParameterLayout& spirv = spirvOpt.value();

    ASSERT_TRUE(hlsl.HasConstantBuffer());
    ASSERT_TRUE(spirv.HasConstantBuffer());

    // cbuffer 字节大小一致。
    EXPECT_EQ(hlsl.GetConstantBufferSize(), spirv.GetConstantBufferSize());

    // 字段数一致。
    ASSERT_EQ(hlsl.GetFields().size(), spirv.GetFields().size());

    // 逐字段(按名)偏移 + 大小相等。
    for (const auto& hf : hlsl.GetFields()) {
        const auto* sf = FindField(spirv, hf.Name);
        ASSERT_NE(sf, nullptr) << "SPIR-V missing field " << hf.Name;
        EXPECT_EQ(hf.Offset, sf->Offset) << "offset mismatch for " << hf.Name;
        EXPECT_EQ(hf.Size, sf->Size) << "size mismatch for " << hf.Name;
    }

    // 资源槽(按名 + 种类)一致。
    ASSERT_EQ(hlsl.GetResourceSlots().size(), spirv.GetResourceSlots().size());
    for (const auto& hs : hlsl.GetResourceSlots()) {
        bool found = false;
        for (const auto& ss : spirv.GetResourceSlots()) {
            if (ss.Name == hs.Name && ss.Kind == hs.Kind) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "SPIR-V missing resource slot " << hs.Name;
    }
}
