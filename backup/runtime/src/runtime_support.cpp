#include <algorithm>

#include <radray/runtime/renderer_runtime.h>

namespace radray::runtime {

void FrameLinearAllocator::Reset() noexcept {
    _storage.clear();
    _cursor = 0;
}

void* FrameLinearAllocator::Allocate(size_t size, size_t alignment) noexcept {
    const size_t alignedCursor = Align(_cursor, std::max<size_t>(size_t{1}, alignment));
    const size_t end = alignedCursor + size;
    if (_storage.size() < end) {
        _storage.resize(end);
    }
    _cursor = end;
    return _storage.data() + alignedCursor;
}

DescriptorArena::DescriptorArena(render::Device* device) noexcept
    : _device(device) {}

void DescriptorArena::Reset(render::Device* device) noexcept {
    if (device != nullptr) {
        _device = device;
    }
    _bufferViews.clear();
    _descriptorSets.clear();
}

Nullable<render::BufferView*> DescriptorArena::CreateBufferView(const render::BufferViewDescriptor& desc) noexcept {
    if (_device == nullptr) {
        return nullptr;
    }
    auto viewOpt = _device->CreateBufferView(desc);
    if (!viewOpt.HasValue()) {
        return nullptr;
    }
    _bufferViews.push_back(viewOpt.Release());
    return _bufferViews.back().get();
}

Nullable<render::DescriptorSet*> DescriptorArena::CreateDescriptorSet(
    render::RootSignature* rootSig,
    render::DescriptorSetIndex set) noexcept {
    if (_device == nullptr || rootSig == nullptr) {
        return nullptr;
    }
    auto setOpt = _device->CreateDescriptorSet(rootSig, set);
    if (!setOpt.HasValue()) {
        return nullptr;
    }
    _descriptorSets.push_back(setOpt.Release());
    return _descriptorSets.back().get();
}

}  // namespace radray::runtime
