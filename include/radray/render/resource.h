#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Resource : public RenderBase {
public:
    ~Resource() noexcept override = default;

    virtual ResourceType GetType() const noexcept = 0;
    virtual ResourceState GetInitState() const noexcept = 0;
};

class Texture : public Resource {
public:
    ~Texture() noexcept override = default;
};

class RenderTarget : public Texture {
public:
    ~RenderTarget() noexcept override = default;
};

class Buffer : public Resource {
public:
    ~Buffer() noexcept override = default;
};

}  // namespace radray::render
