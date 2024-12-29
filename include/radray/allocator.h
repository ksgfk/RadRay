#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/utility.h>

namespace radray {

class LinearAllocator {
protected:
    struct Buffer {
        void* handle;
        uint64_t capacity;
        uint64_t count;
    };

public:
    struct View {
        void* handle;
        uint64_t offset;
    };

    LinearAllocator(uint64_t capacity, double incMag = 1.5) noexcept;
    virtual ~LinearAllocator() noexcept;

    View Allocate(uint64_t size) noexcept;
    void Clear() noexcept;
    void Reset() noexcept;

protected:
    virtual Nullable<void> DoAllocate(uint64_t size) = 0;
    virtual uint64_t DoDestroy(void* handle) = 0;
    virtual radray::vector<Buffer>& GetBuffer() = 0;

private:
    uint64_t _capacity;
    uint64_t _initCapacity;
    double _capacityIncMag;
};

class BuddyAllocator {
private:
    enum class NodeState : uint8_t {
        Unused = 0,
        Used = 1,
        Split = 2,
        Full = 3
    };

public:
    BuddyAllocator(uint64_t capacity) noexcept;
    ~BuddyAllocator() noexcept = default;

    std::optional<uint64_t> Allocate(uint64_t size) noexcept;
    void Destroy(uint64_t offset) noexcept;

private:
    radray::vector<NodeState> _tree;
    uint64_t _capacity;
};

}  // namespace radray
