#include "gpu_test_fixture.h"

namespace radray::render {
namespace {

class BufferMappingContractTest : public testing::TestWithParam<RenderBackend> {
protected:
    test::DeviceContext Context;
    void SetUp() override {
        if (!test::TryCreateDevice(GetParam(), Context, true, true)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(GetParam()))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
    void TearDown() override { Context.Reset(); }

    unique_ptr<Buffer> MakeBuffer(MemoryType memory, bool persistent) {
        const auto usage = memory == MemoryType::Upload ? BufferUse::CopySource | BufferUse::MapWrite : BufferUse::CopyDestination | BufferUse::MapRead;
        const auto hints = persistent ? ResourceHint::Dedicated | ResourceHint::PersistentMap : ResourceHints{ResourceHint::Dedicated};
        return Context.Device->CreateBuffer({.Size = 256, .Memory = memory, .Usage = usage, .Hints = hints}).Unwrap();
    }

    void CacheOperation(Buffer& buffer) {
        if (buffer.GetDesc().Memory == MemoryType::Upload)
            buffer.FlushMappedRange({0, 16});
        else
            buffer.InvalidateMappedRange({0, 16});
    }
};

TEST_P(BufferMappingContractTest, CacheOperationsRequireActiveMapping) {
    for (const auto memory : {MemoryType::Upload, MemoryType::ReadBack}) {
        auto buffer = MakeBuffer(memory, false);
        const auto before = Context.RhiValidationErrors.load();
        CacheOperation(*buffer);
        EXPECT_EQ(Context.RhiValidationErrors.load(), before + 1);

        auto* mapped = static_cast<byte*>(buffer->Map(0, 256));
        ASSERT_NE(mapped, nullptr);
        if (memory == MemoryType::Upload) mapped[0] = byte{0x7b};
        CacheOperation(*buffer);
        EXPECT_EQ(Context.RhiValidationErrors.load(), before + 1);
        buffer->Unmap();

        CacheOperation(*buffer);
        EXPECT_EQ(Context.RhiValidationErrors.load(), before + 2);
    }
}

TEST_P(BufferMappingContractTest, PersistentMappingKeepsCacheOperationsValid) {
    for (const auto memory : {MemoryType::Upload, MemoryType::ReadBack}) {
        auto buffer = MakeBuffer(memory, true);
        CacheOperation(*buffer);

        auto* mapped = static_cast<byte*>(buffer->Map(0, 256));
        ASSERT_NE(mapped, nullptr);
        if (memory == MemoryType::Upload) mapped[0] = byte{0x7b};
        CacheOperation(*buffer);
        buffer->Unmap();

        CacheOperation(*buffer);
        EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, BufferMappingContractTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

}  // namespace
}  // namespace radray::render
