#include <radray/allocator.h>

#include <algorithm>
#include <bit>

#include <radray/errors.h>
#include <radray/basic_math.h>
#include <radray/utility.h>

namespace radray {

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
    auto iter = _nodes.emplace(0, make_unique<FreeListAllocator::LinkNode>(0, _capacity));
    _sizeQuery.emplace(_capacity, iter.first->second.get());
}

std::optional<size_t> FreeListAllocator::Allocate(size_t size) noexcept {
    if (size == 0 || size > _capacity) {
        return std::nullopt;
    }
    auto iter = _sizeQuery.lower_bound(size);
    if (iter == _sizeQuery.end()) {
        return std::nullopt;
    }
    FreeListAllocator::LinkNode* node = iter->second;
    if (node->_length == size) {
        _sizeQuery.erase(iter);
        node->_state = FreeListAllocator::NodeState::Used;
        return std::make_optional(node->_start);
    } else {
        RADRAY_ASSERT(node->_length > size);
        size_t newStart = node->_start + size;
        size_t newLength = node->_length - size;
        auto [newIter, isInsert] = _nodes.emplace(
            newStart,
            make_unique<FreeListAllocator::LinkNode>(newStart, newLength));
        RADRAY_ASSERT(isInsert);
        FreeListAllocator::LinkNode* newNode = newIter->second.get();
        node->_state = FreeListAllocator::NodeState::Used;
        node->_length = size;
        newNode->_state = FreeListAllocator::NodeState::Free;
        newNode->_prev = node;
        newNode->_next = node->_next;
        node->_next = newNode;
        if (newNode->_next != nullptr) {
            newNode->_next->_prev = newNode;
        }
        _sizeQuery.erase(iter);
        _sizeQuery.emplace(newNode->_length, newNode);
        return std::make_optional(node->_start);
    }
}

void FreeListAllocator::Destroy(size_t offset) noexcept {
    auto iter = _nodes.find(offset);
    if (iter == _nodes.end()) {
        RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
        return;
    }
    FreeListAllocator::LinkNode* node = iter->second.get();
    if (node->_state == FreeListAllocator::NodeState::Free) {
        RADRAY_ABORT("{} '{}'", Errors::InvalidOperation, "offset");
        return;
    }
    node->_state = FreeListAllocator::NodeState::Free;
    FreeListAllocator::LinkNode* startFreePtr = node;
    while (startFreePtr->_prev != nullptr && startFreePtr->_prev->_state == FreeListAllocator::NodeState::Free) {
        startFreePtr = startFreePtr->_prev;
    }
    FreeListAllocator::LinkNode* endFreePtr = node;
    while (endFreePtr->_next != nullptr && endFreePtr->_next->_state == FreeListAllocator::NodeState::Free) {
        endFreePtr = endFreePtr->_next;
    }
    if (startFreePtr == endFreePtr) {
        _sizeQuery.emplace(node->_length, node);
        return;
    }
    FreeListAllocator::LinkNode* endFreeNext = endFreePtr->_next;
    size_t newSize = 0;
    for (FreeListAllocator::LinkNode* i = startFreePtr; i != endFreeNext;) {
        newSize += i->_length;
        for (auto j = _sizeQuery.begin(); j != _sizeQuery.end(); j++) {
            if (j->second == i) {
                _sizeQuery.erase(j);
                break;
            }
        }
        if (i == startFreePtr) {
            i = i->_next;
        } else {
            auto key = i->_start;
            i = i->_next;
            _nodes.erase(key);
        }
    }
    startFreePtr->_length = newSize;
    startFreePtr->_next = endFreeNext;
    if (endFreeNext != nullptr) {
        endFreeNext->_prev = startFreePtr;
    }
    _sizeQuery.emplace(startFreePtr->_length, startFreePtr);
}

FreeListAllocator::LinkNode::LinkNode(size_t start, size_t length) noexcept
    : _start(start),
      _length(length),
      _prev(nullptr),
      _next(nullptr),
      _state(FreeListAllocator::NodeState::Free) {}

}  // namespace radray
