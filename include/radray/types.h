#pragma once

#include <cstddef>
#include <cstdint>

namespace radray {

using std::byte;
using std::int8_t;
using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

}  // namespace radray

#define RADRAY_NO_COPY_CTOR(type) \
    type(const type&) = delete;   \
    type& operator=(const type&) = delete;
#define RADRAY_NO_MOVE_CTOR(type) \
    type(type&&) = delete;   \
    type& operator=(type&&) = delete;
