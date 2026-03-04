#include <radray/allocator.h>

#include <algorithm>
#include <bit>

#include <radray/logger.h>

namespace radray {

static constexpr size_t LeftChild(size_t index) noexcept { return index * 2 + 1; }
// static constexpr size_t RightChild(size_t index) noexcept { return index * 2 + 2; }
static constexpr size_t Parent(size_t index) noexcept { return (index - 1) / 2; }

static constexpr size_t NodeLevel(size_t index) noexcept {
    return std::bit_width(index + 1) - 1;
}

static constexpr size_t LevelStart(size_t level) noexcept {
    return (size_t{1} << level) - 1;
}

static size_t NodeSize(size_t virtualCapacity, size_t index) noexcept {
    const size_t level = NodeLevel(index);
    return virtualCapacity >> level;
}

static size_t NodeOffset(size_t virtualCapacity, size_t index) noexcept {
    const size_t level = NodeLevel(index);
    const size_t first = LevelStart(level);
    const size_t pos = index - first;
    return pos * (virtualCapacity >> level);
}

static size_t ActualCapacity(size_t capacity, size_t virtualCapacity, size_t index) noexcept {
    const size_t offset = NodeOffset(virtualCapacity, index);
    const size_t size = NodeSize(virtualCapacity, index);
    if (offset >= capacity) {
        return 0;
    }
    return std::min(size, capacity - offset);
}

BuddyAllocator::BuddyAllocator(size_t capacity) noexcept : _capacity(capacity) {
    _virtualCapacity = capacity > 0 ? std::bit_ceil(capacity) : size_t{1};
    RADRAY_ASSERT(_capacity != 0);
    RADRAY_ASSERT(_virtualCapacity != 0);
    RADRAY_ASSERT(std::has_single_bit(_virtualCapacity));
    RADRAY_ASSERT(_capacity <= _virtualCapacity);
    const size_t treeSize = _virtualCapacity * 2 - 1;

    _nodes.assign(treeSize, {});
    for (size_t idx = 0; idx < treeSize; ++idx) {
        _nodes[idx].Longest = ActualCapacity(_capacity, _virtualCapacity, idx);
    }
}

std::optional<BuddyAllocator::Allocation> BuddyAllocator::Allocate(size_t size_) noexcept {
    if (size_ == 0) {
        return std::nullopt;
    }
    const size_t request = size_;
    if (request > _capacity) {
        return std::nullopt;
    }
    const size_t targetSize = std::min(std::bit_ceil(request), _virtualCapacity);
    size_t index = 0;
    size_t nodeSize = NodeSize(_virtualCapacity, index);
    if (_nodes[index].Longest < request) {
        return std::nullopt;
    }
    while (nodeSize > targetSize) {
        const size_t left = LeftChild(index);
        const size_t right = left + 1;
        if (_nodes[left].Longest >= request) {
            index = left;
        } else if (_nodes[right].Longest >= request) {
            index = right;
        } else {
            return std::nullopt;
        }
        nodeSize = NodeSize(_virtualCapacity, index);
    }
    if (_nodes[index].Longest < request || ActualCapacity(_capacity, _virtualCapacity, index) < request) {
        return std::nullopt;
    }
    _nodes[index].Longest = 0;
    const size_t offset = NodeOffset(_virtualCapacity, index);
    UpdateAncestors(index);
    return std::make_optional(BuddyAllocator::Allocation{offset, index});
}

void BuddyAllocator::Destroy(Allocation allocation) noexcept {
    RADRAY_ASSERT(allocation.Offset < _capacity);
    RADRAY_ASSERT(allocation.NodeIndex < _nodes.size());
    const size_t index = allocation.NodeIndex;
    RADRAY_ASSERT(NodeOffset(_virtualCapacity, index) == allocation.Offset);
    RADRAY_ASSERT(ActualCapacity(_capacity, _virtualCapacity, index) != 0);
    RADRAY_ASSERT(_nodes[index].Longest == 0);
    const size_t left = LeftChild(index);
    if (left < _nodes.size()) {
        const size_t right = left + 1;
        const size_t leftCap = ActualCapacity(_capacity, _virtualCapacity, left);
        const size_t rightCap = ActualCapacity(_capacity, _virtualCapacity, right);
        RADRAY_ASSERT(_nodes[left].Longest == leftCap);
        RADRAY_ASSERT(_nodes[right].Longest == rightCap);
    }
    _nodes[index].Longest = ActualCapacity(_capacity, _virtualCapacity, index);
    UpdateAncestors(index);
}

void BuddyAllocator::UpdateAncestors(size_t index) noexcept {
    while (index > 0) {
        index = Parent(index);
        const size_t left = LeftChild(index);
        const size_t right = left + 1;
        const size_t leftCap = ActualCapacity(_capacity, _virtualCapacity, left);
        const size_t rightCap = ActualCapacity(_capacity, _virtualCapacity, right);
        const size_t idxCap = ActualCapacity(_capacity, _virtualCapacity, index);
        if (_nodes[left].Longest == leftCap && _nodes[right].Longest == rightCap) {
            _nodes[index].Longest = idxCap;
        } else {
            _nodes[index].Longest = std::max(_nodes[left].Longest, _nodes[right].Longest);
        }
    }
}

FirstFitAllocator::FirstFitAllocator(size_t capacity) noexcept : _capacity(capacity) {
    _freeRanges.reserve(64);
    if (_capacity > 0) {
        _freeRanges.emplace_back(FreeRange{0, _capacity});
    }
}

std::optional<FirstFitAllocator::Allocation> FirstFitAllocator::Allocate(size_t size) noexcept {
    if (size == 0 || size > _capacity) {
        return std::nullopt;
    }

    for (size_t i = 0; i < _freeRanges.size(); ++i) {
        FreeRange& range = _freeRanges[i];
        if (range.Length < size) {
            continue;
        }

        const size_t start = range.Start;
        if (range.Length == size) {
            _freeRanges.erase(_freeRanges.begin() + i);
        } else {
            range.Start += size;
            range.Length -= size;
        }
        return std::make_optional(Allocation{start, size});
    }

    return std::nullopt;
}

void FirstFitAllocator::Destroy(Allocation allocation) noexcept {
    RADRAY_ASSERT(allocation.Length != 0);
    RADRAY_ASSERT(allocation.Start <= _capacity);
    RADRAY_ASSERT(allocation.Length <= _capacity - allocation.Start);

    auto it = std::lower_bound(
        _freeRanges.begin(),
        _freeRanges.end(),
        allocation.Start,
        [](const FreeRange& range, size_t start) {
            return range.Start < start;
        });

    if (it != _freeRanges.begin()) {
        const auto prev = it - 1;
        RADRAY_ASSERT(prev->Start <= _capacity);
        RADRAY_ASSERT(prev->Length <= _capacity - prev->Start);
        RADRAY_ASSERT(prev->Start + prev->Length <= allocation.Start);
    }
    if (it != _freeRanges.end()) {
        RADRAY_ASSERT(allocation.Start + allocation.Length <= it->Start);
    }

    size_t mergedStart = allocation.Start;
    size_t mergedLength = allocation.Length;

    if (it != _freeRanges.begin()) {
        auto prev = it - 1;
        const size_t prevEnd = prev->Start + prev->Length;
        if (prevEnd == mergedStart) {
            mergedStart = prev->Start;
            RADRAY_ASSERT(mergedLength <= _capacity - mergedStart);
            RADRAY_ASSERT(prev->Length <= (_capacity - mergedStart) - mergedLength);
            mergedLength += prev->Length;
            it = _freeRanges.erase(prev);
        }
    }

    if (it != _freeRanges.end()) {
        RADRAY_ASSERT(mergedLength <= _capacity - mergedStart);
        const size_t mergedEnd = mergedStart + mergedLength;
        if (mergedEnd == it->Start) {
            RADRAY_ASSERT(it->Length <= (_capacity - mergedStart) - mergedLength);
            mergedLength += it->Length;
            it = _freeRanges.erase(it);
        }
    }

    _freeRanges.insert(it, FreeRange{mergedStart, mergedLength});
}

}  // namespace radray
