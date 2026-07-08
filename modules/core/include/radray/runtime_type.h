#pragma once

#include <tuple>
#include <type_traits>

#include <radray/guid.h>

namespace radray {

/// 进程内运行时类型标识。由类型手写固定 Guid,不依赖 C++ RTTI。
/// 只表达精确类型身份;不提供反射或注册表。
///
/// 【继承关系】通过 RuntimeTypeTrait<T>::Bases(直接基类列表,std::tuple<...>)可选声明。
/// 消费方(如 ServiceRegistry)在【拥有确切静态类型】的上下文里用 static_cast 完成
/// 指针偏移修正 —— 运行期只有 Guid 算不出多继承的基类子对象偏移。默认无基类。
using RuntimeTypeId = Guid;

template <class T>
struct RuntimeTypeTrait {
    static constexpr RuntimeTypeId value = Guid::Empty();
    using Bases = std::tuple<>;
};

template <class T>
inline constexpr RuntimeTypeId runtime_type_id_v = []() constexpr {
    constexpr RuntimeTypeId id = RuntimeTypeTrait<T>::value;
    static_assert(!id.IsEmpty(), "RuntimeTypeTrait<T>::value must be specialized with a fixed non-empty Guid.");
    return id;
}();

namespace detail {

template <class T, class = void>
struct RuntimeTypeBasesOf {
    using type = std::tuple<>;
};

template <class T>
struct RuntimeTypeBasesOf<T, std::void_t<typename RuntimeTypeTrait<T>::Bases>> {
    using type = typename RuntimeTypeTrait<T>::Bases;
};

}  // namespace detail

/// T 的直接基类列表(std::tuple<...>)。未声明 Bases 的类型退化为空 tuple。
template <class T>
using runtime_type_bases_t = typename detail::RuntimeTypeBasesOf<T>::type;

namespace detail {

/// 编译期沿 Bases 图递归判断 Derived 是否可达 Base(含 Derived == Base)。
/// 只依赖 RuntimeTypeTrait 声明的关系,不依赖 C++ 继承 —— 故与运行期 Guid 查询同源。
template <class Derived, class Base>
struct RuntimeIsA;

template <class Derived, class Base, class Bases>
struct RuntimeIsAAny;

template <class Derived, class Base, class... Direct>
struct RuntimeIsAAny<Derived, Base, std::tuple<Direct...>>
    : std::bool_constant<(RuntimeIsA<Direct, Base>::value || ...)> {};

template <class Derived, class Base>
struct RuntimeIsA
    : std::bool_constant<
          std::is_same_v<Derived, Base> ||
          RuntimeIsAAny<Derived, Base, runtime_type_bases_t<Derived>>::value> {};

}  // namespace detail

/// 编译期 is-a 查询:Derived 是否是 Base(或与之相同),沿 RuntimeTypeTrait::Bases 图判定。
/// 【注意】只回答关系,不做指针修正 —— 多继承下的基类子对象偏移需在拥有确切静态类型的
/// 上下文里用 static_cast 完成(见 ServiceRegistry)。
template <class Derived, class Base>
inline constexpr bool runtime_is_a_v = detail::RuntimeIsA<Derived, Base>::value;

}  // namespace radray
