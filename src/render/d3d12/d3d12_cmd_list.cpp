#include "d3d12_cmd_list.h"

#include "d3d12_buffer.h"

namespace radray::render::d3d12 {

void CmdListD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

void CmdListD3D12::CopyBuffer(Buffer* src_, uint64_t srcOffset, Buffer* dst_, uint64_t dstOffset, uint64_t size) noexcept {
    BufferD3D12* src = static_cast<BufferD3D12*>(src_);
    BufferD3D12* dst = static_cast<BufferD3D12*>(dst_);
    _cmdList->CopyBufferRegion(dst->_buf.Get(), dstOffset, src->_buf.Get(), srcOffset, size);
}

}  // namespace radray::render::d3d12
