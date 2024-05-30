#pragma once

#include <radray/object.h>
#include <radray/rhi/common.h>

namespace radray::rhi {

class IDevice;

class IBuffer : public Object {
public:
    IBuffer(
        std::shared_ptr<IDevice>&& device,
        BufferType type,
        uint64_t byteSize) noexcept
        : _device(std::move(device)),
          _byteSize(byteSize),
          _type(type) {}
    ~IBuffer() noexcept override = default;

    IDevice* GetDevice() const noexcept { return _device.get(); }
    uint64_t GetByteSize() const noexcept { return _byteSize; }
    BufferType GetBufferType() const noexcept { return _type; }

private:
    std::shared_ptr<IDevice> _device;
    uint64_t _byteSize;
    BufferType _type;
};

}  // namespace radray::rhi
