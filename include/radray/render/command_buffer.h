#pragma once

#include <radray/render/common.h>

namespace radray::render {

class CommandBuffer : public RenderBase {
public:
    virtual ~CommandBuffer() noexcept = default;

    virtual void CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept = 0;
};

}  // namespace radray::render
