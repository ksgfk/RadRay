#pragma once

#include <concepts>
#include <limits>
#include <span>
#include <utility>

#include <radray/logger.h>
#include <radray/types.h>

namespace radray {

struct SparseSetHandle {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    uint32_t Generation{0};

    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }

    constexpr static SparseSetHandle Invalid() noexcept { return {}; }

    constexpr auto operator<=>(const SparseSetHandle&) const noexcept = default;
};

}  // namespace radray

template <class CharT>
struct fmt::formatter<radray::SparseSetHandle, CharT> : fmt::formatter<radray::string, CharT> {
    template <class FormatContext>
    auto format(const radray::SparseSetHandle& val, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "SparseSetHandle{{Index={}, Generation={}}}", val.Index, val.Generation);
    }
};

namespace radray {

template <typename T>
concept SparseSetElement = std::move_constructible<T> && std::assignable_from<T&, T>;

template <SparseSetElement T>
class SparseSet {
public:
    SparseSet() noexcept = default;

    explicit SparseSet(uint32_t initialCapacity) {
        this->Reserve(initialCapacity);
    }

    template <typename... Args>
    SparseSetHandle Emplace(Args&&... args) noexcept {
        RADRAY_ASSERT(_denseValues.size() < InvalidIndex);

        const bool reuseSparseSlot = _freeHead != InvalidIndex;
        uint32_t sparseIndex = InvalidIndex;
        uint32_t nextFree = InvalidIndex;

        if (reuseSparseSlot) {
            sparseIndex = _freeHead;
            RADRAY_ASSERT(sparseIndex < _sparse.size());
            RADRAY_ASSERT(!_sparse[sparseIndex].Alive);
            nextFree = _sparse[sparseIndex].NextFree;
        } else {
            RADRAY_ASSERT(_sparse.size() < InvalidIndex);
            sparseIndex = static_cast<uint32_t>(_sparse.size());
            _sparse.emplace_back();
        }

        const uint32_t denseIndex = static_cast<uint32_t>(_denseValues.size());
        _denseValues.emplace_back(std::forward<Args>(args)...);
        _sparseIndices.emplace_back(sparseIndex);

        if (reuseSparseSlot) {
            _freeHead = nextFree;
        }

        auto& node = _sparse[sparseIndex];
        node.DenseIndex = denseIndex;
        node.NextFree = InvalidIndex;
        node.Alive = true;

        return SparseSetHandle{sparseIndex, node.Generation};
    }

    void Destroy(SparseSetHandle handle) noexcept {
        RADRAY_ASSERT(this->IsAlive(handle));

        auto& node = _sparse[handle.Index];
        const uint32_t denseIndex = node.DenseIndex;
        RADRAY_ASSERT(node.DenseIndex < _denseValues.size());
        RADRAY_ASSERT(_sparseIndices[denseIndex] == handle.Index);
        const uint32_t lastDenseIndex = static_cast<uint32_t>(_denseValues.size() - 1);

        if (denseIndex != lastDenseIndex) {
            _denseValues[denseIndex] = std::move(_denseValues[lastDenseIndex]);
            _sparseIndices[denseIndex] = _sparseIndices[lastDenseIndex];

            const uint32_t movedSparseIndex = _sparseIndices[denseIndex];
            _sparse[movedSparseIndex].DenseIndex = denseIndex;
        }

        _denseValues.pop_back();
        _sparseIndices.pop_back();

        node.Alive = false;
        ++node.Generation;
        node.NextFree = _freeHead;
        _freeHead = handle.Index;
    }

    T& Get(SparseSetHandle handle) {
        RADRAY_ASSERT(this->IsAlive(handle));
        return _denseValues[_sparse[handle.Index].DenseIndex];
    }

    const T& Get(SparseSetHandle handle) const {
        RADRAY_ASSERT(this->IsAlive(handle));
        return _denseValues[_sparse[handle.Index].DenseIndex];
    }

    T* TryGet(SparseSetHandle handle) noexcept {
        if (!this->IsAlive(handle)) {
            return nullptr;
        }
        return &_denseValues[_sparse[handle.Index].DenseIndex];
    }

    const T* TryGet(SparseSetHandle handle) const noexcept {
        if (!this->IsAlive(handle)) {
            return nullptr;
        }
        return &_denseValues[_sparse[handle.Index].DenseIndex];
    }

    bool IsAlive(SparseSetHandle handle) const noexcept {
        if (!handle.IsValid() || handle.Index >= _sparse.size()) {
            return false;
        }

        const auto& node = _sparse[handle.Index];
        return node.Alive && node.Generation == handle.Generation;
    }

    std::span<T> Values() noexcept {
        return std::span<T>{_denseValues.data(), _denseValues.size()};
    }

    std::span<const T> Values() const noexcept {
        return std::span<const T>{_denseValues.data(), _denseValues.size()};
    }

    uint32_t Count() const noexcept {
        RADRAY_ASSERT(_denseValues.size() <= InvalidIndex);
        return static_cast<uint32_t>(_denseValues.size());
    }

    bool Empty() const noexcept {
        return _denseValues.empty();
    }

    void Reserve(uint32_t capacity) {
        _sparse.reserve(capacity);
    }

    void Clear() {
        _denseValues.clear();
        _sparseIndices.clear();
        _freeHead = InvalidIndex;

        for (size_t i = _sparse.size(); i > 0; --i) {
            auto& node = _sparse[i - 1];
            if (node.Alive) {
                ++node.Generation;
                node.Alive = false;
            }
            node.NextFree = _freeHead;
            _freeHead = static_cast<uint32_t>(i - 1);
        }
    }

private:
    static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

    struct SparseNode {
        uint32_t DenseIndex{0};
        uint32_t Generation{0};
        uint32_t NextFree{InvalidIndex};
        bool Alive{false};
    };

    vector<SparseNode> _sparse{};
    vector<T> _denseValues{};
    vector<uint32_t> _sparseIndices{};
    uint32_t _freeHead{InvalidIndex};
};

}  // namespace radray
