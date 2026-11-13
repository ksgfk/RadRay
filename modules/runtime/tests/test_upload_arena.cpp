#include <gtest/gtest.h>

#include <radray/runtime/upload_arena.h>

#include "test_fakes.h"

using namespace radray;
using namespace radray::render;

namespace {

struct MappingProbe {
    uint32_t LiveBuffers{0};
    uint32_t MappedBuffers{0};
    bool DestroyedWhileMapped{false};
    bool UnmappedTwice{false};
};

class ObservedBuffer final : public test::FakeBuffer {
public:
    ObservedBuffer(const BufferDescriptor& desc, MappingProbe& probe)
        : FakeBuffer(desc.Size, desc.Usage), _probe(probe), _bytes(static_cast<size_t>(desc.Size)) {
        Desc = desc;
        ++_probe.LiveBuffers;
    }
    ~ObservedBuffer() noexcept override {
        _probe.DestroyedWhileMapped |= _mapped;
        --_probe.LiveBuffers;
    }
    void* Map(uint64_t offset, uint64_t) noexcept override {
        _mapped = true;
        ++_probe.MappedBuffers;
        return _bytes.data() + offset;
    }
    void Unmap() noexcept override {
        if (_mapped)
            --_probe.MappedBuffers;
        else
            _probe.UnmappedTwice = true;
        _mapped = false;
    }

private:
    MappingProbe& _probe;
    vector<byte> _bytes;
    bool _mapped{false};
};

class ObservedDevice final : public test::FakeDevice {
public:
    explicit ObservedDevice(MappingProbe& probe) : _probe(probe) {}
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        return make_unique<ObservedBuffer>(desc, _probe);
    }

private:
    MappingProbe& _probe;
};

}  // namespace

TEST(UploadArena, GrowthPreservesEarlierAddressesAndResetReusesMappedPages) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    uint32_t maps = 0;
    device.OnMap = [&] { ++maps; };
    UploadArena arena{device, 16};
    auto first = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(first);
    const auto desc = first->Buffer->GetDesc();
    EXPECT_EQ(desc.Memory, MemoryType::Upload);
    EXPECT_TRUE(desc.Usage.HasFlag(BufferUse::MapWrite | BufferUse::CopySource | BufferUse::Vertex | BufferUse::CBuffer));
    EXPECT_TRUE(desc.Hints.HasFlag(ResourceHint::PersistentMap));
    auto second = arena.Allocate(32, 4, 32, BufferState::CopySource);
    ASSERT_TRUE(second);
    EXPECT_NE(first->Buffer, second->Buffer);
    first->Data.Get()[0] = byte{42};
    EXPECT_EQ(first->Data.Get()[0], byte{42});
    EXPECT_EQ(arena.GetCapacity(), 48u);
    EXPECT_EQ(maps, 2u);
    ASSERT_TRUE(arena.Flush());
    arena.Reset();
    auto reused = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->Buffer, first->Buffer);
    EXPECT_EQ(reused->Data, first->Data);
    EXPECT_EQ(maps, 2u);
}

TEST(UploadArena, SharedPageUnifiesReadStatesAndPaddingDoesNotOverlap) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    vector<BufferRange> flushed;
    device.OnFlush = [&](BufferRange range) { flushed.push_back(range); };
    UploadArena arena{device, 1024};
    const auto constants = arena.Allocate(80, 256, 256, BufferState::CBuffer);
    const auto vertices = arena.Allocate(16, 4, 16, BufferState::Vertex);
    const auto staging = arena.Allocate(4, 4, 4, BufferState::CopySource);
    ASSERT_TRUE(constants && vertices && staging);
    EXPECT_EQ(constants->Buffer, vertices->Buffer);
    EXPECT_EQ(staging->Buffer, vertices->Buffer);
    EXPECT_EQ(vertices->Offset, 256u);
    EXPECT_EQ(constants->Size, 80u);
    ASSERT_TRUE(arena.Flush());
    ASSERT_EQ(flushed.size(), 1u);
    EXPECT_EQ(flushed[0].Size, 276u);
    vector<ResourceBarrierDescriptor> barriers;
    arena.AppendReadBarriers(barriers);
    ASSERT_EQ(barriers.size(), 1u);
    EXPECT_EQ(std::get<BarrierBufferDescriptor>(barriers[0]).After, BufferState::CBuffer | BufferState::Vertex | BufferState::CopySource);
}

TEST(UploadArena, ZeroAndInvalidAllocationsDoNotMap) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    uint32_t maps = 0;
    device.OnMap = [&] { ++maps; };
    UploadArena arena{device, 64};
    const auto empty = arena.Allocate(0, 1, 0, BufferState::Vertex);
    ASSERT_TRUE(empty);
    EXPECT_FALSE(empty->Buffer);
    EXPECT_FALSE(arena.Allocate(16, 0, 16, BufferState::Vertex));
    EXPECT_FALSE(arena.Allocate(16, 1, 8, BufferState::Vertex));
    EXPECT_FALSE(arena.Allocate(16, 1, 16, BufferState::UnorderedAccess));
    EXPECT_EQ(maps, 0u);
    EXPECT_EQ(arena.GetCapacity(), 0u);
}

TEST(UploadArena, FailedPageCreationOrMappingPreservesExistingAllocation) {
    test::FakeDevice device;
    device.EnableHostBuffers = true;
    UploadArena arena{device, 16};
    const auto first = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(first);
    first->Data.Get()[0] = byte{42};

    device.BuffersUntilFailure = 0;
    EXPECT_FALSE(arena.Allocate(32, 4, 32, BufferState::CopySource));
    device.BuffersUntilFailure = -1;
    device.FailMaps = true;
    EXPECT_FALSE(arena.Allocate(32, 4, 32, BufferState::CopySource));
    EXPECT_EQ(first->Data.Get()[0], byte{42});
    EXPECT_EQ(arena.GetCapacity(), 16u);
    ASSERT_TRUE(arena.Flush());

    arena.Reset();
    const auto reused = arena.Allocate(16, 4, 16, BufferState::Vertex);
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->Buffer, first->Buffer);
    EXPECT_EQ(reused->Data, first->Data);
}

TEST(UploadArena, ClearAndDestructionUnmapAndReleasePages) {
    MappingProbe probe;
    ObservedDevice device{probe};
    {
        UploadArena arena{device, 16};
        ASSERT_TRUE(arena.Allocate(16, 4, 16, BufferState::Vertex));
        ASSERT_TRUE(arena.Allocate(32, 4, 32, BufferState::CopySource));
        arena.Reset();
        EXPECT_EQ(probe.LiveBuffers, 2u);
        EXPECT_EQ(probe.MappedBuffers, 2u);
        arena.Clear();
        EXPECT_EQ(arena.GetCapacity(), 0u);
        EXPECT_EQ(probe.LiveBuffers, 0u);
        EXPECT_EQ(probe.MappedBuffers, 0u);
        arena.Clear();
        ASSERT_TRUE(arena.Allocate(16, 4, 16, BufferState::CBuffer));
    }
    EXPECT_EQ(probe.LiveBuffers, 0u);
    EXPECT_EQ(probe.MappedBuffers, 0u);
    EXPECT_FALSE(probe.DestroyedWhileMapped);
    EXPECT_FALSE(probe.UnmappedTwice);
}
