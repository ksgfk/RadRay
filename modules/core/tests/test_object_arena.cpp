#include <radray/object_arena.h>

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <vector>

#include <gtest/gtest.h>

namespace radray {
namespace {

// Records destruction order; the arena must not see these counters.
struct Probe {
    Probe() { Destroyed.reserve(256); }
    vector<int> Destroyed;
    int Live{0};
};

struct Tracked {
    Tracked(Probe& probe, int id) noexcept : Observer(probe), Id(id) { ++Observer.Live; }
    ~Tracked() noexcept {
        --Observer.Live;
        Observer.Destroyed.push_back(Id);
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Probe& Observer;
    int Id;
};

// Emplaces nested Tracked objects 0..count-1 while constructing, then optionally fails.
struct Nested {
    Nested(ObjectArena& arena, Probe& probe, int count, bool fail) : Observer(probe) {
        for (int i = 0; i < count; ++i) arena.Emplace<Tracked>(probe, i);
        if (fail) (void)arena.AllocateBytes(std::numeric_limits<size_t>::max(), 16);
        ++Observer.Live;
    }
    ~Nested() noexcept {
        --Observer.Live;
        Observer.Destroyed.push_back(100);
    }
    Probe& Observer;
};

struct alignas(256) OverAligned {
    size_t Value;
};

template <class T>
using ArenaVector = std::vector<T, ArenaAllocator<T>>;
using ArenaString = std::basic_string<char, std::char_traits<char>, ArenaAllocator<char>>;

vector<int> Descending(int from, int to) {
    vector<int> result;
    for (int i = from; i >= to; --i) result.push_back(i);
    return result;
}

bool IsAligned(const void* pointer, size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

TEST(ObjectArenaTest, NewArenaReservesNothing) {
    ObjectArena arena;

    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, ZeroBlockSizeAborts) {
    EXPECT_DEATH((ObjectArena{{0, 0}}), "");
}

TEST(ObjectArenaTest, ZeroByteRequestsReturnDistinctStorage) {
    ObjectArena arena;

    void* first = arena.AllocateBytes(0, 1);
    void* second = arena.AllocateBytes(0, 1);

    EXPECT_NE(first, second);
}

TEST(ObjectArenaTest, InvalidRequestsFailAndLeaveArenaUsable) {
    ObjectArena arena;
    auto* existing = static_cast<unsigned char*>(arena.AllocateBytes(1, 1));
    *existing = 7;
    const size_t reserved = arena.GetReservedBytes();
    const size_t hugeAlignment = size_t{1} << (std::numeric_limits<size_t>::digits - 1);

    EXPECT_FALSE(arena.TryAllocateBytes(1, 0));
    EXPECT_FALSE(arena.TryAllocateBytes(1, 3));
    EXPECT_FALSE(arena.TryAllocateBytes(std::numeric_limits<size_t>::max(), 16));
    EXPECT_FALSE(arena.TryAllocateBytes(1, hugeAlignment));
    EXPECT_THROW((void)arena.AllocateBytes(1, 3), std::bad_alloc);

    EXPECT_EQ(arena.GetReservedBytes(), reserved);
    EXPECT_EQ(*existing, 7);
    EXPECT_EQ(*arena.Emplace<int>(42), 42);
}

TEST(ObjectArenaTest, BlockSizeOverflowFailsWithoutReservingStorage) {
    ObjectArena arena{{std::numeric_limits<size_t>::max(), 0}};

    EXPECT_FALSE(arena.TryAllocateBytes(1, 1));
    EXPECT_THROW(arena.Emplace<int>(1), std::bad_alloc);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, AllocationsAreAlignedAndStableAcrossBlockGrowth) {
    ObjectArena arena{{128, 1024}};
    vector<std::pair<unsigned char*, unsigned char>> written;

    for (size_t alignment : {size_t{1}, size_t{16}, size_t{256}, size_t{4096}}) {
        for (int i = 0; i < 12; ++i) {
            auto* bytes = static_cast<unsigned char*>(arena.AllocateBytes(19, alignment));
            EXPECT_TRUE(IsAligned(bytes, alignment));
            const auto fill = static_cast<unsigned char>(written.size());
            std::fill_n(bytes, 19, fill);
            written.emplace_back(bytes, fill);
        }
    }

    for (const auto& [bytes, fill] : written) {
        EXPECT_TRUE(std::all_of(bytes, bytes + 19, [fill](unsigned char b) { return b == fill; }));
    }
    EXPECT_TRUE(IsAligned(arena.Emplace<OverAligned>(OverAligned{73}), alignof(OverAligned)));
}

TEST(ObjectArenaTest, LargeAllocationDoesNotConsumeOrdinaryBlock) {
    ObjectArena arena{{128, 4096}};
    auto* first = static_cast<byte*>(arena.AllocateBytes(1, 1));
    const size_t ordinary = arena.GetReservedBytes();

    (void)arena.AllocateBytes(4096, 256);
    auto* second = static_cast<byte*>(arena.AllocateBytes(1, 1));

    EXPECT_EQ(second, first + 1);
    arena.Reset();
    EXPECT_EQ(arena.GetReservedBytes(), ordinary);
    EXPECT_EQ(arena.AllocateBytes(1, 1), first);
}

TEST(ObjectArenaTest, ResetReusesRetainedBlockFromTheStart) {
    ObjectArena arena{{4096, 8192}};
    int* first = arena.Emplace<int>(0);
    for (int i = 1; i < 200; ++i) arena.Emplace<int>(i);
    const size_t reserved = arena.GetReservedBytes();

    arena.Reset();

    EXPECT_EQ(arena.GetReservedBytes(), reserved);
    EXPECT_EQ(arena.Emplace<int>(9), first);
}

TEST(ObjectArenaTest, ResetDestroysObjectsOnceInReverseOrder) {
    Probe probe;
    ObjectArena arena;
    for (int i = 0; i < 80; ++i) arena.Emplace<Tracked>(probe, i);

    arena.Reset();
    arena.Reset();
    arena.Release();

    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(probe.Destroyed, Descending(79, 0));
}

TEST(ObjectArenaTest, ArenaRemainsUsableAfterRelease) {
    Probe probe;
    {
        ObjectArena arena;
        arena.Emplace<Tracked>(probe, 1);
        arena.Release();
        EXPECT_EQ(arena.GetReservedBytes(), 0u);

        arena.Emplace<Tracked>(probe, 2);
    }

    EXPECT_EQ(probe.Destroyed, (vector<int>{1, 2}));
}

TEST(ObjectArenaTest, OwnsMovedInResourcesAndScopeGuards) {
    Probe probe;
    int callbacks = 0;
    ObjectArena arena;
    auto owner = make_unique<Tracked>(probe, 4);
    auto guard = MakeScopeGuard([&callbacks]() noexcept { ++callbacks; });

    arena.Emplace<decltype(owner)>(std::move(owner));
    arena.Emplace<decltype(guard)>(std::move(guard));
    arena.Reset();

    EXPECT_EQ(probe.Destroyed, (vector<int>{4}));
    EXPECT_EQ(callbacks, 1);
}

TEST(ObjectArenaTest, AllocationFailureDoesNotConsumeConstructorArguments) {
    Probe probe;
    auto owner = make_unique<Tracked>(probe, 5);
    ObjectArena arena{{std::numeric_limits<size_t>::max(), 0}};

    EXPECT_THROW(arena.Emplace<decltype(owner)>(std::move(owner)), std::bad_alloc);

    EXPECT_NE(owner, nullptr);
    EXPECT_TRUE(probe.Destroyed.empty());
}

TEST(ObjectArenaTest, NestedObjectsAreDestroyedInCompletionOrder) {
    Probe probe;
    ObjectArena arena;
    arena.Emplace<Nested>(arena, probe, 40, false);

    arena.Reset();

    vector<int> expected{100};
    for (int id : Descending(39, 0)) expected.push_back(id);
    EXPECT_EQ(probe.Destroyed, expected);
}

TEST(ObjectArenaTest, FailedConstructionKeepsCompletedNestedObjects) {
    Probe probe;
    ObjectArena arena;

    EXPECT_THROW(arena.Emplace<Nested>(arena, probe, 40, true), std::bad_alloc);
    EXPECT_EQ(probe.Live, 40);
    arena.Emplace<Tracked>(probe, 101);
    arena.Reset();

    vector<int> expected{101};
    for (int id : Descending(39, 0)) expected.push_back(id);
    EXPECT_EQ(probe.Destroyed, expected);
    EXPECT_EQ(probe.Live, 0);
}

TEST(ObjectArenaTest, ZeroBudgetResetReleasesEverything) {
    Probe probe;
    ObjectArena arena{{128, 0}};
    for (int i = 0; i < 100; ++i) arena.Emplace<Tracked>(probe, i);
    (void)arena.AllocateBytes(8192, 4096);

    arena.Reset();

    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, ResetRetainsEarliestBlocksWithinBudget) {
    ObjectArena measuring{{128, 0}};
    (void)measuring.AllocateBytes(128, 1);
    const size_t blockBytes = measuring.GetReservedBytes();
    ObjectArena arena{{128, blockBytes * 2}};
    void* first = arena.AllocateBytes(128, 1);
    void* second = arena.AllocateBytes(128, 1);
    (void)arena.AllocateBytes(128, 1);

    arena.Reset();

    EXPECT_EQ(arena.GetReservedBytes(), blockBytes * 2);
    EXPECT_EQ(arena.AllocateBytes(128, 1), first);
    EXPECT_EQ(arena.AllocateBytes(128, 1), second);
}

TEST(ObjectArenaTest, ResetKeepsCleanupCapacityOnlyWhenItFitsBudget) {
    Probe probe;
    ObjectArena measuring{{4096, 0}};
    measuring.Emplace<Tracked>(probe, 0);
    const size_t blockBytes = measuring.GetReservedBytes();
    for (int i = 1; i < 40; ++i) measuring.Emplace<Tracked>(probe, i);
    const size_t cleanupBytes = measuring.GetReservedBytes() - blockBytes;
    ASSERT_GT(cleanupBytes, 0u);

    ObjectArena fits{{4096, cleanupBytes}};
    ObjectArena tooSmall{{4096, cleanupBytes - 1}};
    for (int i = 0; i < 40; ++i) {
        fits.Emplace<Tracked>(probe, i);
        tooSmall.Emplace<Tracked>(probe, i);
    }
    fits.Reset();
    tooSmall.Reset();

    EXPECT_EQ(fits.GetReservedBytes(), cleanupBytes);
    EXPECT_EQ(tooSmall.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, RetainedBytesStayWithinBudgetAfterPeak) {
    ObjectArena arena{{1024, 4096}};
    for (int i = 0; i < 100; ++i) (void)arena.AllocateBytes(1024, 1);
    (void)arena.AllocateBytes(1024 * 1024, 256);

    arena.Reset();
    const size_t retained = arena.GetReservedBytes();
    for (int i = 0; i < 20; ++i) {
        arena.Emplace<int>(i);
        arena.Reset();
    }

    EXPECT_GT(retained, 0u);
    EXPECT_LE(retained, 4096u);
    EXPECT_EQ(arena.GetReservedBytes(), retained);
}

TEST(ArenaAllocatorTest, EqualityFollowsArenaAcrossRebind) {
    ObjectArena first;
    ObjectArena second;
    const ArenaAllocator<int> allocator{first};

    const ArenaAllocator<char> rebound{allocator};

    EXPECT_TRUE(allocator == rebound);
    EXPECT_FALSE(allocator == ArenaAllocator<int>{second});
    EXPECT_EQ(&rebound.GetArena(), &first);
}

TEST(ArenaAllocatorTest, DeallocateDoesNotReclaimStorage) {
    ObjectArena arena;
    ArenaAllocator<uint64_t> allocator{arena};
    uint64_t* value = allocator.allocate(4);
    const size_t reserved = arena.GetReservedBytes();

    allocator.deallocate(value, 4);

    EXPECT_EQ(arena.GetReservedBytes(), reserved);
    EXPECT_NE(allocator.allocate(4), value);
}

TEST(ArenaAllocatorTest, CountOverflowAndStorageFailureThrowDistinctExceptions) {
    ObjectArena arena;
    ArenaAllocator<uint64_t> words{arena};
    ArenaAllocator<byte> bytes{arena};

    EXPECT_THROW((void)words.allocate(std::numeric_limits<size_t>::max() / sizeof(uint64_t) + 1), std::bad_array_new_length);
    EXPECT_THROW((void)bytes.allocate(std::numeric_limits<size_t>::max()), std::bad_alloc);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ArenaAllocatorTest, ContainerOwnsElementLifetimes) {
    Probe probe;
    ObjectArena arena;
    {
        ArenaVector<unique_ptr<Tracked>> values{ArenaAllocator<unique_ptr<Tracked>>{arena}};
        for (int i = 0; i < 60; ++i) values.push_back(make_unique<Tracked>(probe, i));

        values.erase(values.begin() + 1);
        EXPECT_EQ(probe.Destroyed, (vector<int>{1}));
        values.clear();
        EXPECT_EQ(probe.Live, 0);
    }

    arena.Reset();
    EXPECT_EQ(probe.Destroyed.size(), 60u);
}

TEST(ArenaAllocatorTest, ArenaOwnedContainerIsDestroyedOnReset) {
    Probe probe;
    ObjectArena arena;
    auto* values = arena.Emplace<ArenaVector<unique_ptr<Tracked>>>(ArenaAllocator<unique_ptr<Tracked>>{arena});
    for (int i = 0; i < 40; ++i) values->push_back(make_unique<Tracked>(probe, i));

    arena.Reset();

    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(probe.Destroyed.size(), 40u);
}

TEST(ArenaAllocatorTest, NestedContainersReceiveTheSameArena) {
    ObjectArena arena;
    {
        ArenaVector<ArenaString> names{ArenaAllocator<ArenaString>{arena}};
        names.emplace_back(200, 'a');
        using MapAllocator = ArenaAllocator<std::pair<const int, ArenaVector<int>>>;
        std::map<int, ArenaVector<int>, std::less<int>, MapAllocator> map{MapAllocator{arena}};
        map[7].push_back(11);
        std::list<int, ArenaAllocator<int>> list{ArenaAllocator<int>{arena}};
        list.push_back(1);

        EXPECT_EQ(&names[0].get_allocator().GetArena(), &arena);
        EXPECT_EQ(names[0].size(), 200u);
        EXPECT_EQ(&map[7].get_allocator().GetArena(), &arena);
        EXPECT_EQ(map[7].front(), 11);
        EXPECT_EQ(list.front(), 1);
    }
    arena.Reset();
}

TEST(ArenaAllocatorTest, OverAlignedElementsStayAligned) {
    ObjectArena arena{{128, 0}};
    {
        ArenaVector<OverAligned> values{ArenaAllocator<OverAligned>{arena}};
        for (size_t i = 0; i < 40; ++i) values.push_back(OverAligned{i});

        EXPECT_TRUE(IsAligned(values.data(), alignof(OverAligned)));
        EXPECT_EQ(values[39].Value, 39u);
    }
    arena.Reset();
}

TEST(ArenaAllocatorTest, CopyConstructionKeepsSourceArenaAndAssignmentKeepsTarget) {
    ObjectArena first;
    ObjectArena second;
    {
        ArenaVector<int> source{ArenaAllocator<int>{first}};
        source.assign({1, 2, 3});
        ArenaVector<int> target{ArenaAllocator<int>{second}};

        const ArenaVector<int> copy = source;
        target = source;

        EXPECT_EQ(&copy.get_allocator().GetArena(), &first);
        EXPECT_EQ(&target.get_allocator().GetArena(), &second);
        EXPECT_EQ(target, source);
    }
}

TEST(ArenaAllocatorTest, MoveAssignmentStealsOnlyWithinTheSameArena) {
    ObjectArena first;
    ObjectArena second;
    {
        ArenaVector<int> source{{1, 2, 3}, ArenaAllocator<int>{first}};
        ArenaVector<int> sameArena{ArenaAllocator<int>{first}};
        ArenaVector<int> otherArena{ArenaAllocator<int>{second}};
        const int* storage = source.data();

        sameArena = std::move(source);
        EXPECT_EQ(sameArena.data(), storage);

        otherArena = std::move(sameArena);
        EXPECT_NE(otherArena.data(), storage);
        EXPECT_EQ(&otherArena.get_allocator().GetArena(), &second);
        EXPECT_EQ(otherArena, (ArenaVector<int>{{1, 2, 3}, ArenaAllocator<int>{second}}));
    }
}

TEST(ArenaAllocatorTest, CrossArenaAssignmentRebuildsNestedElementsInTarget) {
    ObjectArena first;
    ObjectArena second;
    {
        ArenaVector<ArenaString> source{ArenaAllocator<ArenaString>{first}};
        source.emplace_back(500, 'x');
        ArenaVector<ArenaString> target{ArenaAllocator<ArenaString>{second}};

        target = source;
        EXPECT_EQ(&target.front().get_allocator().GetArena(), &second);

        target.clear();
        target = std::move(source);
        EXPECT_EQ(&target.front().get_allocator().GetArena(), &second);
        EXPECT_EQ(target.front().size(), 500u);
    }
}

}  // namespace
}  // namespace radray
