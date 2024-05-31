#pragma once

#include <radray/object.h>
#include <radray/rhi/resource.h>

namespace radray::rhi {

class IDevice;

class IBuffer : public IResource {
public:
    IBuffer(
        std::shared_ptr<IDevice> device,
        BufferType type,
        uint64_t byteSize) noexcept
        : IResource(std::move(device)),
          _byteSize(byteSize),
          _type(type) {}
    ~IBuffer() noexcept override = default;

    uint64_t GetByteSize() const noexcept { return _byteSize; }
    BufferType GetBufferType() const noexcept { return _type; }

private:
    uint64_t _byteSize;
    BufferType _type;
};

}  // namespace radray::rhi
