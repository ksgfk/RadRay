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
    if (_capacity == 0 || offset >= _capacity) {
        RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
        return;
    }

    size_t index = 0;
    while (true) {
        if (_allocated[index] && _nodeOffset[index] == offset) {
            break;
        }

        if (_nodeSize[index] == 1) {
            RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
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
    _startToNode.assign(_capacity > 0 ? _capacity : 1, -1);
    _head = NewNode(0, _capacity, FreeListAllocator::NodeState::Free);
    _startToNode[0] = static_cast<int32_t>(_head);
    AddFree(_head);
}

std::optional<size_t> FreeListAllocator::Allocate(size_t size) noexcept {
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
        return std::make_optional(allocStart);
    }
    RADRAY_ASSERT(node.length > size);
    const size_t remainStart = node.start + size;
    const size_t remainLen = node.length - size;
    uint32_t remainIdx = NewNode(remainStart, remainLen, FreeListAllocator::NodeState::Free);
    _startToNode[remainStart] = static_cast<int32_t>(remainIdx);
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
    return std::make_optional(allocStart);
}

void FreeListAllocator::Destroy(size_t offset) noexcept {
    if (offset >= _capacity) {
        RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
        return;
    }
    int32_t idxSigned = _startToNode[offset];
    if (idxSigned < 0) {
        RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
        return;
    }
    uint32_t idx = static_cast<uint32_t>(idxSigned);
    Node& node = _nodes[idx];
    if (node.state == FreeListAllocator::NodeState::Free) {
        RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
        return;
    }
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
        _startToNode[node.start] = -1;
        DeleteNode(idx);
    } else {
        RemoveFree(idx);
    }
    while (_nodes[base].next != FreeListAllocator::npos && _nodes[_nodes[base].next].state == FreeListAllocator::NodeState::Free) {
        uint32_t n = _nodes[base].next;
        RemoveFree(n);
        mergedLen += _nodes[n].length;
        _nodes[base].next = _nodes[n].next;
        if (_nodes[n].next != FreeListAllocator::npos) {
            _nodes[_nodes[n].next].prev = base;
        }
        _startToNode[_nodes[n].start] = -1;
        DeleteNode(n);
    }
    _nodes[base].start = mergedStart;
    _nodes[base].length = mergedLen;
    _nodes[base].state = FreeListAllocator::NodeState::Free;
    _startToNode[mergedStart] = static_cast<int32_t>(base);
    AddFree(base);
}

uint32_t FreeListAllocator::NewNode(size_t start, size_t length, NodeState state) noexcept {
    uint32_t idx;
    if (!_nodeFreePool.empty()) {
        idx = _nodeFreePool.back();
        _nodeFreePool.pop_back();
        _nodes[idx] = Node{};
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
    return idx;
}

void FreeListAllocator::DeleteNode(uint32_t idx) noexcept {
    _nodeFreePool.emplace_back(idx);
}

void FreeListAllocator::AddFree(uint32_t idx) noexcept {
    for (uint32_t existing : _freeNodes) {
        if (existing == idx) {
            return;
        }
    }
    _freeNodes.emplace_back(idx);
}

void FreeListAllocator::RemoveFree(uint32_t idx) noexcept {
    for (size_t pos = 0; pos < _freeNodes.size(); ++pos) {
        if (_freeNodes[pos] != idx) {
            continue;
        }
        _freeNodes[pos] = _freeNodes.back();
        _freeNodes.pop_back();
        return;
    }
}

}  // namespace radray
