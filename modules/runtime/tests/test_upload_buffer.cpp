#include <gtest/gtest.h>

#include <limits>

#include <radray/runtime/upload_buffer.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::render;

namespace {

class MappedBuffer final : public test::FakeBuffer {
public:
    explicit MappedBuffer(const BufferDescriptor& desc) : FakeBuffer(desc.Size, desc.Usage), Bytes(desc.Size) { Desc = desc; }
    void* Map(uint64_t offset, uint64_t) noexcept override { return Bytes.data() + offset; }
    vector<byte> Bytes;
};

class UploadDevice final : public test::FakeDevice {
public:
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        if (FailAllocations) return nullptr;
        return make_unique<MappedBuffer>(desc);
    }
    bool FailAllocations{false};
};

TEST(MappedUploadBuffer, SupportsCustomUsageAndTransactionalGrowth) {
    UploadDevice device;
    for (const auto usage : {BufferUse::CopySource, BufferUse::Vertex, BufferUse::CBuffer}) {
        auto upload = MappedUploadBuffer::Create(device, 24, usage);
        ASSERT_TRUE(upload);
        const auto desc = upload->Get()->GetDesc();
        EXPECT_EQ(desc.Memory, MemoryType::Upload);
        EXPECT_TRUE(desc.Usage.HasFlag(usage | BufferUse::MapWrite));
        EXPECT_TRUE(desc.Hints.HasFlag(ResourceHint::PersistentMap));
        auto* original = upload->Get();
        upload->GetData()[0] = byte{42};
        device.FailAllocations = true;
        EXPECT_FALSE(upload->Reserve(device, 40));
        EXPECT_EQ(upload->Get(), original);
        EXPECT_EQ(upload->GetData()[0], byte{42});
        device.FailAllocations = false;
        ASSERT_TRUE(upload->Reserve(device, 40));
        EXPECT_EQ(upload->GetCapacity(), 64u);
        EXPECT_TRUE(upload->Get()->GetDesc().Usage.HasFlag(usage));
        EXPECT_FALSE(upload->Reserve(device, std::numeric_limits<uint64_t>::max()));
    }
    EXPECT_FALSE(MappedUploadBuffer::Create(device, 0));
}

TEST(BatchedBufferUpload, RejectsInvalidRangesAlignmentAndPostCommitWrites) {
    UploadDevice device;
    test::FakeBuffer destination{64, BufferUse::CopyDestination};
    auto upload = BatchedBufferUpload::Create(device, 16);
    ASSERT_TRUE(upload);
    EXPECT_FALSE(upload->Acquire(&destination, 65, 1));
    EXPECT_FALSE(upload->Acquire(&destination, 60, 8));
    EXPECT_FALSE(upload->Acquire(&destination, 0, 0));
    EXPECT_FALSE(upload->Acquire(&destination, 0, 8, 3));
    EXPECT_FALSE(upload->Acquire(&destination, 0, 8, 0));
    ObjectArena arena;
    RDGBuilder graph{arena};
    EXPECT_TRUE(upload->CommitToGraph(graph));
    EXPECT_FALSE(upload->Acquire(&destination, 0, 8));
    upload->Reset();
    EXPECT_TRUE(upload->Acquire(&destination, 0, 8));
}

}  // 匿名命名空间
