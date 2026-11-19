#include <gtest/gtest.h>

#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/mesh_draw_command.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::gpu;

namespace {

class PoolDevice final : public test::FakeDevice {
public:
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        if (FailCreate) return nullptr;
        auto buffer = make_unique<test::FakeBuffer>(desc.Size, desc.Usage);
        buffer->Desc = desc;
        return buffer;
    }
    bool FailCreate{false};
};

TEST(GeometryBufferPool, VertexPagesKeepInputsStableShareBindingsAndRecycleRanges) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}, VertexBufferLayout{1, 16, VertexStepMode::Instance}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3},
                           VertexInputAttribute{1, 0, "TEXCOORD", 0, VertexFormat::FLOAT32X4}};
    auto pool = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    ASSERT_TRUE(pool);
    auto a = pool->Allocate(3);
    auto b = pool->Allocate(5);
    ASSERT_TRUE(a && b);
    const auto* input = &a->GetVertexInput();
    EXPECT_EQ(input, &b->GetVertexInput());
    EXPECT_EQ(a->GetGpuBuffer(), b->GetGpuBuffer());
    EXPECT_EQ(a->GetGpuBuffer()->GetBuffer(), a->GetBuffer());
    EXPECT_TRUE(ValidateVertexInput(*input, true));
    EXPECT_FALSE(ValidateVertexInput(*input, false));
    EXPECT_EQ(b->GetFirstVertex(), 3u);
    EXPECT_EQ(b->GetByteOffset(), 36u);
    auto c = pool->Allocate(9);
    ASSERT_TRUE(c);
    EXPECT_EQ(&a->GetVertexInput(), input);
    EXPECT_NE(c->GetBuffer(), a->GetBuffer());
    a.reset();
    auto reused = pool->Allocate(3);
    ASSERT_TRUE(reused);
    EXPECT_EQ(&reused->GetVertexInput(), input);
    EXPECT_EQ(reused->GetFirstVertex(), 0u);
}

template <class Pool>
void CheckSparsePageAccounting(Pool& pool, PoolDevice& device) {
    EXPECT_FALSE(pool.Allocate(0));
    device.FailCreate = true;
    EXPECT_FALSE(pool.Allocate(1));
    EXPECT_EQ(pool.GetPageCount(), 0u);
    EXPECT_EQ(pool.GetCapacityBytes(), 0u);
    EXPECT_EQ(pool.GetFreeBytes(), 0u);
    EXPECT_EQ(pool.GetAllocatedBytes(), 0u);
    device.FailCreate = false;

    auto first = pool.Allocate(8);
    auto middle = pool.Allocate(8);
    auto last = pool.Allocate(4);
    ASSERT_TRUE(first && middle && last);
    auto* lastBuffer = last->GetBuffer();
    auto oldBuffer = middle->GetGpuBuffer();
    middle.reset();
    EXPECT_EQ(oldBuffer->UseCount(), 2u);
    EXPECT_EQ(pool.GetPageCount(), 3u);
    EXPECT_EQ(pool.GetCapacityBytes(), 96u);
    EXPECT_EQ(pool.GetFreeBytes(), 48u);
    EXPECT_EQ(pool.GetAllocatedBytes(), 48u);
    EXPECT_EQ(pool.TrimEmptyPages(), 1u);
    EXPECT_EQ(pool.TrimEmptyPages(), 0u);
    EXPECT_EQ(oldBuffer->UseCount(), 1u);

    device.FailCreate = true;
    EXPECT_FALSE(pool.Allocate(12));
    device.FailCreate = false;
    EXPECT_EQ(pool.GetPageCount(), 2u);
    EXPECT_EQ(pool.GetCapacityBytes(), 64u);
    EXPECT_EQ(pool.GetFreeBytes(), 16u);
    EXPECT_EQ(pool.GetAllocatedBytes(), 48u);
    EXPECT_EQ(last->GetBuffer(), lastBuffer);
    EXPECT_EQ(last->GetPageIndex(), 2u);
    EXPECT_TRUE(oldBuffer->GetBuffer()->IsValid());

    auto tail = pool.Allocate(4);
    ASSERT_TRUE(tail);
    EXPECT_EQ(tail->GetPageIndex(), 2u);
    auto large = pool.Allocate(12);
    ASSERT_TRUE(large);
    EXPECT_EQ(large->GetPageIndex(), 1u);
    EXPECT_NE(large->GetBuffer(), oldBuffer->GetBuffer());
    EXPECT_EQ(pool.GetCapacityBytes(), 112u);
    EXPECT_EQ(pool.GetAllocatedBytes(), 112u);
    EXPECT_EQ(pool.GetPageCount(), 3u);
    large.reset();
    EXPECT_EQ(pool.TrimEmptyPages(), 1u);
    EXPECT_EQ(pool.GetPageCount(), 2u);
    EXPECT_EQ(last->GetBuffer(), lastBuffer);

    using Handle = typename decltype(last)::value_type;
    Handle moved{std::move(*last)};
    EXPECT_EQ(last->GetPageIndex(), 0u);
    EXPECT_EQ(moved.GetPageIndex(), 2u);
    *last = std::move(moved);
    EXPECT_EQ(moved.GetPageIndex(), 0u);
    EXPECT_EQ(last->GetPageIndex(), 2u);
    *last = Handle{};
    EXPECT_FALSE(*last);
    EXPECT_EQ(last->GetPageIndex(), 0u);
    EXPECT_EQ(Handle{}.GetPageIndex(), 0u);
}

TEST(GeometryBufferPool, SparseSlotsPreserveLivePagesAndCommittedBytes) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 4, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32}};
    auto vertices = VertexBufferPool::Create(device, {.VertexStride = 4, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    auto indices = IndexBufferPool::Create(device, {.IndexStride = 4, .PageIndexCapacity = 8});
    ASSERT_TRUE(vertices && indices);
    CheckSparsePageAccounting(*vertices, device);
    CheckSparsePageAccounting(*indices, device);
}

TEST(GeometryBufferPool, RecommittedVertexPageBindsItsNewBuffer) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{3, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{3, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
    auto pool = VertexBufferPool::Create(device, {.VertexStride = 12, .VertexStreamBinding = 3, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    ASSERT_TRUE(pool);
    auto first = pool->Allocate(8);
    auto middle = pool->Allocate(8);
    auto last = pool->Allocate(8);
    ASSERT_TRUE(first && middle && last);
    auto old = middle->GetGpuBuffer();
    middle.reset();
    ASSERT_EQ(pool->TrimEmptyPages(), 1u);
    auto replacement = pool->Allocate(5);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->GetPageIndex(), 1u);
    EXPECT_NE(old->GetBuffer(), replacement->GetBuffer());
    const auto& input = replacement->GetVertexInput();
    ASSERT_TRUE(ValidateVertexInput(input, true));
    ASSERT_EQ(input.Streams.size(), 1u);
    EXPECT_EQ(input.Streams[0].Buffer.Get(), replacement->GetBuffer());
    EXPECT_EQ(input.Streams[0].Size, 96u);
    EXPECT_EQ(input.Streams[0].Binding, 3u);
    first.reset();
    last.reset();
    replacement.reset();
    EXPECT_EQ(pool->TrimEmptyPages(), 3u);
    EXPECT_EQ(pool->GetCapacityBytes(), 0u);
    auto fresh = pool->Allocate(1);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->GetPageIndex(), 0u);
}

TEST(GeometryBufferPool, RejectsInvalidLayoutsAndIndexStrides) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}, VertexBufferLayout{1, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 12, .Layouts = layouts, .Attributes = attributes}));
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 16, .Layouts = std::span{layouts}.first(1), .Attributes = attributes}));
    const array threeLayouts{layouts[0], VertexBufferLayout{1, 16, VertexStepMode::Instance}, VertexBufferLayout{2, 16, VertexStepMode::Instance}};
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 12, .Layouts = threeLayouts, .Attributes = attributes}));
    EXPECT_FALSE(IndexBufferPool::Create(device, {.IndexStride = 3}));
    EXPECT_FALSE(IndexBufferPool::Create(device, {.PageIndexCapacity = 0}));
}

TEST(GeometryBufferPool, GeometryDoesNotRequireShaderLocations) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 28, VertexStepMode::Vertex}};
    array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3},
                     VertexInputAttribute{0, 12, "COLOR", 0, VertexFormat::FLOAT32X4}};
    auto pool = VertexBufferPool::Create(device, {.VertexStride = 28, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    ASSERT_TRUE(pool);
    auto allocation = pool->Allocate(3);
    ASSERT_TRUE(allocation);
    EXPECT_TRUE(ValidateVertexInput(allocation->GetVertexInput()));
    attributes[1].Semantic = "POSITION";
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 28, .Layouts = layouts, .Attributes = attributes}));
}

TEST(GeometryBufferPool, IndexAllocationsExposeSharedViewsAndMoveOwnership) {
    PoolDevice device;
    for (const auto stride : {2u, 4u}) {
        auto pool = IndexBufferPool::Create(device, {.IndexStride = stride, .PageIndexCapacity = 8});
        ASSERT_TRUE(pool);
        auto a = pool->Allocate(2);
        auto b = pool->Allocate(6);
        ASSERT_TRUE(a && b);
        EXPECT_EQ(b->GetFirstIndex(), 2u);
        EXPECT_EQ(b->GetByteOffset(), 2u * stride);
        EXPECT_EQ(b->GetByteSize(), 6u * stride);
        EXPECT_EQ(a->GetIndexBufferView().Target, b->GetIndexBufferView().Target);
        EXPECT_EQ(a->GetGpuBuffer(), b->GetGpuBuffer());
        EXPECT_EQ(a->GetGpuBuffer()->GetBuffer(), a->GetBuffer());
        EXPECT_EQ(b->GetIndexBufferView().Offset, 0u);
        auto moved = std::move(*b);
        b.reset();
        EXPECT_EQ(moved.GetIndexCount(), 6u);
        *a = std::move(moved);
        auto reused = pool->Allocate(2);
        ASSERT_TRUE(reused);
        EXPECT_EQ(reused->GetFirstIndex(), 0u);
    }
}

TEST(GeometryBufferPool, PageStrongReferenceSurvivesPoolAfterSuballocationsAreReleased) {
    PoolDevice device;
    auto pool = IndexBufferPool::Create(device, {.PageIndexCapacity = 8, .AdditionalUsage = BufferUse::Common}).Release();
    ASSERT_TRUE(pool);
    auto allocation = pool->Allocate(3);
    ASSERT_TRUE(allocation);
    auto owner = allocation->GetGpuBuffer();
    auto* native = allocation->GetBuffer();
    allocation.reset();
    pool.reset();
    EXPECT_EQ(owner->UseCount(), 1u);
    EXPECT_EQ(owner->GetBuffer(), native);
    EXPECT_EQ(native->GetDesc().Size, 32u);
}

TEST(GeometryBufferPool, GetFreeBytesTracksMultiPageAllocationsCrossPageFreesAndReallocations) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3}};
    auto vbPool = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    auto ibPool = IndexBufferPool::Create(device, {.IndexStride = 2, .PageIndexCapacity = 10});
    ASSERT_TRUE(vbPool && ibPool);

    EXPECT_EQ(vbPool->GetFreeBytes(), 0u);
    EXPECT_EQ(ibPool->GetFreeBytes(), 0u);

    // 跨多页分配（含超出单页容量的超大页）。
    auto v0 = vbPool->Allocate(5);   // page 0 (cap 8): free 3
    auto v1 = vbPool->Allocate(6);   // page 1 (cap 8): free 2
    auto v2 = vbPool->Allocate(12);  // page 2 (cap 12): free 0
    ASSERT_TRUE(v0 && v1 && v2);
    EXPECT_EQ(vbPool->GetPageCount(), 3u);
    EXPECT_EQ(vbPool->GetFreeBytes(), (3u + 2u + 0u) * 12u);

    // 跨页释放与重分配。
    v0.reset();  // page 0: free 8
    EXPECT_EQ(vbPool->GetFreeBytes(), (8u + 2u + 0u) * 12u);
    v2.reset();  // page 2: free 12
    EXPECT_EQ(vbPool->GetFreeBytes(), (8u + 2u + 12u) * 12u);
    auto vReused = vbPool->Allocate(4);  // 命中已有空闲页
    ASSERT_TRUE(vReused);
    EXPECT_EQ(vbPool->GetPageCount(), 3u);
    EXPECT_EQ(vbPool->GetFreeBytes(), (8u + 2u + 12u - 4u) * 12u);
    // 移动赋值释放左值持有的 4 顶点子分配，仅剩 v1 的 6 顶点在用。
    *vReused = std::move(*v1);
    v1.reset();
    EXPECT_EQ(vbPool->GetFreeBytes(), (8u + 2u + 12u) * 12u);
    vReused.reset();
    EXPECT_EQ(vbPool->GetFreeBytes(), (8u + 8u + 12u) * 12u);

    // IndexBufferPool 同样在多页分配、释放、重分配与全释放中保持精确字节统计。
    auto i0 = ibPool->Allocate(4);   // page 0 (cap 10): free 6
    auto i1 = ibPool->Allocate(7);   // page 1 (cap 10): free 3
    auto i2 = ibPool->Allocate(15);  // page 2 (cap 15): free 0
    ASSERT_TRUE(i0 && i1 && i2);
    EXPECT_EQ(ibPool->GetPageCount(), 3u);
    EXPECT_EQ(ibPool->GetFreeBytes(), (6u + 3u + 0u) * 2u);
    i1.reset();
    EXPECT_EQ(ibPool->GetFreeBytes(), (6u + 10u + 0u) * 2u);
    auto i3 = ibPool->Allocate(6);
    ASSERT_TRUE(i3);
    EXPECT_EQ(ibPool->GetFreeBytes(), (6u + 10u - 6u) * 2u);
    i0.reset();
    i2.reset();
    i3.reset();
    EXPECT_EQ(ibPool->GetFreeBytes(), (10u + 10u + 15u) * 2u);
}

}  // namespace
