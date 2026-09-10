#pragma once

#include <span>

#include <radray/logger.h>
#include <radray/types.h>

namespace radray {

/// Reinterprets GPU-layout bytes as the POD generated for that cbuffer. The caller asserts the
/// bytes really are a `T`; there is no runtime layout or type check. The Debug size assertion only
/// guards against writing past the block. Never call this on empty bytes.
template <class T>
T* AsCBuffer(std::span<byte> bytes) noexcept {
#ifdef RADRAY_IS_DEBUG
    RADRAY_ASSERT(bytes.size() == sizeof(T));
#endif
    return reinterpret_cast<T*>(bytes.data());
}

template <class T>
const T* AsCBuffer(std::span<const byte> bytes) noexcept {
#ifdef RADRAY_IS_DEBUG
    RADRAY_ASSERT(bytes.size() == sizeof(T));
#endif
    return reinterpret_cast<const T*>(bytes.data());
}

/// The bytes of a filled cbuffer POD, for handing it to the upload path.
template <class T>
std::span<const byte> AsCBufferBytes(const T& value) noexcept {
    return {reinterpret_cast<const byte*>(&value), sizeof(T)};
}

/// Per-flight rows of one cbuffer POD, packed at `sizeof(T)` with no inter-row padding. The owner
/// freezes the whole table on the game thread and guarantees every row holds the same `T`; the
/// render thread reads rows with `AsCBuffer<T>` and gathers the visible ones into aligned CBV slots.
class PackedCBufferTable {
public:
    /// Keeps the allocation across frames; rows are left uninitialized for the caller to overwrite.
    void Reset(uint32_t stride, size_t rows) {
        _stride = stride;
        _bytes.resize(stride * rows);
    }
    void Clear() noexcept {
        _stride = 0;
        _bytes.clear();
    }
    uint32_t Stride() const noexcept { return _stride; }
    size_t RowCount() const noexcept { return _stride == 0 ? 0 : _bytes.size() / _stride; }

    std::span<byte> Row(size_t index) noexcept {
        RADRAY_ASSERT(index < RowCount());
        return {_bytes.data() + index * _stride, _stride};
    }
    std::span<const byte> Row(size_t index) const noexcept {
        RADRAY_ASSERT(index < RowCount());
        return {_bytes.data() + index * _stride, _stride};
    }

private:
    uint32_t _stride{0};
    vector<byte> _bytes;
};

}  // namespace radray
