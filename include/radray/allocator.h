#pragma once

#include <optional>
#include <type_traits>

#include <radray/types.h>
#include <radray/utility.h>
#include <radray/logger.h>

namespace radray {

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
