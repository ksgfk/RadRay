#include <radray/object_arena.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct Probe {
    Probe() { Destroyed.reserve(256); }
    radray::vector<int> Destroyed;
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

struct Nested {
    Nested(radray::ObjectArena& arena, Probe& probe, bool fail) : Observer(probe) {
        for (int i = 0; i < 40; ++i) arena.Emplace<Tracked>(probe, i);
        if (fail) (void)arena.AllocateBytes(std::numeric_limits<size_t>::max(), 16);
        ++Observer.Live;
    }
    ~Nested() noexcept {
        --Observer.Live;
        Observer.Destroyed.push_back(100);
    }
    Probe& Observer;
};

struct Recursive {
    Recursive(radray::ObjectArena& arena, Probe& probe, int depth) : Observer(probe), Depth(depth) {
        if (depth > 0) arena.Emplace<Recursive>(arena, probe, depth - 1);
        ++Observer.Live;
    }
    ~Recursive() noexcept {
        --Observer.Live;
        Observer.Destroyed.push_back(Depth);
    }
    Probe& Observer;
    int Depth;
};

struct alignas(256) AlignedValue {
    size_t Value;
};

TEST(ObjectArenaTest, EmptyAndDefaultConfiguration) {
    const radray::ObjectArenaOptions options;
    EXPECT_EQ(options.BlockSize, 64u * 1024);
    EXPECT_EQ(options.MaxRetainedBytes, 256u * 1024);
    static_assert(!std::is_copy_constructible_v<radray::ObjectArena>);
    static_assert(!std::is_move_constructible_v<radray::ObjectArena>);
    radray::ObjectArena arena;
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
    arena.Reset();
    arena.Release();
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, ZeroBlockSizeIsRejected) {
    EXPECT_DEATH((radray::ObjectArena{{0, 0}}), "");
}

TEST(ObjectArenaTest, ZeroSizeAndInvalidAllocationLeaveArenaUsable) {
    radray::ObjectArena arena;
    auto* first = static_cast<unsigned char*>(arena.AllocateBytes(0, 1));
    auto* second = static_cast<unsigned char*>(arena.AllocateBytes(0, 1));
    ASSERT_NE(first, second);
    *first = 7;
    *second = 9;
    const size_t reserved = arena.GetReservedBytes();
    EXPECT_FALSE(arena.TryAllocateBytes(1, 0));
    EXPECT_FALSE(arena.TryAllocateBytes(1, 3));
    EXPECT_FALSE(arena.TryAllocateBytes(std::numeric_limits<size_t>::max(), 16));
    const size_t hugeAlignment = size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    EXPECT_FALSE(arena.TryAllocateBytes(1, hugeAlignment));
    EXPECT_THROW((void)arena.AllocateBytes(1, 3), std::bad_alloc);
    EXPECT_THROW((void)arena.AllocateBytes(std::numeric_limits<size_t>::max(), 16), std::bad_alloc);
    EXPECT_EQ(arena.GetReservedBytes(), reserved);
    EXPECT_EQ(*first, 7);
    EXPECT_EQ(*second, 9);
    EXPECT_EQ(*arena.Emplace<int>(42), 42);
}

TEST(ObjectArenaTest, BlockAndAlignmentPaddingOverflowIsRejected) {
    radray::ObjectArena arena{{std::numeric_limits<size_t>::max(), 0}};
    EXPECT_FALSE(arena.TryAllocateBytes(1, 1));
    EXPECT_THROW(arena.Emplace<int>(1), std::bad_alloc);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, AlignmentAndAddressesSurviveBlockGrowth) {
    radray::ObjectArena arena{{128, 1024}};
    radray::vector<std::pair<unsigned char*, unsigned char>> values;
    for (size_t alignment : {size_t{1}, size_t{2}, size_t{8}, size_t{16}, size_t{64}, size_t{256}, size_t{4096}}) {
        for (size_t i = 0; i < 12; ++i) {
            auto* value = static_cast<unsigned char*>(arena.AllocateBytes(19, alignment));
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(value) % alignment, 0u);
            const auto fill = static_cast<unsigned char>(values.size());
            std::fill_n(value, 19, fill);
            values.emplace_back(value, fill);
        }
    }
    for (const auto& [value, fill] : values) {
        EXPECT_TRUE(std::all_of(value, value + 19, [fill](auto byte) { return byte == fill; }));
    }
    auto* aligned = arena.Emplace<AlignedValue>(AlignedValue{73});
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % alignof(AlignedValue), 0u);
    EXPECT_EQ(aligned->Value, 73u);
}

TEST(ObjectArenaTest, LargeAllocationDoesNotConsumeOrdinaryCursor) {
    radray::ObjectArena arena{{128, 4096}};
    auto* first = static_cast<radray::byte*>(arena.AllocateBytes(1, 1));
    const size_t ordinary = arena.GetReservedBytes();
    (void)arena.AllocateBytes(4096, 256);
    auto* second = static_cast<radray::byte*>(arena.AllocateBytes(1, 1));
    EXPECT_EQ(second, first + 1);
    EXPECT_GT(arena.GetReservedBytes(), ordinary);
    arena.Reset();
    EXPECT_EQ(arena.GetReservedBytes(), ordinary);
    EXPECT_EQ(arena.AllocateBytes(1, 1), first);
}

TEST(ObjectArenaTest, TrivialObjectsDoNotAllocateCleanupStorage) {
    radray::ObjectArena arena{{4096, 8192}};
    auto* first = arena.Emplace<int>(0);
    const size_t blockBytes = arena.GetReservedBytes();
    for (int i = 1; i < 200; ++i) EXPECT_EQ(*arena.Emplace<int>(i), i);
    EXPECT_EQ(arena.GetReservedBytes(), blockBytes);
    arena.Reset();
    EXPECT_EQ(arena.Emplace<int>(9), first);
}

TEST(ObjectArenaTest, DestructorsRunExactlyOnceInReverseOrder) {
    Probe probe;
    {
        radray::ObjectArena arena;
        for (int i = 0; i < 80; ++i) arena.Emplace<Tracked>(probe, i);
        EXPECT_EQ(probe.Live, 80);
        arena.Reset();
        EXPECT_EQ(probe.Live, 0);
        ASSERT_EQ(probe.Destroyed.size(), 80u);
        for (int i = 0; i < 80; ++i) EXPECT_EQ(probe.Destroyed[i], 79 - i);
        arena.Reset();
        arena.Release();
        arena.Release();
        EXPECT_EQ(probe.Destroyed.size(), 80u);
        arena.Emplace<Tracked>(probe, 80);
    }
    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(probe.Destroyed.back(), 80);
    EXPECT_EQ(probe.Destroyed.size(), 81u);
}

TEST(ObjectArenaTest, OwnsMoveOnlyResourceAndCleanupCallback) {
    Probe probe;
    int callbacks = 0;
    radray::ObjectArena arena;
    auto owner = radray::make_unique<Tracked>(probe, 4);
    auto* original = owner.get();
    auto* retained = arena.Emplace<decltype(owner)>(std::move(owner));
    EXPECT_EQ(owner, nullptr);
    EXPECT_EQ(retained->get(), original);
    auto callback = radray::MakeScopeGuard([&callbacks]() noexcept { ++callbacks; });
    arena.Emplace<decltype(callback)>(std::move(callback));
    arena.Reset();
    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(probe.Destroyed, radray::vector<int>{4});
    EXPECT_EQ(callbacks, 1);
}

TEST(ObjectArenaTest, AllocationFailurePrecedesOwnershipTransfer) {
    Probe probe;
    auto owner = radray::make_unique<Tracked>(probe, 5);
    radray::ObjectArena arena{{std::numeric_limits<size_t>::max(), 0}};
    EXPECT_THROW(arena.Emplace<decltype(owner)>(std::move(owner)), std::bad_alloc);
    EXPECT_TRUE(owner);
    arena.Release();
    EXPECT_EQ(probe.Live, 1);
    EXPECT_TRUE(probe.Destroyed.empty());
}

TEST(ObjectArenaTest, NestedConstructionUsesCompletionOrder) {
    Probe probe;
    radray::ObjectArena arena;
    arena.Emplace<Nested>(arena, probe, false);
    EXPECT_EQ(probe.Live, 41);
    arena.Reset();
    ASSERT_EQ(probe.Destroyed.size(), 41u);
    EXPECT_EQ(probe.Destroyed[0], 100);
    for (int i = 0; i < 40; ++i) EXPECT_EQ(probe.Destroyed[i + 1], 39 - i);
}

TEST(ObjectArenaTest, RecursiveConstructionReservesAllPendingCleanups) {
    Probe probe;
    radray::ObjectArena arena;
    arena.Emplace<Recursive>(arena, probe, 64);
    EXPECT_EQ(probe.Live, 65);
    arena.Reset();
    ASSERT_EQ(probe.Destroyed.size(), 65u);
    for (int i = 0; i < 65; ++i) EXPECT_EQ(probe.Destroyed[i], 64 - i);
}

TEST(ObjectArenaTest, FailedConstructorKeepsNestedObjectsAndAllowsRecovery) {
    Probe probe;
    radray::ObjectArena arena;
    EXPECT_THROW(arena.Emplace<Nested>(arena, probe, true), std::bad_alloc);
    EXPECT_EQ(probe.Live, 40);
    EXPECT_TRUE(probe.Destroyed.empty());
    arena.Emplace<Tracked>(probe, 101);
    arena.Reset();
    EXPECT_EQ(probe.Live, 0);
    ASSERT_EQ(probe.Destroyed.size(), 41u);
    EXPECT_EQ(probe.Destroyed[0], 101);
    for (int i = 0; i < 40; ++i) EXPECT_EQ(probe.Destroyed[i + 1], 39 - i);
    arena.Emplace<Tracked>(probe, 102);
    arena.Release();
    EXPECT_EQ(probe.Destroyed.back(), 102);
}

TEST(ObjectArenaTest, ZeroBudgetReleasesBlocksAndCleanupCapacity) {
    Probe probe;
    radray::ObjectArena arena{{128, 0}};
    for (int i = 0; i < 100; ++i) arena.Emplace<Tracked>(probe, i);
    (void)arena.AllocateBytes(8192, 4096);
    EXPECT_GT(arena.GetReservedBytes(), 8192u);
    arena.Reset();
    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
    arena.Emplace<Tracked>(probe, 100);
    arena.Release();
    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, BudgetIncludesHeadersAndRetainsEarliestBlocks) {
    radray::ObjectArena measuring{{128, 0}};
    (void)measuring.AllocateBytes(128, 1);
    const size_t blockBytes = measuring.GetReservedBytes();
    ASSERT_GT(blockBytes, 128u);
    radray::ObjectArena arena{{128, blockBytes * 2}};
    auto* first = arena.AllocateBytes(128, 1);
    auto* second = arena.AllocateBytes(128, 1);
    (void)arena.AllocateBytes(128, 1);
    EXPECT_EQ(arena.GetReservedBytes(), blockBytes * 3);
    arena.Reset();
    EXPECT_EQ(arena.GetReservedBytes(), blockBytes * 2);
    EXPECT_EQ(arena.AllocateBytes(128, 1), first);
    EXPECT_EQ(arena.AllocateBytes(128, 1), second);
    arena.Release();
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, CleanupCapacityHasPriorityWithinBudget) {
    Probe probe;
    radray::ObjectArena measuring{{4096, 8192}};
    auto* first = measuring.Emplace<Tracked>(probe, 0);
    const size_t blockBytes = measuring.GetReservedBytes();
    for (int i = 1; i < 40; ++i) measuring.Emplace<Tracked>(probe, i);
    const size_t cleanupBytes = measuring.GetReservedBytes() - blockBytes;
    ASSERT_GT(cleanupBytes, 0u);
    measuring.Reset();
    EXPECT_EQ(measuring.Emplace<Tracked>(probe, 40), first);
    measuring.Release();

    radray::ObjectArena arena{{4096, cleanupBytes}};
    for (int i = 0; i < 40; ++i) arena.Emplace<Tracked>(probe, i);
    arena.Reset();
    EXPECT_EQ(arena.GetReservedBytes(), cleanupBytes);
    arena.Release();
    EXPECT_EQ(arena.GetReservedBytes(), 0u);

    radray::ObjectArena tooSmall{{4096, cleanupBytes - 1}};
    for (int i = 0; i < 40; ++i) tooSmall.Emplace<Tracked>(probe, i);
    tooSmall.Reset();
    EXPECT_EQ(tooSmall.GetReservedBytes(), 0u);
}

TEST(ObjectArenaTest, PeakThenSmallWorkloadStaysWithinBudget) {
    radray::ObjectArena arena{{1024, 4096}};
    for (int i = 0; i < 100; ++i) (void)arena.AllocateBytes(1024, 1);
    (void)arena.AllocateBytes(1024 * 1024, 256);
    EXPECT_GT(arena.GetReservedBytes(), 1024u * 1024);
    arena.Reset();
    const size_t retained = arena.GetReservedBytes();
    EXPECT_LE(retained, 4096u);
    EXPECT_GT(retained, 0u);
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(*arena.Emplace<int>(i), i);
        arena.Reset();
        EXPECT_EQ(arena.GetReservedBytes(), retained);
    }
}

#ifdef RADRAY_IS_DEBUG
struct ResetInConstructor {
    explicit ResetInConstructor(radray::ObjectArena& arena) { arena.Reset(); }
};

struct ReenterInDestructor {
    explicit ReenterInDestructor(radray::ObjectArena& arena) noexcept : Arena(arena) {}
    ~ReenterInDestructor() noexcept { (void)Arena.TryAllocateBytes(1, 1); }
    radray::ObjectArena& Arena;
};

TEST(ObjectArenaTest, RejectsReclaimDuringConstruction) {
    EXPECT_DEATH({
        radray::ObjectArena arena;
        arena.Emplace<ResetInConstructor>(arena); }, "");
}

TEST(ObjectArenaTest, RejectsReentryDuringDestruction) {
    EXPECT_DEATH({
        radray::ObjectArena arena;
        arena.Emplace<ReenterInDestructor>(arena);
        arena.Reset(); }, "");
}
#endif

TEST(ArenaAllocatorTest, TraitsRebindIdentityAndZeroAllocation) {
    static_assert(!std::is_default_constructible_v<radray::ArenaAllocator<int>>);
    static_assert(!std::allocator_traits<radray::ArenaAllocator<int>>::is_always_equal::value);
    static_assert(!std::allocator_traits<radray::ArenaAllocator<int>>::propagate_on_container_copy_assignment::value);
    static_assert(!std::allocator_traits<radray::ArenaAllocator<int>>::propagate_on_container_move_assignment::value);
    static_assert(!std::allocator_traits<radray::ArenaAllocator<int>>::propagate_on_container_swap::value);
    static_assert(std::is_same_v<std::allocator_traits<radray::ArenaAllocator<int>>::rebind_alloc<char>, radray::ArenaAllocator<char>>);
    radray::ObjectArena first, second;
    radray::ArenaAllocator<int> a{first};
    radray::ArenaAllocator<char> rebound{a};
    EXPECT_EQ(a, rebound);
    EXPECT_NE(a, radray::ArenaAllocator<int>{second});
    auto copy = std::allocator_traits<decltype(a)>::select_on_container_copy_construction(a);
    EXPECT_EQ(&copy.GetArena(), &first);
    auto* empty = a.allocate(0);
    EXPECT_NE(empty, nullptr);
    const size_t bytes = first.GetReservedBytes();
    a.deallocate(empty, 0);
    EXPECT_EQ(first.GetReservedBytes(), bytes);
}

TEST(ArenaAllocatorTest, CountOverflowAndStorageFailureThrowDistinctExceptions) {
    radray::ObjectArena arena;
    radray::ArenaAllocator<uint64_t> words{arena};
    EXPECT_THROW((void)words.allocate(std::numeric_limits<size_t>::max() / sizeof(uint64_t) + 1), std::bad_array_new_length);
    radray::ArenaAllocator<radray::byte> bytes{arena};
    EXPECT_THROW((void)bytes.allocate(std::numeric_limits<size_t>::max()), std::bad_alloc);
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
    auto* value = words.allocate(1);
    words.construct(value, uint64_t{17});
    EXPECT_EQ(*value, 17u);
    std::allocator_traits<decltype(words)>::destroy(words, value);
    words.deallocate(value, 1);
}

TEST(ArenaAllocatorTest, ContainerOwnsElementsAcrossGrowthEraseAndClear) {
    Probe probe;
    radray::ObjectArena arena;
    {
        std::vector<radray::unique_ptr<Tracked>, radray::ArenaAllocator<radray::unique_ptr<Tracked>>> values{radray::ArenaAllocator<radray::unique_ptr<Tracked>>{arena}};
        for (int i = 0; i < 60; ++i) values.push_back(radray::make_unique<Tracked>(probe, i));
        EXPECT_EQ(probe.Live, 60);
        values.erase(values.begin() + 1);
        EXPECT_EQ(probe.Live, 59);
        EXPECT_EQ(probe.Destroyed, radray::vector<int>{1});
        values.clear();
        EXPECT_EQ(probe.Live, 0);
        EXPECT_EQ(probe.Destroyed.size(), 60u);
        EXPECT_GE(values.capacity(), 60u);
    }
    arena.Reset();
    EXPECT_EQ(probe.Destroyed.size(), 60u);
    std::sort(probe.Destroyed.begin(), probe.Destroyed.end());
    for (int i = 0; i < 60; ++i) EXPECT_EQ(probe.Destroyed[i], i);
}

TEST(ArenaAllocatorTest, ArenaOwnedContainerRegistersOnlyItsOwnDestructor) {
    Probe probe;
    radray::ObjectArena arena;
    auto* values = arena.Emplace<std::vector<radray::unique_ptr<Tracked>, radray::ArenaAllocator<radray::unique_ptr<Tracked>>>>(radray::ArenaAllocator<radray::unique_ptr<Tracked>>{arena});
    for (int i = 0; i < 40; ++i) values->push_back(radray::make_unique<Tracked>(probe, i));
    values->pop_back();
    EXPECT_EQ(probe.Live, 39);
    arena.Reset();
    EXPECT_EQ(probe.Live, 0);
    EXPECT_EQ(probe.Destroyed.size(), 40u);
    arena.Release();
    EXPECT_EQ(probe.Destroyed.size(), 40u);
}

TEST(ArenaAllocatorTest, NestedStringsReceiveAllocator) {
    radray::ObjectArena arena;
    {
        radray::ArenaAllocator<std::basic_string<char, std::char_traits<char>, radray::ArenaAllocator<char>>> allocator{arena};
        std::vector<decltype(allocator)::value_type, decltype(allocator)> names{allocator};
        for (int i = 0; i < 40; ++i) names.emplace_back(200, static_cast<char>('a' + i % 26));
        for (int i = 0; i < 40; ++i) {
            EXPECT_EQ(&names[i].get_allocator().GetArena(), &arena);
            EXPECT_EQ(names[i].size(), 200u);
            EXPECT_EQ(names[i].front(), static_cast<char>('a' + i % 26));
        }
    }
    arena.Reset();
}

TEST(ArenaAllocatorTest, NodeContainersAndPairsPropagateAllocator) {
    radray::ObjectArena arena;
    {
        std::list<int, radray::ArenaAllocator<int>> values{radray::ArenaAllocator<int>{arena}};
        values.push_back(1);
        values.push_back(2);
        values.pop_front();
        EXPECT_EQ(values.front(), 2);
        radray::ArenaAllocator<std::pair<const int, std::vector<int, radray::ArenaAllocator<int>>>> allocator{arena};
        std::map<int, std::vector<int, radray::ArenaAllocator<int>>, std::less<int>, decltype(allocator)> map{allocator};
        auto& child = map[7];
        child.push_back(11);
        EXPECT_EQ(&child.get_allocator().GetArena(), &arena);
        EXPECT_EQ(child.front(), 11);
        map.erase(7);
        EXPECT_TRUE(map.empty());
    }
    arena.Reset();
}

TEST(ArenaAllocatorTest, OveralignedContainerElements) {
    radray::ObjectArena arena{{128, 0}};
    {
        std::vector<AlignedValue, radray::ArenaAllocator<AlignedValue>> values{radray::ArenaAllocator<AlignedValue>{arena}};
        for (size_t i = 0; i < 40; ++i) values.push_back(AlignedValue{i});
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(values.data()) % 256, 0u);
        for (size_t i = 0; i < values.size(); ++i) EXPECT_EQ(values[i].Value, i);
    }
    arena.Reset();
    EXPECT_EQ(arena.GetReservedBytes(), 0u);
}

TEST(ArenaAllocatorTest, CopyConstructionAndAssignmentsRespectArenaIdentity) {
    radray::ObjectArena first, second;
    {
        std::vector<int, radray::ArenaAllocator<int>> source{radray::ArenaAllocator<int>{first}};
        source.assign({1, 2, 3});
        auto copy = source;
        EXPECT_EQ(&copy.get_allocator().GetArena(), &first);
        EXPECT_EQ(copy, source);
        std::vector<int, radray::ArenaAllocator<int>> target{radray::ArenaAllocator<int>{second}};
        target = source;
        EXPECT_EQ(&target.get_allocator().GetArena(), &second);
        EXPECT_EQ(target, source);
        auto* oldSource = source.data();
        target = std::move(source);
        EXPECT_EQ(&target.get_allocator().GetArena(), &second);
        EXPECT_NE(target.data(), oldSource);
        EXPECT_EQ(target.size(), 3u);
        EXPECT_EQ(target.back(), 3);
        auto moved = std::move(copy);
        EXPECT_EQ(&moved.get_allocator().GetArena(), &first);
        std::vector<int, radray::ArenaAllocator<int>> sameArena{radray::ArenaAllocator<int>{first}};
        auto* stolen = moved.data();
        sameArena = std::move(moved);
        EXPECT_EQ(sameArena.data(), stolen);
        sameArena.swap(copy);
        EXPECT_EQ(copy.data(), stolen);
    }
    first.Reset();
    second.Reset();
}

TEST(ArenaAllocatorTest, NestedCrossArenaCopyAndMoveUseDestination) {
    radray::ObjectArena first, second;
    {
        radray::ArenaAllocator<std::basic_string<char, std::char_traits<char>, radray::ArenaAllocator<char>>> allocator{first};
        std::vector<decltype(allocator)::value_type, decltype(allocator)> source{allocator};
        source.emplace_back(500, 'x');
        decltype(source) target{decltype(allocator){second}};
        target = source;
        EXPECT_EQ(&target.front().get_allocator().GetArena(), &second);
        EXPECT_EQ(target.front().size(), 500u);
        target.clear();
        target = std::move(source);
        EXPECT_EQ(&target.front().get_allocator().GetArena(), &second);
        EXPECT_EQ(target.front().size(), 500u);
    }
}

}  // namespace
