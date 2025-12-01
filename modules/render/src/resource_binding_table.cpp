#include <radray/render/ext/resource_binding_table.h>

#include <radray/errors.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

#ifdef RADRAY_ENABLE_D3D12
class ResourceBindingTableD3D12 final : public ResourceBindingTable {
public:
    ResourceBindingTableD3D12() noexcept = default;
    ~ResourceBindingTableD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return true; }

    void Destroy() noexcept override {}
};
#endif

Nullable<unique_ptr<ResourceBindingTable>> CreateResourceBindingTable(
    Device* device,
    RootSignature* rs_,
    const BuildResourceBindingTableExtraData& extraData) noexcept {
    if (device->GetBackend() == RenderBackend::D3D12) {
#ifdef RADRAY_ENABLE_D3D12
        // std::span<const StagedHlslShaderDesc> hlslDescs{};
        // if (std::holds_alternative<HlslResourceBindingTableExtraData>(extraData)) {
        //     hlslDescs = std::get<HlslResourceBindingTableExtraData>(extraData).StagedDescs;
        // }

        // vector<HlslRSCombinedBinding> bindings;
        // for (const auto& staged : hlslDescs) {
        //     if (staged.Desc == nullptr) {
        //         RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidArgument, "shader desc");
        //         continue;
        //     }
        //     if (staged.Stage == ShaderStage::UNKNOWN) {
        //         RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidArgument, "shader stage");
        //         continue;
        //     }
        //     ShaderStages stageMask = staged.Stage;
        //     for (const auto& bind : staged.Desc->BoundResources) {
        //         if (!_AddOrMergeHlslBinding(bindings, *staged.Desc, bind, stageMask)) {
        //             return nullptr;
        //         }
        //     }
        // }

        // auto rs = d3d12::CastD3D12Object(rs_);
        // const auto& desc = rs->_desc;
        // if (desc.GetRootConstantCount() > 0) {
        //     const auto& rootConstant = desc.GetRootConstant();
        // }
        // UINT rootDescCount = desc.GetRootDescriptorCount();
        // if (rootDescCount > 0) {
        //     for (UINT i = 0; i < rootDescCount; ++i) {
        //         const auto& rootDesc = desc.GetRootDescriptor(i);
        //         auto it = std::find_if(bindings.begin(), bindings.end(), [&](const HlslRSCombinedBinding& b) {
        //             return b.Space == rootDesc.data.RegisterSpace && b.Slot == rootDesc.data.ShaderRegister;
        //         });
        //         if (it == bindings.end()) {
        //             RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidArgument, "missing binding data for root descriptor");
        //             return nullptr;
        //         } else {
        //         }
        //     }
        // }
        // UINT descTableCount = desc.GetDescriptorTableCount();
        // for (UINT i = 0; i < descTableCount; i++) {
        // }
#else
        RADRAY_ERR_LOG("{} {}", Errors::D3D12, "disabled");
        return nullptr;
#endif
    }
    return nullptr;
}

}  // namespace radray::render
