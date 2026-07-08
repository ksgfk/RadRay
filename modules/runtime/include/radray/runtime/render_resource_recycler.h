#pragma once

#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray::render {
class RenderBase;
}

namespace radray {

class IRenderResourceRecycler {
public:
    virtual ~IRenderResourceRecycler() noexcept = default;

    /// 逻辑所有者已死，把 GPU object 交出延迟释放。线程安全。
    virtual void RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept = 0;
};

template <>
struct RuntimeTypeTrait<IRenderResourceRecycler> {
    static constexpr RuntimeTypeId value{0x3b1f9c4a, 0x7d62, 0x4e08, 0xa9, 0x14, 0x6c, 0x5e, 0x2b, 0x8f, 0xd1, 0x37};
    using Bases = std::tuple<>;
};

}  // namespace radray
