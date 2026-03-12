#pragma once

#include <radray/types.h>
#include <radray/nullable.h>

namespace radray {

class GpuRuntime;
class GpuSubmissionContext;

class GpuSubmissionContext {
public:
private:
    GpuRuntime* _service{nullptr};

    friend class GpuRuntime;
};

class GpuPresentSurface {
public:
};

class GpuPresentImage {
public:
};

class GpuRuntime {
public:
    bool IsValid() const noexcept;

    void Destroy() noexcept;

    unique_ptr<GpuSubmissionContext> BeginSubmission() noexcept;

    void Submit(unique_ptr<GpuSubmissionContext>&& frame) noexcept;
};

}  // namespace radray
