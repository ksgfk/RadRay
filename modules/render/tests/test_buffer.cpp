#include "gpu_test_fixture.h"

namespace radray::render {
namespace {

constexpr uint64_t BufferSize = 256;

vector<byte> Pattern() {
    vector<byte> bytes(BufferSize);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<byte>(i * 7 + 3);
    return bytes;
}

class BufferTest : public test::BackendTest {
protected:
    unique_ptr<Buffer> CreateMappable(MemoryType memory, ResourceHints hints = ResourceHint::None) {
        const BufferUses usage = memory == MemoryType::Upload ? BufferUse::MapWrite | BufferUse::CopySource
                                                              : BufferUse::MapRead | BufferUse::CopyDestination;
        return GetDevice().CreateBuffer({.Size = BufferSize, .Memory = memory, .Usage = usage, .Hints = hints}).Unwrap();
    }

    void CopyToHost(Buffer& upload, Buffer& readback) {
        auto recording = test::BeginRecording(GetDevice());
        auto& commands = *recording.Commands;
        test::Barrier(commands, {BarrierBufferDescriptor{.Target = &upload, .Before = BufferState::HostWrite, .After = BufferState::CopySource}});
        commands.CopyBufferToBuffer(&readback, 0, &upload, 0, BufferSize);
        test::Barrier(commands, {BarrierBufferDescriptor{.Target = &readback, .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
        test::SubmitAndWait(GetDevice(), recording);
    }
};

TEST_P(BufferTest, UploadContentCopiesToReadback) {
    const auto expected = Pattern();
    auto upload = CreateMappable(MemoryType::Upload);
    auto readback = CreateMappable(MemoryType::ReadBack);
    void* written = upload->Map(0, BufferSize);
    ASSERT_NE(written, nullptr);
    std::memcpy(written, expected.data(), expected.size());
    upload->FlushMappedRange({0, BufferSize});
    upload->Unmap();

    CopyToHost(*upload, *readback);

    EXPECT_EQ(test::MapAndRead(*readback), expected);
}

TEST_P(BufferTest, PersistentMapAllowsCacheOperationsOutsideMap) {
    const auto expected = Pattern();
    auto upload = CreateMappable(MemoryType::Upload, ResourceHint::PersistentMap);
    auto readback = CreateMappable(MemoryType::ReadBack, ResourceHint::PersistentMap);
    void* written = upload->Map(0, BufferSize);
    ASSERT_NE(written, nullptr);
    std::memcpy(written, expected.data(), expected.size());
    upload->Unmap();
    upload->FlushMappedRange({0, BufferSize});

    CopyToHost(*upload, *readback);
    readback->InvalidateMappedRange({0, BufferSize});

    const auto* read = static_cast<const byte*>(readback->Map(0, BufferSize));
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(vector<byte>(read, read + BufferSize), expected);
    readback->Unmap();
}

TEST_P(BufferTest, CacheOperationsOutsideMapAreRejected) {
    auto upload = CreateMappable(MemoryType::Upload);
    auto readback = CreateMappable(MemoryType::ReadBack);

    upload->FlushMappedRange({0, BufferSize});
    EXPECT_EQ(TakeRhiValidationErrors(), 1u);
    readback->InvalidateMappedRange({0, BufferSize});
    EXPECT_EQ(TakeRhiValidationErrors(), 1u);
}

INSTANTIATE_TEST_SUITE_P(Backends, BufferTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::render
