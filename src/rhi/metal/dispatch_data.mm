#include "dispatch_data.h"

namespace radray::rhi::metal {

DispatchData::DispatchData(const uint8_t *data, size_t count)
    : data(dispatch_data_create(data, count, nullptr,
                                DISPATCH_DATA_DESTRUCTOR_DEFAULT)) {}

DispatchData::~DispatchData() noexcept { data = nil; }

} // namespace radray::rhi::metal
