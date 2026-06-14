#pragma once

#include <radray/guid.h>

namespace radray {

/// 进程内运行时类型标识。由类型手写固定 Guid,不依赖 C++ RTTI。
/// 只表达精确类型身份,不提供反射、继承关系查询或注册表。
using RuntimeTypeId = Guid;

template <class T>
struct RuntimeTypeTrait {
    static constexpr RuntimeTypeId value = Guid::Empty();
};

template <class T>
inline constexpr RuntimeTypeId runtime_type_id_v = []() constexpr {
    constexpr RuntimeTypeId id = RuntimeTypeTrait<T>::value;
    static_assert(!id.IsEmpty(), "RuntimeTypeTrait<T>::value must be specialized with a fixed non-empty Guid.");
    return id;
}();

}  // namespace radray
