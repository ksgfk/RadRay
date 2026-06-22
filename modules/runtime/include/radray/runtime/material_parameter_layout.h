#pragma once

#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/structured_buffer.h>
#include <radray/render/common.h>

namespace radray {

/// 材质参数布局。后端无关:从 shader 反射(HLSL DXIL 或 SPIR-V)抽取出指定
/// descriptor set 上那一个常量缓冲(cbuffer)的字段表 + 资源槽(贴图/采样器)表。
///
/// 这是吸收两后端反射结构差异的【唯一处】:
/// - HLSL(D3D12)走 register space 约定:cbuffer/texture/sampler 的 `space` == set 索引。
/// - SPIR-V(Vulkan)走 descriptor set:ResourceBinding.Set == set 索引。
///
/// 字段表喂给 StructuredBufferStorage::Builder 生成一份"默认值"存储模板,
/// MaterialInstance 克隆它再按名写值,GetData() 即可直接上传到常量缓冲。
class MaterialParameterLayout {
public:
    /// cbuffer 内的一个标量/向量/矩阵字段。Offset/Size 直接取自反射(字节)。
    struct Field {
        string Name;
        uint32_t Offset{0};
        uint32_t Size{0};

        friend auto operator<=>(const Field&, const Field&) = default;
    };

    enum class ResourceKind {
        Texture,
        Sampler,
    };

    /// cbuffer 之外的资源槽(贴图 / 采样器)。Binding 为 set 内的 register/binding 序号。
    struct ResourceSlot {
        string Name;
        ResourceKind Kind{ResourceKind::Texture};
        uint32_t Binding{0};

        friend auto operator<=>(const ResourceSlot&, const ResourceSlot&) = default;
    };

    MaterialParameterLayout() = default;

    /// 从 shader 反射抽取指定 set 的参数布局。
    /// cbufferName 非空时仅匹配该名字的 cbuffer;为空时取该 set 上第一个 cbuffer。
    /// 反射里该 set 没有任何 cbuffer/资源时返回一个空布局(IsValid() 仍为 true)。
    /// 反射 variant 形态不被支持(理论上不会发生)时返回 nullopt。
    static std::optional<MaterialParameterLayout> CreateFromReflection(
        const render::ShaderReflectionDesc& reflection,
        uint32_t setIndex,
        std::string_view cbufferName = {}) noexcept;

    uint32_t GetSetIndex() const noexcept { return _setIndex; }

    bool HasConstantBuffer() const noexcept { return _hasConstantBuffer; }
    std::string_view GetConstantBufferName() const noexcept { return _cbufferName; }
    uint32_t GetConstantBufferSize() const noexcept { return _cbufferSize; }

    std::span<const Field> GetFields() const noexcept { return _fields; }
    std::span<const ResourceSlot> GetResourceSlots() const noexcept { return _resourceSlots; }

    Nullable<const Field*> FindField(std::string_view name) const noexcept;

    /// 反射派生的布局签名:同一材质不同 shader 变体的 per-material set 布局完全相同
    /// (同 shader 家族,同 space1 绑定),故签名相等即可跨变体/跨 RootSignature 复用
    /// 已建好的 descriptor set。签名由反射(set 索引 + cbuffer 名/大小 + 资源槽名/种类,
    /// 按名/种类排序)决定,不含原始 binding 号以保持跨后端稳定,与具体 RootSignature
    /// 实例无关 —— 这正是缺陷3(per-RootSig proxy 复制)的解耦点。
    string GetLayoutSignature() const noexcept;

    /// 依据字段表建一份 StructuredBufferStorage 模板:单个根结构(cbuffer),
    /// 字段按反射 offset 作为成员落位。值全为 0(默认)。无 cbuffer 时返回 nullopt。
    std::optional<StructuredBufferStorage> CreateStorageTemplate() const noexcept;

private:
    uint32_t _setIndex{0};
    bool _hasConstantBuffer{false};
    string _cbufferName{};
    uint32_t _cbufferSize{0};
    vector<Field> _fields{};
    vector<ResourceSlot> _resourceSlots{};
};

}  // namespace radray
