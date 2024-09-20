#pragma once

#include <dispatch/dispatch.h>

namespace radray::rhi::metal {

class DispatchData {
public:
    DispatchData(const uint8_t* data, size_t count);
    ~DispatchData() noexcept;

public:
    dispatch_data_t data;
};

}  // namespace radray::rhi::metal
