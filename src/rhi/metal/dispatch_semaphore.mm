#include "dispatch_semaphore.h"

#include <stdexcept>

namespace radray::rhi::metal {

Semaphore::Semaphore(intptr_t value)
    : semaphore(dispatch_semaphore_create(value)),
      count{static_cast<int64_t>(value)} {
  if (semaphore == nil) {
    throw std::runtime_error("cannot create semaphore");
  }
}

Semaphore::~Semaphore() noexcept { semaphore = nil; }

void Semaphore::Signal() noexcept {
  dispatch_semaphore_signal(semaphore);
  count++;
}

void Semaphore::Wait() noexcept {
  count--;
  dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
}

} // namespace radray::rhi::metal
