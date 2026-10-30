#pragma once

#include <radray/runtime/material_instance.h>

namespace radray {

// Scene 拥有的渲染快照，不保留用户侧 Material / MaterialInstance 引用。
// RHI 资源和 MaterialResource 仍为借用；调用方须串行组织 Update/Get。
class MaterialRenderProxy {
public:
    // 显式同步：后续实例编辑只有再次 Update 才能进入本快照。
    void Update(const Material& material);
    void Update(const MaterialInstance& instance);
    Nullable<const MaterialResource*> GetResource() const noexcept;
    // 返回的引用不能跨 Update、Proxy 搬移或所属 SparseSet 记录搬移保存。
    Nullable<const MaterialParameterValue*> GetParameter(size_t index) const noexcept;

private:
    Nullable<const MaterialResource*> _resource{nullptr};
    vector<MaterialParameterValue> _parameters;
};

}  // namespace radray
