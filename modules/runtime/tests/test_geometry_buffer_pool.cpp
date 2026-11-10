#include <gtest/gtest.h>

#include <radray/runtime/geometry_buffer_pool.h>
#include <radray/runtime/mesh_draw_command.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::render;

namespace {

class PoolDevice final : public test::FakeDevice {
public:
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        auto buffer = make_unique<test::FakeBuffer>(desc.Size, desc.Usage);
        buffer->Desc = desc;
        return buffer;
    }
};

TEST(GeometryBufferPool, VertexPagesKeepInputsStableShareBindingsAndRecycleRanges) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}, VertexBufferLayout{1, 16, VertexStepMode::Instance}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0},
                           VertexInputAttribute{1, 0, "TEXCOORD", 0, VertexFormat::FLOAT32X4, 1}};
    auto pool = VertexBufferPool::Create(device, {.VertexStride = 12, .PageVertexCapacity = 8, .Layouts = layouts, .Attributes = attributes});
    ASSERT_TRUE(pool);
    auto a = pool->Allocate(3);
    auto b = pool->Allocate(5);
    ASSERT_TRUE(a && b);
    const auto* input = &a->GetVertexInput();
    EXPECT_EQ(input, &b->GetVertexInput());
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

TEST(GeometryBufferPool, RejectsInvalidLayoutsAndIndexStrides) {
    PoolDevice device;
    const array layouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}, VertexBufferLayout{1, 12, VertexStepMode::Vertex}};
    const array attributes{VertexInputAttribute{0, 0, "POSITION", 0, VertexFormat::FLOAT32X3, 0}};
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 12, .Layouts = layouts, .Attributes = attributes}));
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 16, .Layouts = std::span{layouts}.first(1), .Attributes = attributes}));
    const array threeLayouts{layouts[0], VertexBufferLayout{1, 16, VertexStepMode::Instance}, VertexBufferLayout{2, 16, VertexStepMode::Instance}};
    EXPECT_FALSE(VertexBufferPool::Create(device, {.VertexStride = 12, .Layouts = threeLayouts, .Attributes = attributes}));
    EXPECT_FALSE(IndexBufferPool::Create(device, {.IndexStride = 3}));
    EXPECT_FALSE(IndexBufferPool::Create(device, {.PageIndexCapacity = 0}));
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

}  // 匿名命名空间
