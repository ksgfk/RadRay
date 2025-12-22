#include <radray/allocator.h>

#include <algorithm>
#include <bit>

#include <radray/errors.h>
#include <radray/basic_math.h>
#include <radray/utility.h>

namespace radray {

static constexpr size_t LeftChild(size_t index) noexcept { return index * 2 + 1; }
// static constexpr size_t RightChild(size_t index) noexcept { return index * 2 + 2; }
static constexpr size_t Parent(size_t index) noexcept { return (index - 1) / 2; }

BuddyAllocator::BuddyAllocator(size_t capacity) noexcept : _capacity(capacity) {
    _virtualCapacity = capacity > 0 ? std::bit_ceil(capacity) : size_t{1};
    RADRAY_ASSERT(_capacity != 0);
    RADRAY_ASSERT(_virtualCapacity != 0);
    RADRAY_ASSERT(std::has_single_bit(_virtualCapacity));
    RADRAY_ASSERT(_capacity <= _virtualCapacity);
    const size_t treeSize = _virtualCapacity * 2 - 1;

    _longest.assign(treeSize, 0);
    _nodeSize.assign(treeSize, 0);
    _nodeOffset.assign(treeSize, 0);
    _actualCapacity.assign(treeSize, 0);
    _allocated.assign(treeSize, 0);

    const size_t leafStart = _virtualCapacity - 1;
    for (size_t i = 0; i < _virtualCapacity; ++i) {
        const size_t idx = leafStart + i;
        _nodeSize[idx] = 1;
        _nodeOffset[idx] = i;
        if (i < _capacity) {
            _actualCapacity[idx] = 1;
            _longest[idx] = 1;
        }
    }

    if (_virtualCapacity > 1) {
        for (size_t idx = leafStart; idx-- > 0;) {
            const size_t left = LeftChild(idx);
            const size_t right = left + 1;
            _nodeSize[idx] = _nodeSize[left] << 1;
            _nodeOffset[idx] = _nodeOffset[left];
            _actualCapacity[idx] = _actualCapacity[left] + _actualCapacity[right];
            _longest[idx] = _actualCapacity[idx];
        }
    }
}

std::optional<size_t> BuddyAllocator::Allocate(size_t size_) noexcept {
    if (_capacity == 0) {
        return std::nullopt;
    }
    const size_t request = size_ == 0 ? size_t{1} : size_;
    if (request > _capacity) {
        return std::nullopt;
    }
    const size_t targetSize = std::min(std::bit_ceil(request), _virtualCapacity);
    size_t index = 0;
    size_t nodeSize = _nodeSize[index];
    if (_longest[index] < request) {
        return std::nullopt;
    }
    while (nodeSize > targetSize) {
        const size_t left = LeftChild(index);
        const size_t right = left + 1;
        if (_longest[left] >= request) {
            index = left;
        } else if (_longest[right] >= request) {
            index = right;
        } else {
            return std::nullopt;
        }
        nodeSize = _nodeSize[index];
    }
    if (_longest[index] < request || _actualCapacity[index] < request) {
        return std::nullopt;
    }
    _allocated[index] = 1;
    _longest[index] = 0;
    const size_t offset = _nodeOffset[index];
    UpdateAncestors(index);
    return offset;
}

void BuddyAllocator::Destroy(size_t offset) noexcept {
    RADRAY_ASSERT(offset < _capacity);
    size_t index = 0;
    while (true) {
        if (_allocated[index] && _nodeOffset[index] == offset) {
            break;
        }
        if (_nodeSize[index] == 1) {
            RADRAY_ASSERT(false);
            return;
        }
        const size_t left = LeftChild(index);
        const size_t right = left + 1;
        if (offset < _nodeOffset[right]) {
            index = left;
        } else {
            index = right;
        }
    }
    RADRAY_ASSERT(_allocated[index] != 0);
    _allocated[index] = 0;
    _longest[index] = _actualCapacity[index];
    UpdateAncestors(index);
}

void BuddyAllocator::UpdateAncestors(size_t index) noexcept {
    while (index > 0) {
        index = Parent(index);
        const size_t left = LeftChild(index);
        const size_t right = left + 1;
        if (_longest[left] == _actualCapacity[left] && _longest[right] == _actualCapacity[right]) {
            _longest[index] = _actualCapacity[index];
        } else {
            _longest[index] = std::max(_longest[left], _longest[right]);
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
        if (n.length < size) {
            continue;
        }
        if (n.length < bestLen) {
            best = idx;
            bestLen = n.length;
        }
    }
    if (best == FreeListAllocator::npos) {
        return std::nullopt;
    }
    Node& node = _nodes[best];
    RADRAY_ASSERT(node.state == FreeListAllocator::NodeState::Free);
    RemoveFree(best);
    const size_t allocStart = node.start;
    if (node.length == size) {
        node.state = FreeListAllocator::NodeState::Used;
        return std::make_optional(FreeListAllocator::Allocation{
            allocStart,
            size,
            best,
            node.generation});
    }
    RADRAY_ASSERT(node.length > size);
    const size_t remainStart = node.start + size;
    const size_t remainLen = node.length - size;
    uint32_t remainIdx = NewNode(remainStart, remainLen, FreeListAllocator::NodeState::Free);
    remainIdx = static_cast<uint32_t>(remainIdx);
    _nodes[remainIdx].prev = best;
    _nodes[remainIdx].next = node.next;
    if (node.next != FreeListAllocator::npos) {
        _nodes[node.next].prev = remainIdx;
    }
    node.next = remainIdx;
    node.length = size;
    node.state = FreeListAllocator::NodeState::Used;
    AddFree(remainIdx);
    return std::make_optional(FreeListAllocator::Allocation{
        allocStart,
        size,
        best,
        node.generation});
}

void FreeListAllocator::Destroy(Allocation allocation) noexcept {
    RADRAY_ASSERT(allocation.Length != 0);
    RADRAY_ASSERT(allocation.Start < _capacity);
    RADRAY_ASSERT(allocation.Node < _nodes.size());
    uint32_t idx = allocation.Node;
    Node& node = _nodes[idx];
    RADRAY_ASSERT(node.generation == allocation.Generation);
    RADRAY_ASSERT(node.start == allocation.Start);
    RADRAY_ASSERT(node.length == allocation.Length);
    RADRAY_ASSERT(node.state == FreeListAllocator::NodeState::Used);
    node.state = FreeListAllocator::NodeState::Free;
    uint32_t base = idx;
    size_t mergedStart = node.start;
    size_t mergedLen = node.length;
    if (node.prev != FreeListAllocator::npos && _nodes[node.prev].state == FreeListAllocator::NodeState::Free) {
        uint32_t p = node.prev;
        RemoveFree(p);
        base = p;
        mergedStart = _nodes[p].start;
        mergedLen += _nodes[p].length;
        _nodes[base].next = node.next;
        if (node.next != FreeListAllocator::npos) {
            _nodes[node.next].prev = base;
        }
        DeleteNode(idx);
    }
    while (_nodes[base].next != FreeListAllocator::npos && _nodes[_nodes[base].next].state == FreeListAllocator::NodeState::Free) {
        uint32_t n = _nodes[base].next;
        RemoveFree(n);
        mergedLen += _nodes[n].length;
        _nodes[base].next = _nodes[n].next;
        if (_nodes[n].next != FreeListAllocator::npos) {
            _nodes[_nodes[n].next].prev = base;
        }
        DeleteNode(n);
    }
    _nodes[base].start = mergedStart;
    _nodes[base].length = mergedLen;
    _nodes[base].state = FreeListAllocator::NodeState::Free;
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
    n.start = start;
    n.length = length;
    n.state = state;
    n.prev = npos;
    n.next = npos;
    n.freePos = npos;
    return idx;
}

void FreeListAllocator::DeleteNode(uint32_t idx) noexcept {
    if (_nodes[idx].freePos != npos) {
        RemoveFree(idx);
    }
    _nodes[idx].generation += 1;
    _nodes[idx].start = 0;
    _nodes[idx].length = 0;
    _nodes[idx].prev = npos;
    _nodes[idx].next = npos;
    _nodes[idx].freePos = npos;
    _nodes[idx].state = FreeListAllocator::NodeState::Free;
    _nodeFreePool.emplace_back(idx);
}

void FreeListAllocator::AddFree(uint32_t idx) noexcept {
    Node& n = _nodes[idx];
    if (n.freePos != npos) {
        return;
    }
    n.freePos = static_cast<uint32_t>(_freeNodes.size());
    _freeNodes.emplace_back(idx);
}

void FreeListAllocator::RemoveFree(uint32_t idx) noexcept {
    Node& n = _nodes[idx];
    if (n.freePos == npos) {
        return;
    }
    const uint32_t pos = n.freePos;
    RADRAY_ASSERT(pos < _freeNodes.size());
    const uint32_t backIdx = _freeNodes.back();
    _freeNodes[pos] = backIdx;
    _freeNodes.pop_back();
    if (backIdx != idx) {
        _nodes[backIdx].freePos = pos;
    }
    n.freePos = npos;
}

StackAllocator::StackAllocator(size_t capacity) noexcept
    : _capacity(capacity),
      _offset(0) {}

std::optional<size_t> StackAllocator::Allocate(size_t size) noexcept {
    if (size == 0 || size > _capacity) {
        return std::nullopt;
    }
    if (_offset + size > _capacity) {
        return std::nullopt;
    }
    const size_t start = _offset;
    _offset += size;
    return start;
}

}  // namespace radray
