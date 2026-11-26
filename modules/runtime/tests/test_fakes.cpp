#include "test_fakes.h"

namespace radray::test {

gpu::BufferMemoryProperties FakeBuffer::GetMemoryProperties() const noexcept {
    return {Desc.Memory == gpu::MemoryType::DeviceLocal || Desc.Memory == gpu::MemoryType::DeviceUpload,
            Desc.Memory != gpu::MemoryType::DeviceLocal, Desc.Memory != gpu::MemoryType::DeviceLocal,
            Desc.Memory == gpu::MemoryType::HostReadBack};
}

gpu::GpuMemoryBudget FakeDevice::QueryMemoryBudget() const noexcept { return Budget; }

}  // namespace radray::test
