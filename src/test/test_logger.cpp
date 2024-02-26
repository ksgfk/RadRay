#include <radray/logger.h>
#include <radray/types.h>

struct E {
    void A(int c) {
    }
};

void AAA(int a) {
    RADRAY_LOG_DEBUG("AAA");
}

template <typename T>
requires(std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>)
void b(T t) {
    RADRAY_LOG_DEBUG("is_function_v {}", std::is_function_v<decltype(t)>);
    RADRAY_LOG_DEBUG("is_pointer_v {}", std::is_pointer_v<decltype(t)>);
    RADRAY_LOG_DEBUG("is_reference_v {}", std::is_reference_v<decltype(t)>);
}

template <typename T>
void a(T&& t) {
    using RawType = radray::remove_all_cvref_t<T>;
    RADRAY_LOG_DEBUG("input type {}", typeid(T).name());
    RADRAY_LOG_DEBUG("raw type {}", typeid(RawType).name());
    if constexpr (std::is_function_v<RawType>) {
        b(t);
    } else {
        RADRAY_LOG_DEBUG("no match {}", typeid(t).name());
    }
}

int main() {
    a(AAA);
    RADRAY_LOG_DEBUG("---------------------");
    auto v = AAA;
    a(v);
    RADRAY_LOG_DEBUG("---------------------");
    a([&](int t) {
        v(t);
    });
    RADRAY_LOG_DEBUG("---------------------");
    a(&E::A);
    RADRAY_LOG_DEBUG("---------------------");
    E e{};
    auto t = std::bind(&E::A, e, std::placeholders::_1);
    a(t);
    RADRAY_LOG_DEBUG("---------------------");
    return 0;
}
