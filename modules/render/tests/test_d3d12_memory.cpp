#include "gpu_test_fixture.h"

#ifdef RADRAY_ENABLE_D3D12
#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

namespace radray::render::d3d12 {
namespace {

class D3D12Memory : public testing::Test {
protected:
    test::DeviceContext Context;

    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::D3D12, Context)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
};

TEST_F(D3D12Memory, DeviceAndResourceMetadataUseConfiguredAllocator) {
    auto* device = static_cast<DeviceD3D12*>(Context.Device.get());
    ASSERT_NE(device->_mainAlloc.Get(), nullptr);
    auto buffer = device->CreateBuffer({.Size = 4096, .Memory = MemoryType::Upload, .Usage = BufferUse::MapWrite | BufferUse::CopySource}).Unwrap();
    ASSERT_TRUE(buffer);
    auto* native = static_cast<BufferD3D12*>(buffer.get());
    ASSERT_NE(native->_alloc.Get(), nullptr);
#ifdef RADRAY_ENABLE_MIMALLOC
    EXPECT_TRUE(mi_any_heap_contains(device->_mainAlloc.Get()));
    EXPECT_TRUE(mi_any_heap_contains(native->_alloc.Get()));
#endif
}

TEST_F(D3D12Memory, DescriptorPagesUseConfiguredAllocatorAcrossPageReuse) {
    auto* device = static_cast<DeviceD3D12*>(Context.Device.get());
    CpuDescriptorAllocator allocator{device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, 0};
    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto first = allocator.Allocate(12);
        ASSERT_TRUE(first);
        CpuDescriptorHeapViewRAII firstView{&allocator, *first};
        const auto second = allocator.Allocate(12);
        ASSERT_TRUE(second);
        CpuDescriptorHeapViewRAII secondView{&allocator, *second};
        EXPECT_NE(first->PagePtr, second->PagePtr);
#ifdef RADRAY_ENABLE_MIMALLOC
        EXPECT_TRUE(mi_any_heap_contains(first->PagePtr->Allocator.Get()));
        EXPECT_TRUE(mi_any_heap_contains(second->PagePtr->Allocator.Get()));
#endif
    }
}

}  // namespace
}  // namespace radray::render::d3d12
#endif
