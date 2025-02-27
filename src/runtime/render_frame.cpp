#include <radray/runtime/render_frame.h>

namespace radray::runtime {

ConstantBufferPool::ConstantBufferPool(render::Device* device) noexcept
    : _device(device) {
}

ConstantBufferPool::Ref ConstantBufferPool::Allocate(uint64_t size) noexcept {
    for (auto&& i : _blocks) {
        uint64_t freeSize = i.capacity - i.size;
        if (freeSize >= size) {
            uint64_t offset = i.size;
            i.size += size;
            return {i.buf.get(), offset};
        }
    }
    uint64_t allocSize = std::max<uint64_t>(size, _initSize);
    shared_ptr<render::Buffer> buf = _device->CreateBuffer(
                                                allocSize,
                                                render::ResourceType::CBuffer,
                                                render::ResourceUsage::Upload,
                                                ToFlag(render::ResourceState::GenericRead),
                                                ToFlag(render::ResourceMemoryTip::None))
                                         .Unwrap();
    _blocks.emplace_back(ConstantBufferPool::Block{
        std::move(buf),
        allocSize,
        size});
    return {_blocks.back().buf.get(), 0};
}

void ConstantBufferPool::Clear() noexcept {
    if (_blocks.size() == 1) {
        _blocks[0].size = 0;
    } else {
        uint64_t sumSize = 0u;
        for (auto&& i : _blocks) {
            sumSize += i.capacity;
        }
        _blocks.clear();
        shared_ptr<render::Buffer> buf = _device->CreateBuffer(
                                                    sumSize,
                                                    render::ResourceType::CBuffer,
                                                    render::ResourceUsage::Upload,
                                                    ToFlag(render::ResourceState::GenericRead),
                                                    ToFlag(render::ResourceMemoryTip::None))
                                             .Unwrap();
        _blocks.emplace_back(ConstantBufferPool::Block{
            std::move(buf),
            sumSize,
            0});
    }
}

}  // namespace radray::runtime
