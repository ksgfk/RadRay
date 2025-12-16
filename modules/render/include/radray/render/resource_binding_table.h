#pragma once

#include <radray/render/common.h>
#include <radray/render/dxc.h>

namespace radray::render {

class HlslResourceBindingTableExtraData {
public:
    std::span<const HlslShaderDesc> StagedDescs{};
};

using BuildResourceBindingTableExtraData = std::variant<HlslResourceBindingTableExtraData>;

class ResourceBindingTable : public RenderBase {
public:
    ~ResourceBindingTable() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTags{RenderObjectTag::UNKNOWN}; }
};

Nullable<unique_ptr<ResourceBindingTable>> CreateResourceBindingTable(Device* device, RootSignature* rs, const BuildResourceBindingTableExtraData& extraData) noexcept;

}  // namespace radray::render
