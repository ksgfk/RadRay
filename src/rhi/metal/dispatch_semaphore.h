#pragma once

#include <atomic>

#include <dispatch/dispatch.h>

namespace radray::rhi::metal {

class Semaphore {
public:
    explicit Semaphore(intptr_t value);
    ~Semaphore() noexcept;

    void Signal() noexcept;
    void Wait() noexcept;

public:
    dispatch_semaphore_t semaphore;
    std::atomic_int64_t count;
};

}  // namespace radray::rhi::metal
