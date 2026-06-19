#pragma once

#include <filesystem>
#include <optional>

#include <radray/types.h>
#include <radray/structured_buffer.h>
#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_parameter_layout.h>

namespace radray {

class GpuSystem;

/// Material 初始化描述。承载一个图形材质所需的 shader 源与渲染状态。
/// 对应 UE5 UMaterial 的最小化等价:一个 pass、一组 VS/PS、固定渲染状态。
/// PSO 真正创建依赖顶点布局与 RT 格式,故不在此处而在 PSOCache 按需组合。
struct MaterialDescriptor {
    std::filesystem::path ShaderPath{};
    string ShaderName{};
    string VsEntry{"VSMain"};
    string PsEntry{"PSMain"};
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    render::DepthStencilState DepthStencil{render::DepthStencilState::Default()};
    std::optional<render::BlendState> Blend{};
    /// per-material 绑定频率所在的 descriptor set 索引(register space)。
    /// 约定:set0=per-view,set1=per-material,push-constant=per-object。
    uint32_t MaterialSetIndex{1};
    /// per-material cbuffer 名字。为空时取该 set 上第一个 cbuffer。
    string MaterialCBufferName{};
};

/// 图形材质资产。对应 UE5 的 UMaterial / FMaterial(最小化)。
///
/// 设计:
/// - 走 AssetManager(与 StaticMesh 一致)，【构造函数一次性初始化】: 构造时
///   编译 shader、从 GpuSystem 取共享 RootSignature，不再设二段式 Initialize。
/// - 【RootSignature 不独占】: RootSignature 由参与的 shader 绑定布局决定，
///   由 GpuSystem 按 layout 缓存共享，Material 仅持非拥有指针。
/// - shader 也是非拥有(来自 GpuSystem 的 shader 缓存)。
/// - 不持有 PSO：PSO 依赖顶点布局 + RT 格式，由 PSOCache 按 (Material, 顶点签名, RT 格式)
///   组合缓存。
class Material : public Asset {
public:
    /// 构造时立即初始化: 编译 VS/PS、取共享 RootSignature、填充渲染状态。
    /// 初始化失败时保持无效状态(IsValid() 返回 false)，不抛异常。
    Material(GpuSystem& gpuSystem, const MaterialDescriptor& desc) noexcept;
    ~Material() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept { return _rootSig != nullptr && _vs != nullptr && _ps != nullptr; }

    render::Shader* GetVS() const noexcept { return _vs; }
    render::Shader* GetPS() const noexcept { return _ps; }
    std::string_view GetVsEntry() const noexcept { return _vsEntry; }
    std::string_view GetPsEntry() const noexcept { return _psEntry; }
    render::RootSignature* GetRootSignature() const noexcept { return _rootSig; }

    const render::PrimitiveState& GetPrimitiveState() const noexcept { return _primitive; }
    const render::DepthStencilState& GetDepthStencilState() const noexcept { return _depthStencil; }
    const std::optional<render::BlendState>& GetBlendState() const noexcept { return _blend; }

    std::optional<render::BindingParameterId> FindParameterId(std::string_view name) const noexcept;

    /// per-material 参数布局(后端无关)。从 PS/VS 反射的 MaterialSetIndex set 抽取。
    /// shader 没有 per-material cbuffer 时为一个空布局(HasConstantBuffer()==false)。
    const MaterialParameterLayout& GetParameterLayout() const noexcept { return _paramLayout; }

    /// per-material set 索引(register space)。
    uint32_t GetMaterialSetIndex() const noexcept { return _materialSetIndex; }

    /// 默认值存储模板。MaterialInstance 克隆它得到自己的参数副本。
    /// 无 per-material cbuffer 时为 std::nullopt。
    const std::optional<StructuredBufferStorage>& GetStorageTemplate() const noexcept { return _storageTemplate; }

private:
    render::Shader* _vs{nullptr};
    render::Shader* _ps{nullptr};
    string _vsEntry{};
    string _psEntry{};
    render::RootSignature* _rootSig{nullptr};  // 非拥有: 由 GpuSystem 按 layout 缓存共享。
    render::PrimitiveState _primitive{render::PrimitiveState::Default()};
    render::DepthStencilState _depthStencil{render::DepthStencilState::Default()};
    std::optional<render::BlendState> _blend{};
    uint32_t _materialSetIndex{1};
    MaterialParameterLayout _paramLayout{};
    std::optional<StructuredBufferStorage> _storageTemplate{};
};

template <>
struct RuntimeTypeTrait<Material> {
    static constexpr RuntimeTypeId value{0x4b2c7d9e, 0x1a3f, 0x4e58, 0x9c, 0x6d, 0x2f, 0x71, 0x8b, 0x40, 0x55, 0xc3};
};

/// Material 的加载工厂。纯 CPU(编译 shader + 取共享 RootSignature),无 GPU 上传阶段。
AssetLoadTask LoadMaterial(GpuSystem& gpuSystem, MaterialDescriptor desc);

}  // namespace radray
