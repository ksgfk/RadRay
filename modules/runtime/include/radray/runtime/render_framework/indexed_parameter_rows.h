#pragma once

#include <radray/logger.h>
#include <radray/nullable.h>
#include <radray/types.h>

namespace radray::detail {
// Sparse integer rows: four byte-indexed radix levels address compact values. Reset reuses
// nodes and values without retaining old row keys; no identity hash is needed for indexed draws.
template <class T>
class IndexedParameterRows {
public:
    void Reset() noexcept {
        _values.clear();
        _activeNodes = 0;
    }
    Nullable<const T*> Find(uint32_t row) const noexcept {
        if (_activeNodes == 0) return nullptr;
        uint32_t node = 0;
        for (uint32_t shift = 24; shift != 0; shift -= 8) {
            const auto child = _nodes[node].Children[(row >> shift) & 255];
            if (child == 0) return nullptr;
            node = child - 1;
        }
        const auto value = _nodes[node].Children[row & 255];
        return value ? &_values[value - 1] : nullptr;
    }
    T& GetOrAdd(uint32_t row) {
        if (_activeNodes == 0) AddNode();
        uint32_t node = 0;
        for (uint32_t shift = 24; shift != 0; shift -= 8) {
            const auto offset = (row >> shift) & 255;
            uint32_t child = _nodes[node].Children[offset];
            if (child == 0) {
                child = AddNode() + 1;
                _nodes[node].Children[offset] = child;
            }
            node = child - 1;
        }
        auto& value = _nodes[node].Children[row & 255];
        if (value == 0) {
            if (_values.size() == UINT32_MAX) RADRAY_ABORT("Parameter row index exhausted");
            _values.emplace_back();
            value = static_cast<uint32_t>(_values.size());
        }
        return _values[value - 1];
    }
    size_t CapacityBytes() const noexcept { return _nodes.capacity() * sizeof(Node) + _values.capacity() * sizeof(T); }
    size_t size() const noexcept { return _values.size(); }

private:
    struct Node {
        array<uint32_t, 256> Children{};
    };
    uint32_t AddNode() {
        if (_activeNodes == UINT32_MAX) RADRAY_ABORT("Parameter row radix index exhausted");
        if (_activeNodes == _nodes.size())
            _nodes.emplace_back();
        else
            _nodes[_activeNodes] = {};
        return _activeNodes++;
    }
    vector<Node> _nodes;
    vector<T> _values;
    uint32_t _activeNodes{0};
};
}  // namespace radray::detail
