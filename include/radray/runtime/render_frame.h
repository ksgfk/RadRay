#pragma once

#include <radray/render/device.h>
#include <radray/render/fence.h>

namespace radray::runtime {

class ConstantBufferPool {
public:
    struct Ref {
        render::Buffer* Buffer;
        uint64_t Offset;
    };

    ConstantBufferPool(render::Device* device) noexcept;

    Ref Allocate(uint64_t size) noexcept;
    void Clear() noexcept;

private:
    class Block {
    public:
        shared_ptr<render::Buffer> buf;
        uint64_t capacity;
        uint64_t size;
    };

    render::Device* _device;
    vector<Block> _blocks;
    uint64_t _initSize;
};

class RenderFrame {
public:
private:
    shared_ptr<render::Fence> _fence;
    uint64_t _fenceValue;
};

}  // namespace radray::runtime
