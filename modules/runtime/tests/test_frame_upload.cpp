#include "upload_test_support.h"

#include <gtest/gtest.h>

namespace radray {
namespace {

class FrameUploadTest : public testing::Test {
protected:
    test::UploadTestDevice Device;
    ResourceUploader Uploader{&Device, 2};
    HostWriteBatch Writes;
    test::UploadTestCommand Command;
};

TEST_F(FrameUploadTest, AllocationFailureLeavesNoRecordedMeshCopies) {
    for (int failure = 1; failure <= 2; ++failure) {
        Device.DeviceAllocations = 0;
        Device.FailDeviceAllocation = failure;
        Uploader.BeginFlight(0, Writes);
        auto mesh = Uploader.UploadMeshResource(&Command, test::MakeUploadTestMesh());
        Uploader.EndFlight(0);
        EXPECT_FALSE(mesh);
        EXPECT_EQ(Command.Copies, 0u);
        EXPECT_EQ(Device.LiveDeviceBuffers, 0);
    }
}

TEST_F(FrameUploadTest, InvalidMeshAttributesFailBeforeAllocatingOrRecording) {
    for (uint32_t invalidCase = 0; invalidCase < 8; ++invalidCase) {
        SCOPED_TRACE(invalidCase);
        auto source = test::MakeUploadTestMesh();
        auto& attributes = source.Primitives[0].VertexBuffers;
        switch (invalidCase) {
            case 0: attributes[0].Stride = 0; break;
            case 1: attributes[0].Offset = 8; break;
            case 2: attributes[0].Semantic.clear(); break;
            case 3: attributes[0].ComponentCount = 5; break;
            case 4: attributes[0].Type = static_cast<VertexDataType>(255); break;
            case 5: attributes.push_back(attributes.front()); break;
            case 6:
                attributes.push_back(attributes.front());
                attributes.back().Semantic = "NORMAL";
                attributes.back().BufferIndex = 1;
                break;
            case 7: attributes.clear(); break;
        }
        Uploader.BeginFlight(0, Writes);
        auto mesh = Uploader.UploadMeshResource(&Command, source);
        Uploader.EndFlight(0);
        EXPECT_FALSE(mesh);
        EXPECT_EQ(Device.DeviceAllocations, 0);
        EXPECT_EQ(Command.Copies, 0u);
    }
}

TEST_F(FrameUploadTest, MeshUploadPreservesVertexBindingAndIndexView) {
    auto source = test::MakeUploadTestMesh();
    std::swap(source.Bins[0], source.Bins[1]);
    source.Primitives[0].VertexBuffers[0].BufferIndex = 1;
    source.Primitives[0].IndexBuffer.BufferIndex = 0;
    Uploader.BeginFlight(0, Writes);
    auto mesh = Uploader.UploadMeshResource(&Command, source);
    Uploader.EndFlight(0);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->Buffers.size(), 2u);
    ASSERT_EQ(mesh->Draws.size(), 1u);
    const auto& draw = mesh->Draws.front();
    ASSERT_EQ(draw.VertexBuffers.size(), 1u);
    EXPECT_EQ(draw.VertexBuffers.front().Binding, 0u);
    EXPECT_EQ(draw.VertexBuffers.front().View.Target, mesh->Buffers[1].get());
    EXPECT_EQ(draw.VertexBuffers.front().View.Offset, 0u);
    EXPECT_EQ(draw.VertexBuffers.front().View.Size, 36u);
    EXPECT_EQ(draw.Ibv.Target, mesh->Buffers[0].get());
    EXPECT_EQ(draw.Ibv.Offset, 0u);
    EXPECT_EQ(draw.Ibv.Stride, 4u);
    EXPECT_EQ(draw.Topology, PrimitiveTopology::TriangleList);
    EXPECT_EQ(Command.Copies, 2u);
}

}  // namespace
}  // namespace radray
