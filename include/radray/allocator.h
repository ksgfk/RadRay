#pragma once

#include <vector>

#include <radray/types.h>

namespace radray {

template <typename Handle>
class IAllocator {
public:
    virtual ~IAllocator() noexcept = default;

    virtual Handle Allocate(uint64 size) noexcept = 0;
    virtual void Destroy(Handle handle) noexcept = 0;
};

template <typename Handle>
class LinearAllocator {
private:
    struct Buffer {
        Handle handle;
        uint64 capacity;
        uint64 count;
    };

public:
    struct View {
        Handle handle;
        uint64 offset;
    };

    LinearAllocator(IAllocator<Handle>* alloc, uint64 capacity, double incMag = 1.5) noexcept
        : _proxy(alloc),
          _capacity(capacity),
          _initCapacity(capacity),
          _capacityIncMag(incMag) {}

    ~LinearAllocator() noexcept {
        for (auto&& i : _buffers) {
            _proxy->Destroy(i.handle);
        }
        _buffers.clear();
    }

    View Allocate(uint64 size) noexcept {
        for (auto&& i : _buffers) {
            uint64 freeSize = i.capacity - i.count;
            if (freeSize >= size) {
                uint64 offset = i.count;
                i.count += size;
                return {i.handle, offset};
            }
        }
        if (_capacity < size) {
            _capacity = std::max<uint64>(_capacity, static_cast<uint64>(_capacity * _capacityIncMag));
        }
        uint64 allocSize = std::max<uint64>(size, _capacity);
        Handle newHandle = _proxy->Allocate(allocSize);
        _buffers.emplace_back(Buffer{
            .handle = newHandle,
            .capacity = allocSize,
            .count = size});
        return {newHandle, 0};
    }

    void Clear() noexcept {
        switch (_buffers.size()) {
            case 0:
                break;
            case 1: {
                auto&& i = _buffers[0];
                i.count = 0;
                break;
            }
            default: {
                uint64 sumSize = 0u;
                for (auto&& i : _buffers) {
                    sumSize += i.capacity;
                    _proxy->Destroy(i.handle);
                }
                _buffers.clear();
                _buffers.emplace_back(Buffer{
                    .handle = _proxy->Allocate(sumSize),
                    .capacity = sumSize,
                    .count = 0});
                break;
            }
        }
    }

    void TrimExcess() noexcept {
        _capacity = _initCapacity;
        for (auto&& i : _buffers) {
            _proxy->Destroy(i.handle);
        }
        _buffers.clear();
    }

private:
    std::vector<Buffer> _buffers;
    IAllocator<Handle>* _proxy;
    uint64 _capacity;
    uint64 _initCapacity;
    double _capacityIncMag;
};

}  // namespace radray
