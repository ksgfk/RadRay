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

FreeListAllocator::FreeListAllocator(
    size_t capacity) noexcept
    : _capacity(capacity) {
    _nodes.reserve(64);
    _nodeFreePool.reserve(64);
    _freeNodes.reserve(64);
    _head = NewNode(0, _capacity, FreeListAllocator::NodeState::Free);
    AddFree(_head);
}

std::optional<FreeListAllocator::Allocation> FreeListAllocator::Allocate(size_t size) noexcept {
    if (size == 0 || size > _capacity) {
        return std::nullopt;
    }
    uint32_t best = FreeListAllocator::npos;
    size_t bestLen = std::numeric_limits<size_t>::max();
    for (uint32_t idx : _freeNodes) {
        const auto& n = _nodes[idx];
        if (n.Length < size) {
            continue;
        }
        if (n.Length < bestLen) {
            best = idx;
            bestLen = n.Length;
        }
    }
    if (best == FreeListAllocator::npos) {
        return std::nullopt;
    }
    Node& node = _nodes[best];
    RADRAY_ASSERT(node.State == FreeListAllocator::NodeState::Free);
    RemoveFree(best);
    const size_t allocStart = node.Start;
    if (node.Length == size) {
        node.State = FreeListAllocator::NodeState::Used;
        return std::make_optional(FreeListAllocator::Allocation{
            allocStart,
            size,
            best,
            node.Generation});
    }
    RADRAY_ASSERT(node.Length > size);
    const size_t remainStart = node.Start + size;
    const size_t remainLen = node.Length - size;
    uint32_t remainIdx = NewNode(remainStart, remainLen, FreeListAllocator::NodeState::Free);
    remainIdx = static_cast<uint32_t>(remainIdx);
    _nodes[remainIdx].Prev = best;
    _nodes[remainIdx].Next = node.Next;
    if (node.Next != FreeListAllocator::npos) {
        _nodes[node.Next].Prev = remainIdx;
    }
    node.Next = remainIdx;
    node.Length = size;
    node.State = FreeListAllocator::NodeState::Used;
    AddFree(remainIdx);
    return std::make_optional(FreeListAllocator::Allocation{
        allocStart,
        size,
        best,
        node.Generation});
}

void FreeListAllocator::Destroy(Allocation allocation) noexcept {
    RADRAY_ASSERT(allocation.Length != 0);
    RADRAY_ASSERT(allocation.Start < _capacity);
    RADRAY_ASSERT(allocation.Node < _nodes.size());
    uint32_t idx = allocation.Node;
    Node& node = _nodes[idx];
    RADRAY_ASSERT(node.Generation == allocation.Generation);
    RADRAY_ASSERT(node.Start == allocation.Start);
    RADRAY_ASSERT(node.Length == allocation.Length);
    RADRAY_ASSERT(node.State == FreeListAllocator::NodeState::Used);
    node.State = FreeListAllocator::NodeState::Free;
    uint32_t base = idx;
    size_t mergedStart = node.Start;
    size_t mergedLen = node.Length;
    if (node.Prev != FreeListAllocator::npos && _nodes[node.Prev].State == FreeListAllocator::NodeState::Free) {
        uint32_t p = node.Prev;
        RemoveFree(p);
        base = p;
        mergedStart = _nodes[p].Start;
        mergedLen += _nodes[p].Length;
        _nodes[base].Next = node.Next;
        if (node.Next != FreeListAllocator::npos) {
            _nodes[node.Next].Prev = base;
        }
        DeleteNode(idx);
    }
    while (_nodes[base].Next != FreeListAllocator::npos && _nodes[_nodes[base].Next].State == FreeListAllocator::NodeState::Free) {
        uint32_t n = _nodes[base].Next;
        RemoveFree(n);
        mergedLen += _nodes[n].Length;
        _nodes[base].Next = _nodes[n].Next;
        if (_nodes[n].Next != FreeListAllocator::npos) {
            _nodes[_nodes[n].Next].Prev = base;
        }
        DeleteNode(n);
    }
    _nodes[base].Start = mergedStart;
    _nodes[base].Length = mergedLen;
    _nodes[base].State = FreeListAllocator::NodeState::Free;
    AddFree(base);
}

uint32_t FreeListAllocator::NewNode(size_t start, size_t length, NodeState state) noexcept {
    uint32_t idx;
    if (!_nodeFreePool.empty()) {
        idx = _nodeFreePool.back();
        _nodeFreePool.pop_back();
    } else {
        idx = static_cast<uint32_t>(_nodes.size());
        _nodes.emplace_back();
    }
    Node& n = _nodes[idx];
    n.Start = start;
    n.Length = length;
    n.State = state;
    n.Prev = npos;
    n.Next = npos;
    n.FreePos = npos;
    return idx;
}

void FreeListAllocator::DeleteNode(uint32_t idx) noexcept {
    if (_nodes[idx].FreePos != npos) {
        RemoveFree(idx);
    }
    _nodes[idx].Generation += 1;
    _nodes[idx].Start = 0;
    _nodes[idx].Length = 0;
    _nodes[idx].Prev = npos;
    _nodes[idx].Next = npos;
    _nodes[idx].FreePos = npos;
    _nodes[idx].State = FreeListAllocator::NodeState::Free;
    _nodeFreePool.emplace_back(idx);
}

void FreeListAllocator::AddFree(uint32_t idx) noexcept {
    Node& n = _nodes[idx];
    if (n.FreePos != npos) {
        return;
    }
    n.FreePos = static_cast<uint32_t>(_freeNodes.size());
    _freeNodes.emplace_back(idx);
}

void FreeListAllocator::RemoveFree(uint32_t idx) noexcept {
    Node& n = _nodes[idx];
    if (n.FreePos == npos) {
        return;
    }
    const uint32_t pos = n.FreePos;
    RADRAY_ASSERT(pos < _freeNodes.size());
    const uint32_t backIdx = _freeNodes.back();
    _freeNodes[pos] = backIdx;
    _freeNodes.pop_back();
    if (backIdx != idx) {
        _nodes[backIdx].FreePos = pos;
    }
    n.FreePos = npos;
}

}  // namespace radray
