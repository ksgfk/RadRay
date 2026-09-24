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

TEST_F(FrameUploadTest, TransactionalBufferUploadDoesNotRecordAPrefixOnStagingFailure) {
    // Two requests exceed a standard staging page so failure occurs after the first staged range.
    vector<byte> bytes(5 * 1024 * 1024);
    auto destination = Device.CreateBuffer({.Size = bytes.size() * 2, .Memory = render::MemoryType::Device, .Usage = render::BufferUse::CopyDestination}).Unwrap();
    const array<BufferUploadRequest, 2> requests{{{.SrcData = bytes, .DstBuffer = destination.get()},
                                                  {.SrcData = bytes, .DstBuffer = destination.get(), .DstOffset = bytes.size()}}};
    Device.FailUploadAllocation = 2;
    Uploader.BeginFlight(0, Writes);
    EXPECT_FALSE(Uploader.TryUploadBufferRanges(&Command, requests));
    EXPECT_EQ(Command.Copies, 0u);
    EXPECT_EQ(Command.BarrierCalls, 0u);
    Uploader.EndFlight(0);
    Uploader.CollectFlight(0);
    Writes.Reset();
    Device.FailUploadAllocation = 0;
    Uploader.BeginFlight(0, Writes);
    EXPECT_TRUE(Uploader.TryUploadBufferRanges(&Command, requests));
    EXPECT_EQ(Command.Copies, 2u);
    EXPECT_EQ(Command.BarrierCalls, 2u);
    Uploader.EndFlight(0);
    Uploader.CollectFlight(0);
}

TEST_F(FrameUploadTest, SparseBufferRangesUseOneBarrierPairAndRejectOverlap) {
    array<byte, 64> bytes{};
    auto destination = Device.CreateBuffer({.Size = 12800, .Memory = render::MemoryType::Device, .Usage = render::BufferUse::CopyDestination}).Unwrap();
    array<BufferUploadRequest, 100> requests;
    for (uint32_t i = 0; i < requests.size(); ++i) {
        requests[i] = {.SrcData = bytes, .DstBuffer = destination.get(), .DstOffset = i * 128u,
                       .Before = render::BufferState::ShaderRead, .After = render::BufferState::ShaderRead};
    }
    for (uint32_t flight = 0; flight < 2; ++flight) {
        Uploader.BeginFlight(flight, Writes);
        EXPECT_TRUE(Uploader.TryUploadBufferRanges(&Command, requests));
        EXPECT_EQ(Command.Copies, (flight + 1) * 100u);
        EXPECT_EQ(Command.BarrierCalls, (flight + 1) * 2u);
        Uploader.EndFlight(flight);
        Uploader.CollectFlight(flight);
        Writes.Reset();
    }
    Uploader.BeginFlight(0, Writes);
    requests[1].DstOffset = 0;
    EXPECT_FALSE(Uploader.TryUploadBufferRanges(&Command, requests));
    requests[1].DstOffset = 128;
    requests[1].Before = render::BufferState::Common;
    EXPECT_FALSE(Uploader.TryUploadBufferRanges(&Command, requests));
    EXPECT_EQ(Command.Copies, 200u);
    EXPECT_EQ(Command.BarrierCalls, 4u);
    Uploader.EndFlight(0);
    Uploader.CollectFlight(0);
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
    EXPECT_EQ(draw.Topology, render::PrimitiveTopology::TriangleList);
    EXPECT_EQ(Command.Copies, 2u);
}

}  // namespace
}  // namespace radray
