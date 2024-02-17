#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace radray {

using uint8 = std::uint8_t;
using int8 = std::int8_t;
using int16 = std::int16_t;
using uint16 = std::uint16_t;
using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;

template <typename T>
constexpr auto always_false_v = std::false_type::value;

template <typename T>
struct ArrayTrait;
template <typename T, size_t N>
struct ArrayTrait<T[N]> {
    static constexpr size_t Length = N;
    static constexpr size_t ByteSize = N * sizeof(T);
};
template <typename T>
requires(std::is_bounded_array_v<T>)
constexpr auto array_length_v = ArrayTrait<T>::Length;

}  // namespace radray
