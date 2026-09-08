#pragma once

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

#include <radray/logger.h>
#include <radray/types.h>

namespace radray {

/// Contiguous sequence with storage for N inline elements; only [begin(), end()) is constructed.
/// Growth invalidates all iterators and references. clear()/pop_back() retain capacity.
/// Moving leaves the source empty and reusable. T need not be default constructible or assignable.
template <class T, size_t N>
class InlineVector {
public:
    static_assert(N > 0, "InlineVector needs at least one inline slot");
    static_assert(N <= static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T));
    using value_type = T;
    using size_type = size_t;
    using iterator = T*;
    using const_iterator = const T*;
    static constexpr size_t inline_capacity = N;

    InlineVector() noexcept = default;
    InlineVector(std::initializer_list<T> init) : InlineVector() { AssignUnaliased(init.begin(), init.end()); }
    InlineVector(const InlineVector& other)
    requires std::is_copy_constructible_v<T>
        : InlineVector() { AssignUnaliased(other.begin(), other.end()); }
    InlineVector(InlineVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : InlineVector() { MoveFrom(other); }
    ~InlineVector() noexcept { ReleaseStorage(); }

    InlineVector& operator=(const InlineVector& other)
    requires std::is_copy_constructible_v<T>
    {
        if (this != &other) AssignUnaliased(other.begin(), other.end());
        return *this;
    }
    InlineVector& operator=(InlineVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            ReleaseStorage();
            MoveFrom(other);
        }
        return *this;
    }
    InlineVector& operator=(std::initializer_list<T> init) {
        AssignUnaliased(init.begin(), init.end());
        return *this;
    }

    template <std::input_iterator It>
    void assign(It first, It last) {
        // A range may refer to our own elements, including through iterator adaptors.
        InlineVector replacement;
        replacement.AssignUnaliased(first, last);
        *this = std::move(replacement);
    }

    size_t size() const noexcept { return _size; }
    size_t capacity() const noexcept { return _capacity; }
    static constexpr size_t max_size() noexcept {
        return static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T);
    }
    bool empty() const noexcept { return _size == 0; }
    T* data() noexcept { return _data; }
    const T* data() const noexcept { return _data; }
    iterator begin() noexcept { return _data; }
    iterator end() noexcept { return _data + _size; }
    const_iterator begin() const noexcept { return _data; }
    const_iterator end() const noexcept { return _data + _size; }
    T& operator[](size_t index) noexcept {
        RADRAY_ASSERT(index < _size);
        return _data[index];
    }
    const T& operator[](size_t index) const noexcept {
        RADRAY_ASSERT(index < _size);
        return _data[index];
    }
    T& front() noexcept { return (*this)[0]; }
    const T& front() const noexcept { return (*this)[0]; }
    T& back() noexcept { return (*this)[_size - 1]; }
    const T& back() const noexcept { return (*this)[_size - 1]; }

    void reserve(size_t capacity) {
        if (capacity <= _capacity) return;
        CheckCapacity(capacity);
        PendingStorage storage{capacity};
        RelocateTo(storage);
        AdoptStorage(storage, _size);
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (_size == _capacity) return GrowAndEmplaceBack(std::forward<Args>(args)...);
        T* value = std::construct_at(_data + _size, std::forward<Args>(args)...);
        ++_size;
        return *value;
    }
    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    void pop_back() noexcept {
        RADRAY_ASSERT(!empty());
        --_size;
        DestroyElements(_data + _size, 1);
    }

    void clear() noexcept {
        DestroyElements(_data, _size);
        _size = 0;
    }

    friend bool operator==(const InlineVector& a, const InlineVector& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    }

private:
    static void DestroyElements(T* data, size_t count) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) std::destroy_n(data, count);
    }

    struct PendingStorage {
        explicit PendingStorage(size_t capacity)
            : Data(allocator<T>{}.allocate(capacity)), Capacity(capacity) {}
        ~PendingStorage() noexcept {
            if (!Owned) return;
            DestroyElements(Data, PrefixSize);
            if (TailConstructed) DestroyElements(Data + TailIndex, 1);
            allocator<T>{}.deallocate(Data, Capacity);
        }
        PendingStorage(const PendingStorage&) = delete;
        PendingStorage& operator=(const PendingStorage&) = delete;

        T* Data;
        size_t Capacity;
        size_t PrefixSize{0}, TailIndex{0};
        bool TailConstructed{false}, Owned{true};
    };

    static void CheckCapacity(size_t capacity) {
        if (capacity > max_size()) RADRAY_ABORT("InlineVector capacity exceeds max_size");
    }

    T* InlineData() noexcept { return reinterpret_cast<T*>(_inlineStorage); }
    const T* InlineData() const noexcept { return reinterpret_cast<const T*>(_inlineStorage); }
    bool IsInline() const noexcept { return _data == InlineData(); }

    void ReleaseStorage() noexcept {
        clear();
        if (!IsInline()) allocator<T>{}.deallocate(_data, _capacity);
        _data = InlineData();
        _capacity = N;
    }

    void RelocateTo(PendingStorage& storage) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(storage.Data, _data, _size * sizeof(T));
            storage.PrefixSize = _size;
        } else {
            for (; storage.PrefixSize < _size; ++storage.PrefixSize)
                std::construct_at(storage.Data + storage.PrefixSize, std::move_if_noexcept(_data[storage.PrefixSize]));
        }
    }

    void AdoptStorage(PendingStorage& storage, size_t size) noexcept {
        ReleaseStorage();
        _data = storage.Data;
        _capacity = storage.Capacity;
        _size = size;
        storage.Owned = false;
    }

    template <class... Args>
    T& GrowAndEmplaceBack(Args&&... args) {
        if (_size == max_size()) RADRAY_ABORT("InlineVector capacity exceeds max_size");
        const size_t capacity = _capacity > max_size() / 2 ? max_size() : _capacity * 2;
        PendingStorage storage{capacity};
        // Construct before relocation: arguments can refer to an element or one of its subobjects.
        std::construct_at(storage.Data + _size, std::forward<Args>(args)...);
        storage.TailIndex = _size;
        storage.TailConstructed = true;
        RelocateTo(storage);
        AdoptStorage(storage, _size + 1);
        return back();
    }

    template <std::input_iterator It>
    void AssignUnaliased(It first, It last) {
        clear();
        if constexpr (std::forward_iterator<It>) reserve(static_cast<size_t>(std::distance(first, last)));
        for (; first != last; ++first) emplace_back(*first);
    }

    void MoveFrom(InlineVector& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (!other.IsInline()) {
            _data = std::exchange(other._data, other.InlineData());
            _size = std::exchange(other._size, 0);
            _capacity = std::exchange(other._capacity, N);
        } else {
            std::uninitialized_move_n(other._data, other._size, _data);
            _size = other._size;
            other.clear();
        }
    }

    T* _data{InlineData()};
    size_t _size{0}, _capacity{N};
    alignas(T) byte _inlineStorage[sizeof(T) * N];
};

}  // namespace radray
