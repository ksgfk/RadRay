#pragma once

#include <algorithm>
#include <initializer_list>
#include <span>

#include <radray/types.h>

namespace radray {

/// Contiguous sequence with `N` elements of inline storage; spills to the heap only beyond `N`.
/// Iterators and references are invalidated by any insertion, as with `vector`.
/// `T` must be default constructible; cleared inline slots are reset to `T{}`.
template <class T, size_t N>
class InlineVector {
public:
    static_assert(N > 0, "InlineVector needs at least one inline slot");
    using value_type = T;
    using size_type = size_t;
    using iterator = T*;
    using const_iterator = const T*;
    static constexpr size_t inline_capacity = N;

    InlineVector() noexcept = default;
    InlineVector(std::initializer_list<T> init) { assign(init.begin(), init.end()); }
    InlineVector(const InlineVector&) = default;
    InlineVector& operator=(const InlineVector&) = default;
    InlineVector(InlineVector&& other) noexcept
        : _local(std::move(other._local)), _heap(std::move(other._heap)), _size(other._size) {
        other._size = 0;
    }
    InlineVector& operator=(InlineVector&& other) noexcept {
        if (this != &other) {
            _local = std::move(other._local);
            _heap = std::move(other._heap);
            _size = other._size;
            other._size = 0;
        }
        return *this;
    }
    InlineVector& operator=(std::initializer_list<T> init) {
        assign(init.begin(), init.end());
        return *this;
    }

    template <class It>
    void assign(It first, It last) {
        clear();
        for (; first != last; ++first) emplace_back(*first);
    }

    size_t size() const noexcept { return _size; }
    bool empty() const noexcept { return _size == 0; }
    T* data() noexcept { return _size > N ? _heap.data() : _local.data(); }
    const T* data() const noexcept { return _size > N ? _heap.data() : _local.data(); }
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + _size; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + _size; }
    T& operator[](size_t index) noexcept { return data()[index]; }
    const T& operator[](size_t index) const noexcept { return data()[index]; }
    T& front() noexcept { return data()[0]; }
    const T& front() const noexcept { return data()[0]; }
    T& back() noexcept { return data()[_size - 1]; }
    const T& back() const noexcept { return data()[_size - 1]; }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        // Build first so an argument aliasing one of our own elements survives a spill.
        T value(std::forward<Args>(args)...);
        if (_size < N) {
            _local[_size] = std::move(value);
        } else {
            if (_size == N) {
                _heap.reserve(N * 2);
                for (T& slot : _local) {
                    _heap.push_back(std::move(slot));
                    slot = T{};
                }
            }
            _heap.push_back(std::move(value));
        }
        ++_size;
        return back();
    }
    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    void pop_back() noexcept {
        --_size;
        if (_size > N) {
            _heap.pop_back();
        } else if (_size == N) {
            std::move(_heap.begin(), _heap.begin() + static_cast<std::ptrdiff_t>(N), _local.begin());
            _heap.clear();
        } else {
            _local[_size] = T{};
        }
    }

    void clear() noexcept {
        for (size_t i = 0, n = std::min(_size, N); i < n; ++i) _local[i] = T{};
        _heap.clear();
        _size = 0;
    }

    friend bool operator==(const InlineVector& a, const InlineVector& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

private:
    array<T, N> _local{};
    vector<T> _heap;
    size_t _size{0};
};

}  // namespace radray
