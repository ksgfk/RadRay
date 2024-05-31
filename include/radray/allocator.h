#pragma once

#include <vector>
#include <optional>

#include <radray/types.h>

namespace radray {

class IAllocator {
public:
    virtual ~IAllocator() noexcept = default;

    virtual std::optional<uint64_t> Allocate(uint64_t size, uint64_t align) noexcept = 0;
    virtual void Destroy(uint64_t handle) noexcept = 0;
};

class LinearAllocator {
private:
    struct Buffer {
        uint64_t handle;
        uint64_t capacity;
        uint64_t count;
    };

public:
    struct View {
        uint64_t handle;
        uint64_t offset;
    };

    LinearAllocator(IAllocator* alloc, uint64_t capacity, double incMag = 1.5) noexcept;
    ~LinearAllocator() noexcept;

    View Allocate(uint64_t targetSize, uint64_t align) noexcept;
    View Allocate(uint64_t size) noexcept;
    void Clear() noexcept;
    void Reset() noexcept;

private:
    std::vector<Buffer> _buffers;
    IAllocator* _proxy;
    uint64_t _capacity;
    uint64_t _initCapacity;
    double _capacityIncMag;
};

class BuddyAllocator : public IAllocator {
private:
    enum class NodeState : uint8_t {
        Unused = 0,
        Used = 1,
        Split = 2,
        Full = 3
    };

public:
    BuddyAllocator(uint64_t capacity) noexcept;
    ~BuddyAllocator() noexcept override = default;

    std::optional<uint64_t> Allocate(uint64_t size, uint64_t align = 1) noexcept override;
    void Destroy(uint64_t offset) noexcept override;

private:
    std::vector<NodeState> _tree;
    uint64_t _capacity;
};

}  // namespace radray
