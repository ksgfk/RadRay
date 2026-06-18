#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/runtime/material_instance.h>

namespace radray {

/// 材质渲染代理。对应 UE5 FMaterialRenderProxy 的最小化等价物。独立于 MaterialInstance,
/// 与现有 component→SceneProxy 的镜像关系一致:CPU 侧参数由 MaterialInstance 持有,
/// GPU 侧资源(常量缓冲 + descriptor set)由本类持有。
///
/// 设计(阶段 2,仅静态材质):
/// - Build() 一次性:建一个 Upload 堆常量缓冲,把 MaterialInstance 的打包字节流 memcpy 进去,
///   建 per-material descriptor set,WriteResource 把 cbuffer 绑上去。
/// - 无 per-flight 多缓冲(静态材质不每帧改值);留 Rebuild 接缝供后续动态化。
/// - 仅需 Device*:Upload 堆缓冲走 Map 写入,无需 command buffer / copy。
class MaterialRenderProxy {
public:
    MaterialRenderProxy() = default;
    MaterialRenderProxy(const MaterialRenderProxy&) = delete;
    MaterialRenderProxy& operator=(const MaterialRenderProxy&) = delete;
    MaterialRenderProxy(MaterialRenderProxy&&) noexcept = default;
    MaterialRenderProxy& operator=(MaterialRenderProxy&&) noexcept = default;
    ~MaterialRenderProxy() noexcept = default;

    /// 一次性构建 GPU 资源。instance 必须有效且其 Material 持有效 RootSignature。
    /// 失败时保持 IsBuilt()==false。重复调用会重建。
    bool Build(render::Device* device, const MaterialInstance& instance) noexcept;

    bool IsBuilt() const noexcept { return _descriptorSet != nullptr; }

    /// per-material descriptor set(已写入常量缓冲)。未构建时为 nullptr。
    render::DescriptorSet* GetDescriptorSet() const noexcept { return _descriptorSet.get(); }

    /// per-material set 索引(register space)。
    render::DescriptorSetIndex GetSetIndex() const noexcept { return render::DescriptorSetIndex{_setIndex}; }

private:
    uint32_t _setIndex{1};
    unique_ptr<render::Buffer> _constantBuffer{};
    unique_ptr<render::Sampler> _sampler{};
    unique_ptr<render::DescriptorSet> _descriptorSet{};
};

}  // namespace radray
