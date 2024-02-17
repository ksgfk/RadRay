#include <Eigen/Dense>
#include <radray/types.h>

namespace radray {

template <typename Type, int Size>
class Vector {
    static_assert(always_false_v<Type>, "invalid vector");
};

template <typename Type>
class Vector<Type, 2> {
};

template <typename Type>
class Vector<Type, 3> {
};

template <typename Type>
class Vector<Type, 4> {
};

}  // namespace radray
