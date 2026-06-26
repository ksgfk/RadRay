#pragma once

#include <utility>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/render/material.h>
#include <radray/runtime/render/keyword_set.h>

namespace radray::srp {

class Shader;

/// 具体材质:纯数据 + 懒建 space1 descriptor set。替代旧 Material/MaterialInstance/MaterialRenderProxy。
///
/// 设计(SRP Batcher 根):同 Shader + 同 KeywordSet 的多个 renderer 共享一个变体/PSO,
/// 只各绑各的 space1 set。本类即一份"per-material 数据 + 其 GPU 镜像":
///   - 固定布局的 cbuffer 字节(调用方按 shader 已知布局打包,无需反射)
///   - 命名贴图槽(name → TextureView*,贴图由外部资产拥有)
///   - 一个公共采样器(懒建)
///
/// GetDescriptorSet(rootSig) 按传入 rootSig 的反射名(FindParameterId)逐槽绑定,
/// 命中才绑(depth-only 变体无 space1 → 全部 miss → 返回 nullptr)。结果按 rootSig 指针缓存。
class StandardMaterial final : public Material {
public:
    struct Desc {
        Shader* MaterialShader{nullptr};
        BlendMode Blend{BlendMode::Opaque};
        bool TwoSided{false};
        float Cutoff{0.5f};

        string CBufferName{"gMaterial"};   ///< space1 cbuffer 反射名
        vector<byte> CBufferData{};        ///< 已打包字节(按 shader 已知布局)

        /// 命名贴图槽:反射名 → SRV。TextureView 由外部资产拥有,需在材质存活期内有效。
        vector<std::pair<string, render::TextureView*>> Textures{};
        string SamplerName{"gMaterialSampler"};
        render::DescriptorSetIndex SetIndex{1};
    };

    StandardMaterial(render::Device* device, Desc desc) noexcept;
    ~StandardMaterial() noexcept override;
    StandardMaterial(const StandardMaterial&) = delete;
    StandardMaterial& operator=(const StandardMaterial&) = delete;

    Shader* GetShader() const override { return _desc.MaterialShader; }
    KeywordSet MaterialKeywords() const override;
    BlendMode GetBlendMode() const override { return _desc.Blend; }
    bool IsTwoSided() const override { return _desc.TwoSided; }
    float AlphaCutoff() const override { return _desc.Cutoff; }

    render::DescriptorSet* GetDescriptorSet(render::RootSignature* rootSig) const override;
    render::DescriptorSetIndex MaterialSetIndex() const override { return _desc.SetIndex; }

private:
    /// 为某 rootSig 构建 space1 set(失败或无可绑槽返回 nullptr)。
    render::DescriptorSet* Build(render::RootSignature* rootSig) const;

    render::Device* _device;
    Desc _desc;

    // —— 懒建缓存(per-rootSig)——
    struct CacheEntry {
        render::RootSignature* RootSig{nullptr};
        unique_ptr<render::DescriptorSet> Set;
        unique_ptr<render::Buffer> CBuffer;
        bool Built{false};
        bool Failed{false};
    };
    mutable vector<CacheEntry> _cache;
    mutable unique_ptr<render::Sampler> _sampler;  ///< 全 rootSig 共用
};

}  // namespace radray::srp
