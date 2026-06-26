#pragma once

#include <radray/render/common.h>
#include <radray/runtime/render/keyword_set.h>
#include <radray/runtime/render/sorting.h>

namespace radray::srp {

class Shader;

/// 材质混合模式。决定 renderer 落不透明 / masked / 透明队列。
enum class BlendMode : uint32_t {
    Opaque,
    Masked,
    Transparent,
};

/// 纯数据材质(对应 Unity .mat)。【不拥有任何 shader 代码或编译产物】。
/// 持有:对 Shader 的引用、属性值(space1 数据,经 GetDescriptorSet 落 GPU)、
///       keyword 选择、材质语义(blend/twoSided/cutoff)。
///
/// 抽象接口:runtime 只定义"材质要能回答什么";具体值/贴图/descriptor set 的构建
/// 由 game 层(如 gltf 材质)实现。设计依据:srp_runtime_design.md §5。
class Material {
public:
    virtual ~Material() = default;

    /// 引用的 Shader(多 LightMode 代码容器)。不拥有。
    virtual Shader* GetShader() const = 0;

    /// shader_feature 轴:material 驱动的 keyword(_NORMALMAP / _ALPHATEST 等)。
    virtual KeywordSet MaterialKeywords() const { return {}; }

    /// —— 材质语义(第一层意图谓词读这些;不含任何 shader 代码)——
    virtual BlendMode GetBlendMode() const { return BlendMode::Opaque; }
    virtual bool IsTwoSided() const { return false; }
    virtual float AlphaCutoff() const { return 0.5f; }

    /// 该材质落入的渲染队列。默认按 BlendMode 推导;可覆写细调。
    virtual RenderQueue Queue() const {
        switch (GetBlendMode()) {
            case BlendMode::Opaque: return RenderQueue::Opaque;
            case BlendMode::Masked: return RenderQueue::AlphaTest;
            case BlendMode::Transparent: return RenderQueue::Transparent;
        }
        return RenderQueue::Opaque;
    }

    /// per-material descriptor set(space1):用值填 CBUFFER + 贴图,懒建 + 缓存。
    /// 布局由传入的 rootSig(来自 shader 变体反射)决定。返回 nullptr 表示无 per-material 绑定。
    /// 对应 SRP Batcher:同 variant 的 draw 之间只换这个 set,不重建管线。
    virtual render::DescriptorSet* GetDescriptorSet(render::RootSignature* rootSig) const = 0;

    /// per-material set 索引(register space)。仅在 GetDescriptorSet 返回非空时有意义。
    virtual render::DescriptorSetIndex MaterialSetIndex() const { return render::DescriptorSetIndex{1}; }
};

}  // namespace radray::srp
