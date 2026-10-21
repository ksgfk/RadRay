#pragma once

#include <type_traits>

#include <radray/guid.h>

namespace radray {

/// 独立、稳定的类型标识。Guid 不参与 C++ 对象查询，也不与 RTTI 建立全局映射。
using RuntimeTypeId = Guid;

template <class T>
struct RuntimeTypeTrait {
    static constexpr RuntimeTypeId value = Guid::Empty();
};

template <class T>
inline constexpr RuntimeTypeId runtime_type_id_v = []() constexpr {
    using Type = std::remove_cvref_t<T>;
    constexpr RuntimeTypeId id = RuntimeTypeTrait<Type>::value;
    static_assert(!id.IsEmpty(), "RuntimeTypeTrait<T>::value must be specialized with a fixed non-empty Guid.");
    return id;
}();

}  // namespace radray
