#include <gtest/gtest.h>

#include <radray/runtime/gpu_device.h>

#include "test_fakes.h"

namespace radray {
namespace {

TEST(DeviceContextTest, SupportsIndirectRequiresBothIndirectFeatures) {
    test::FakeGpu gpu;
    auto& features = gpu.Device->Capabilities.Features;
    features.IndirectDraw = true;
    EXPECT_FALSE(gpu.Context.SupportsIndirect());
    features.IndirectFirstInstance = true;
    EXPECT_TRUE(gpu.Context.SupportsIndirect());
}

}  // namespace
}  // namespace radray
