#pragma once

#include <radray/types.h>

namespace radray {

class Object {
public:
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    virtual ~Object() noexcept = default;

    virtual uint64_t AddRef() = 0;

    virtual uint64_t Release() = 0;
};

}  // namespace radray
