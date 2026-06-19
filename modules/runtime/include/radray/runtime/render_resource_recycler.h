#pragma once

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

}  // namespace radray
