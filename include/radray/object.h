#pragma once

#include <memory>

namespace radray {

class Object : public std::enable_shared_from_this<Object> {
public:
    Object() noexcept = default;
    virtual ~Object() noexcept = default;

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
};

}  // namespace radray
