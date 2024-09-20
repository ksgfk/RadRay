#pragma once

#include <span>

#include "metal_helper.h"

namespace radray::rhi::metal {

class Library {
public:
    Library(MTL::Device* device, std::span<const uint8_t> ir, const char* entry);
    ~Library() noexcept;

public:
    MTL::Library* lib;
    MTL::Function* entryPoint;
};

}  // namespace radray::rhi::metal
