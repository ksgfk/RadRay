#include <gtest/gtest.h>

#include <radray/render/common.h>

using namespace radray;
using namespace radray::render;

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(BufferDescriptor desc) noexcept
        : _desc(std::move(desc)) {}

    bool IsValid() const noexcept override { return true; }

    void Destroy() noexcept override {}

    void* Map(uint64_t offset, uint64_t size) noexcept override {
        (void)offset;
        (void)size;
        return nullptr;
    }

    void Unmap(uint64_t offset, uint64_t size) noexcept override {
        (void)offset;
        (void)size;
    }

    BufferDescriptor GetDesc() const noexcept override { return _desc; }

private:
    BufferDescriptor _desc;
};

class DummyAccelerationStructure final : public AccelerationStructure {
public:
    bool IsValid() const noexcept override { return true; }

    void Destroy() noexcept override {}
};

}  // namespace

TEST(RayTracingValidation, RejectBottomLevelASWithoutScratchUsage) {
    DummyAccelerationStructure as{};
    BufferDescriptor vertexDesc{};
    vertexDesc.Size = 1024;
    vertexDesc.Memory = MemoryType::Device;
    vertexDesc.Usage = BufferUse::Resource;
    DummyBuffer vertexBuf{vertexDesc};

    BufferDescriptor badScratchDesc{};
    badScratchDesc.Size = 1024;
    badScratchDesc.Memory = MemoryType::Device;
    badScratchDesc.Usage = BufferUse::Resource;
    DummyBuffer badScratch{badScratchDesc};

    RayTracingTrianglesDesc tri{};
    tri.VertexBuffer = &vertexBuf;
    tri.VertexStride = 16;
    tri.VertexCount = 3;

    RayTracingGeometryDesc geom{};
    geom.Geometry = tri;

    BuildBottomLevelASDescriptor desc{};
    desc.Target = &as;
    desc.Geometries = std::span<const RayTracingGeometryDesc>(&geom, 1);
    desc.ScratchBuffer = &badScratch;
    desc.ScratchSize = 512;

    EXPECT_FALSE(ValidateBuildBottomLevelASDescriptor(desc));
}

TEST(RayTracingValidation, RejectTopLevelASWithNullBlas) {
    DummyAccelerationStructure tlas{};
    BufferDescriptor scratchDesc{};
    scratchDesc.Size = 1024;
    scratchDesc.Memory = MemoryType::Device;
    scratchDesc.Usage = BufferUse::Scratch;
    DummyBuffer scratch{scratchDesc};

    RayTracingInstanceDesc instance{};
    instance.Blas = nullptr;

    BuildTopLevelASDescriptor desc{};
    desc.Target = &tlas;
    desc.Instances = std::span<const RayTracingInstanceDesc>(&instance, 1);
    desc.ScratchBuffer = &scratch;
    desc.ScratchSize = 512;

    EXPECT_FALSE(ValidateBuildTopLevelASDescriptor(desc));
}

TEST(RayTracingValidation, RejectTraceRaysForInvalidSbtUsageAndAlignment) {
    BufferDescriptor badSbtDesc{};
    badSbtDesc.Size = 4096;
    badSbtDesc.Memory = MemoryType::Device;
    badSbtDesc.Usage = BufferUse::Resource;
    DummyBuffer badSbt{badSbtDesc};

    TraceRaysDescriptor desc{};
    desc.RayGen = ShaderBindingTableRegion{&badSbt, 4, 64, 64};
    desc.Miss = ShaderBindingTableRegion{&badSbt, 128, 64, 64};
    desc.HitGroup = ShaderBindingTableRegion{&badSbt, 256, 64, 64};
    desc.Width = 1;
    desc.Height = 1;
    desc.Depth = 1;

    DeviceDetail detail{};
    detail.ShaderTableAlignment = 64;

    EXPECT_FALSE(ValidateTraceRaysDescriptor(desc, detail));
}

TEST(RayTracingValidation, AcceptMinimalValidTraceRays) {
    BufferDescriptor sbtDesc{};
    sbtDesc.Size = 4096;
    sbtDesc.Memory = MemoryType::Device;
    sbtDesc.Usage = BufferUse::ShaderTable;
    DummyBuffer sbt{sbtDesc};

    TraceRaysDescriptor desc{};
    desc.RayGen = ShaderBindingTableRegion{&sbt, 0, 64, 64};
    desc.Miss = ShaderBindingTableRegion{&sbt, 64, 64, 64};
    desc.HitGroup = ShaderBindingTableRegion{&sbt, 128, 64, 64};
    desc.Width = 8;
    desc.Height = 8;
    desc.Depth = 1;

    DeviceDetail detail{};
    detail.ShaderTableAlignment = 64;

    EXPECT_TRUE(ValidateTraceRaysDescriptor(desc, detail));
}
