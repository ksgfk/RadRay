#include <radray/allocator.h>

#include <limits>

#include <radray/basic_math.h>

namespace radray {

LinearAllocator::LinearAllocator(IAllocator* alloc, uint64_t capacity, double incMag) noexcept
    : _proxy(alloc),
      _capacity(capacity),
      _initCapacity(capacity),
      _capacityIncMag(incMag) {}

LinearAllocator::~LinearAllocator() noexcept {
    for (auto&& i : _buffers) {
        _proxy->Destroy(i.handle);
    }
    _buffers.clear();
}

LinearAllocator::View LinearAllocator::Allocate(uint64_t targetSize, uint64_t align) noexcept {
    targetSize = std::max(targetSize, align);
    for (auto&& i : _buffers) {
        auto count = CalcAlign(i.count, align);
        if (i.capacity > count) {
            int64_t leftSize = (i.capacity - count);
            if (leftSize >= targetSize) {
                auto offset = count;
                i.count = count + targetSize;
                return {i.handle, offset};
            }
        }
    }
    if (_capacity < targetSize) {
        _capacity = std::max<uint64_t>(_capacity, _capacity * _capacityIncMag);
    }
    auto allocSize = std::max<uint64_t>(targetSize, _capacity);
    auto neopt = _proxy->Allocate(allocSize, 1);
    auto newuint64_t = neopt.value();
    _buffers.emplace_back(Buffer{
        .handle = newuint64_t,
        .capacity = allocSize,
        .count = targetSize});
    return {newuint64_t, 0};
}

LinearAllocator::View LinearAllocator::Allocate(uint64_t size) noexcept {
    for (auto&& i : _buffers) {
        uint64_t freeSize = i.capacity - i.count;
        if (freeSize >= size) {
            uint64_t offset = i.count;
            i.count += size;
            return {i.handle, offset};
        }
    }
    if (_capacity < size) {
        _capacity = std::max<uint64_t>(_capacity, static_cast<uint64_t>(_capacity * _capacityIncMag));
    }
    uint64_t allocSize = std::max<uint64_t>(size, _capacity);
    auto neopt = _proxy->Allocate(allocSize, 1);
    auto newuint64_t = neopt.value();
    _buffers.emplace_back(Buffer{
        .handle = newuint64_t,
        .capacity = allocSize,
        .count = size});
    return {newuint64_t, 0};
}

void LinearAllocator::Clear() noexcept {
    switch (_buffers.size()) {
        case 0:
            break;
        case 1: {
            auto&& i = _buffers[0];
            i.count = 0;
            break;
        }
        default: {
            uint64_t sumSize = 0u;
            for (auto&& i : _buffers) {
                sumSize += i.capacity;
                _proxy->Destroy(i.handle);
            }
            _buffers.clear();
            _buffers.emplace_back(Buffer{
                .handle = _proxy->Allocate(sumSize, 1).value(),
                .capacity = sumSize,
                .count = 0});
            break;
        }
    }
}

void LinearAllocator::Reset() noexcept {
    _capacity = _initCapacity;
    if (_buffers.empty()) {
        return;
    }
    if (_buffers.size() > 1) {
        for (size_t i = 1; i < _buffers.size(); i++) {
            _proxy->Destroy(_buffers[i].handle);
        }
        _buffers.resize(1);
    }
    Buffer& first = _buffers[0];
    if (first.capacity > _capacity) {
        _proxy->Destroy(first.handle);
        first.handle = _proxy->Allocate(_capacity, 1).value();
        first.capacity = _capacity;
    }
    first.count = 0;
}

// https://github.com/cloudwu/buddy/blob/master/buddy.c

BuddyAllocator::BuddyAllocator(uint64_t capacity) noexcept : _capacity(capacity) {
    uint64_t vcapa = RoundUpPow2(capacity);
    RADRAY_ASSERT(vcapa <= std::numeric_limits<int64_t>::max(), "too large tree");
    uint64_t treeSize = 2 * vcapa - 1;  // 建一颗满二叉树
    _tree.resize(treeSize, NodeState::Unused);
}

std::optional<uint64_t> BuddyAllocator::Allocate(uint64_t size_, uint64_t align) noexcept {
    uint64_t size = size_ == 0 ? 1 : size_;
    size = RoundUpPow2(size);
    size = CalcAlign(size, align);
    RADRAY_ASSERT(IsPowerOf2(size), "cannot alloc size={} align={}", size_, align);
    int64_t vCapacity = RoundUpPow2(_capacity);  // 满二叉树情况虚拟容量
    int64_t vlength = vCapacity;
    if (size > vlength) {
        return std::nullopt;
    }
    int64_t ptr = 0;
    while (ptr >= 0) {          // 从根节点开始搜索可用空间
        if (size == vlength) {  // 找到目标大小层
            if (_tree[ptr] == NodeState::Unused) {
                uint64_t offset = ((uint64_t)ptr + 1) * vlength - vCapacity;  // 树节点下标对应内存下标
                if (offset + size_ > _capacity) {                             // 分配块是否超出实际容量
                    return std::nullopt;
                }
                _tree[ptr] = NodeState::Used;  // 标记为已使用
                int64_t now = ptr;
                while (true) {                                           // 这里需要和buddy节点一起判断父节点状态，递归
                    int64_t buddy = now - 1 + ((now % 2 == 0) ? 0 : 2);  // 找到buddy节点
                    if (buddy > 0 && (_tree[buddy] == NodeState::Used || _tree[buddy] == NodeState::Full)) {
                        now = (now + 1) / 2 - 1;  // buddy节点也已分配，将父节点设为左右子节点都满，递归
                        _tree[now] = NodeState::Full;
                    } else {
                        break;
                    }
                }
                return std::make_optional(offset);
            }
        } else {                                         // 还没到目标大小的层
            if (_tree[ptr] == NodeState::Unused) {  // 还没用过的节点，分裂
                _tree[ptr] = NodeState::Split;
                _tree[ptr * 2 + 1] = NodeState::Unused;
                _tree[ptr * 2 + 2] = NodeState::Unused;
            }
            if (_tree[ptr] == NodeState::Split) {  // 分裂状态说明左子节点可分用
                ptr = ptr * 2 + 1;
                vlength /= 2;
                continue;
            }
        }
        if (ptr % 2 != 0) {  // 还没到目标大小的层，但是该节点已经没有可用空间了，自己是父节点的左节点，去父节点的右节点
            ptr++;
            continue;
        }
        while (true) {  // 自己是父节点的右节点，要回溯到一个自己是父节点左子节点的节点
            vlength *= 2;
            ptr = (ptr + 1) / 2 - 1;
            if (ptr < 0) {
                return std::nullopt;
            }
            if (ptr % 2 != 0) {
                ptr++;
                break;
            }
        }
    }
    return std::nullopt;
}

void BuddyAllocator::Destroy(uint64_t offset) noexcept {
    int64_t vCapacity = RoundUpPow2(_capacity);
    int64_t vlength = vCapacity;
    int64_t ptr = 0;
    int64_t left = 0;
    while (true) {
        switch (_tree[ptr]) {
            case NodeState::Used: {  // 找到需要释放的节点
                RADRAY_ASSERT(offset == left, "what?");
                int64_t now = ptr;
                while (true) {
                    int64_t buddy = now - 1 + ((now % 2 == 0) ? 0 : 2);
                    // 已分配的节点，找它buddy节点是不是有分配
                    if (buddy < 0 || _tree[buddy] != NodeState::Unused) {
                        // 找到了无法合并的节点，停止合并
                        _tree[now] = NodeState::Unused;
                        while (true) {
                            // 但是还是要继续往上回溯，将可能为满的节点设为存在已分配的子节点，即split
                            now = (now + 1) / 2 - 1;
                            if (now >= 0 && _tree[now] == NodeState::Full) {
                                _tree[now] = NodeState::Split;
                            } else {
                                return;
                            }
                        }
                    }
                    // buddy节点没分配，将自己和buddy合并，也就是去父节点设置为unused
                    // 父节点是unused就不可能进到这个需要释放的节点
                    now = (now + 1) / 2 - 1;
                }
                return;
            }
            case NodeState::Unused: {
                RADRAY_ABORT("invalid offset {}", offset);
                return;
            }
            default: {  // 分裂或者子节点都满的情况，往需要释放的节点找过去
                vlength /= 2;
                if (offset < left + vlength) {
                    ptr = ptr * 2 + 1;
                } else {
                    left += vlength;
                    ptr = ptr * 2 + 2;
                }
                break;
            }
        }
    }
}

}  // namespace radray
