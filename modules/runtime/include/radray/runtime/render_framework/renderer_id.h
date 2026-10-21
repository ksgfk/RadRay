#pragma once

#include <radray/guid.h>
#include <radray/hash.h>

namespace radray {
template <class Tag>
struct RendererHandle {
    Guid Owner;
    uint32_t Index{UINT32_MAX}, Generation{0};
    bool IsValid() const noexcept { return !Owner.IsEmpty() && Index != UINT32_MAX && Generation != 0; }
    friend bool operator==(const RendererHandle&, const RendererHandle&) = default;
};
struct RendererTemplateTag;
struct RendererMaterialTag;
struct RendererResourceTag;
struct RendererDescriptorTag;
struct RendererDataTag;
struct RendererFrameTag;
using TemplateId = RendererHandle<RendererTemplateTag>;
using MaterialId = RendererHandle<RendererMaterialTag>;
using ResourceId = RendererHandle<RendererResourceTag>;
using DescriptorBlockId = RendererHandle<RendererDescriptorTag>;
using RendererDataId = RendererHandle<RendererDataTag>;
using RendererFrameId = RendererHandle<RendererFrameTag>;

template <class Tag>
struct RendererHandleHash {
    size_t operator()(const RendererHandle<Tag>& id) const noexcept {
        HashCode hash;
        hash.Add(std::hash<Guid>{}(id.Owner));
        hash.Add(id.Index);
        hash.Add(id.Generation);
        return hash.ToHashCode();
    }
};
}  // namespace radray
