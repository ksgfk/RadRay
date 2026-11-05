#include <radray/inline_vector.h>

#include <cstdlib>
#include <iterator>
#include <new>
#include <sstream>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <malloc.h>
#endif

using radray::InlineVector;

// This executable replaces global new/delete so tests can count calling-thread allocations.
namespace allocation_counter {
thread_local bool Enabled = false;
thread_local size_t Count = 0;

void* Allocate(size_t size) {
    if (Enabled) ++Count;
    if (void* result = std::malloc(std::max(size, size_t{1}))) return result;
    std::abort();
}

void* AllocateAligned(size_t size, size_t alignment) {
    if (Enabled) ++Count;
    size = std::max(size, size_t{1});
#ifdef _WIN32
    void* result = _aligned_malloc(size, alignment);
#else
    void* result = std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
    if (result != nullptr) return result;
    std::abort();
}

void FreeAligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}

template <class F>
size_t Measure(F&& callback) {
    Count = 0;
    Enabled = true;
    callback();
    Enabled = false;
    return Count;
}
}  // namespace allocation_counter

void* operator new(size_t size) { return allocation_counter::Allocate(size); }
void* operator new[](size_t size) { return allocation_counter::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return allocation_counter::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return allocation_counter::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { allocation_counter::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { allocation_counter::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { allocation_counter::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { allocation_counter::FreeAligned(value); }

namespace {

struct LifetimeProbe {
    int Alive{0};
    int Copies{0};
    int Moves{0};
};

// Not default constructible or assignable; reports construction and destruction to a probe.
struct Tracked {
    Tracked(LifetimeProbe& probe, int value) noexcept : Probe(&probe), Value(value) { ++Probe->Alive; }
    Tracked(const Tracked& other) noexcept : Probe(other.Probe), Value(other.Value) {
        ++Probe->Alive;
        ++Probe->Copies;
    }
    Tracked(Tracked&& other) noexcept : Probe(other.Probe), Value(std::exchange(other.Value, -1)) {
        ++Probe->Alive;
        ++Probe->Moves;
    }
    Tracked& operator=(const Tracked&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() noexcept { --Probe->Alive; }

    LifetimeProbe* Probe;
    int Value;
};

struct ThrowingMove {
    ThrowingMove(LifetimeProbe& probe, int value) noexcept : Probe(&probe), Value(value) {}
    ThrowingMove(const ThrowingMove& other) noexcept : Probe(other.Probe), Value(other.Value) { ++Probe->Copies; }
    ThrowingMove(ThrowingMove&& other) noexcept(false) : Probe(other.Probe), Value(other.Value) { ++Probe->Moves; }

    LifetimeProbe* Probe;
    int Value;
};

struct MoveOnly {
    explicit MoveOnly(int value) : Value(radray::make_unique<int>(value)) {}
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) = delete;
    radray::unique_ptr<int> Value;
};

struct alignas(128) OverAligned {
    explicit OverAligned(int value) noexcept : Value(value) {}
    int Value;
};

template <class T, size_t N>
bool IsAligned(const InlineVector<T, N>& values) {
    return reinterpret_cast<std::uintptr_t>(values.data()) % alignof(T) == 0;
}

static_assert(std::is_nothrow_move_constructible_v<InlineVector<MoveOnly, 2>>);
static_assert(!std::is_nothrow_move_constructible_v<InlineVector<ThrowingMove, 2>>);

TEST(InlineVectorTest, EmptyVectorUsesInlineCapacity) {
    const InlineVector<int, 3> values{};

    EXPECT_TRUE(values.empty());
    EXPECT_EQ(values.begin(), values.end());
    EXPECT_EQ(values.capacity(), 3u);
}

TEST(InlineVectorTest, GrowingPastInlineCapacityPreservesOrder) {
    InlineVector<int, 2> values;

    for (int i = 0; i < 6; ++i) values.push_back(i);

    ASSERT_EQ(values.size(), 6u);
    EXPECT_GE(values.capacity(), 6u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(values[static_cast<size_t>(i)], i);
    EXPECT_EQ(values.front(), 0);
    EXPECT_EQ(values.back(), 5);
}

TEST(InlineVectorTest, InlineOperationsDoNotAllocate) {
    int result = 0;

    const size_t allocations = allocation_counter::Measure([&] {
        InlineVector<InlineVector<int, 2>, 3> groups;
        groups = {{1, 2}, {3}, {4}};
        auto copy = groups;
        auto moved = std::move(copy);
        copy = moved;
        groups = std::move(copy);
        groups.pop_back();
        groups.emplace_back(InlineVector<int, 2>{5});
        result = groups[0][1] + groups[2][0];
    });

    EXPECT_EQ(result, 7);
    EXPECT_EQ(allocations, 0u);
}

TEST(InlineVectorTest, AppendWithinCapacityKeepsReferencesValid) {
    InlineVector<int, 4> values{1};
    const int* first = &values.front();

    values.push_back(2);
    values.push_back(3);

    EXPECT_EQ(&values.front(), first);
}

TEST(InlineVectorTest, PopBackAndClearRetainHeapCapacity) {
    InlineVector<int, 2> values;
    values.reserve(16);
    const int* storage = values.data();

    const size_t allocations = allocation_counter::Measure([&] {
        for (int i = 0; i < 16; ++i) values.push_back(i);
        for (int i = 0; i < 14; ++i) values.pop_back();
        values.clear();
        for (int i = 0; i < 16; ++i) values.push_back(i);
    });

    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(values.data(), storage);
    EXPECT_EQ(values.back(), 15);
}

TEST(InlineVectorTest, ReserveWithinCapacityKeepsStorage) {
    InlineVector<int, 4> values{1, 2};
    const int* storage = values.data();

    const size_t allocations = allocation_counter::Measure([&] { values.reserve(4); });

    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(values.data(), storage);
}

TEST(InlineVectorTest, OnlyLiveElementsAreConstructed) {
    LifetimeProbe probe;
    {
        InlineVector<Tracked, 3> values;
        values.reserve(12);
        EXPECT_EQ(probe.Alive, 0);

        values.emplace_back(probe, 1);
        values.emplace_back(probe, 2);
        EXPECT_EQ(probe.Alive, 2);

        values.pop_back();
        EXPECT_EQ(probe.Alive, 1);
        values.emplace_back(probe, 3);
    }
    EXPECT_EQ(probe.Alive, 0);
}

TEST(InlineVectorTest, ClearDestroysElements) {
    LifetimeProbe probe;
    InlineVector<Tracked, 2> values;
    for (int i = 0; i < 5; ++i) values.emplace_back(probe, i);

    values.clear();

    EXPECT_TRUE(values.empty());
    EXPECT_EQ(probe.Alive, 0);
}

TEST(InlineVectorTest, EmplaceBackMayReferenceAnExistingElementDuringGrowth) {
    InlineVector<radray::string, 2> values{"alpha", "beta"};

    values.emplace_back(values.front());
    while (values.size() < values.capacity()) values.emplace_back("padding");
    values.emplace_back(values.front().data(), values.front().size());
    while (values.size() < values.capacity()) values.emplace_back("padding");
    values.emplace_back(std::move(values.front()));

    EXPECT_EQ(values[2], "alpha");
    EXPECT_EQ(values[values.size() - 1], "alpha");
}

TEST(InlineVectorTest, AssignAcceptsOwnSubrange) {
    InlineVector<radray::string, 2> values{"a", "b", "c", "d"};

    values.assign(values.begin() + 1, values.end() - 1);
    EXPECT_EQ(values, (InlineVector<radray::string, 2>{"b", "c"}));

    values.assign(std::make_reverse_iterator(values.end()), std::make_reverse_iterator(values.begin()));
    EXPECT_EQ(values, (InlineVector<radray::string, 2>{"c", "b"}));
}

TEST(InlineVectorTest, AssignAcceptsInputIterators) {
    std::istringstream input{"1 2 3 4"};
    InlineVector<int, 2> values;

    values.assign(std::istream_iterator<int>{input}, std::istream_iterator<int>{});

    EXPECT_EQ(values, (InlineVector<int, 2>{1, 2, 3, 4}));
}

TEST(InlineVectorTest, CopyIsIndependentOfSource) {
    InlineVector<int, 2> source{1, 2, 3};

    InlineVector<int, 2> copy = source;
    copy.push_back(4);

    EXPECT_EQ(source, (InlineVector<int, 2>{1, 2, 3}));
    EXPECT_EQ(copy, (InlineVector<int, 2>{1, 2, 3, 4}));
}

TEST(InlineVectorTest, CopyAssignmentReplacesContentsAcrossStorageKinds) {
    LifetimeProbe probe;
    InlineVector<Tracked, 2> small;
    small.emplace_back(probe, 1);
    InlineVector<Tracked, 2> large;
    for (int i = 0; i < 5; ++i) large.emplace_back(probe, 10 + i);

    large = small;
    ASSERT_EQ(large.size(), 1u);
    EXPECT_EQ(large[0].Value, 1);

    small = InlineVector<Tracked, 2>{Tracked{probe, 7}, Tracked{probe, 8}, Tracked{probe, 9}};
    ASSERT_EQ(small.size(), 3u);
    EXPECT_EQ(small[2].Value, 9);
}

TEST(InlineVectorTest, SelfAssignmentKeepsContents) {
    InlineVector<radray::string, 2> values{"a", "b", "c"};
    auto& alias = values;

    values = alias;
    values = std::move(alias);

    EXPECT_EQ(values, (InlineVector<radray::string, 2>{"a", "b", "c"}));
}

TEST(InlineVectorTest, MoveTransfersHeapStorageAndLeavesSourceReusable) {
    InlineVector<MoveOnly, 2> source;
    for (int i = 0; i < 5; ++i) source.emplace_back(i);
    const MoveOnly* storage = source.data();

    InlineVector<MoveOnly, 2> moved = std::move(source);

    EXPECT_EQ(moved.data(), storage);
    EXPECT_EQ(*moved[4].Value, 4);
    EXPECT_TRUE(source.empty());  // NOLINT(bugprone-use-after-move)
    source.emplace_back(9);
    EXPECT_EQ(*source.front().Value, 9);
}

TEST(InlineVectorTest, MoveFromInlineStorageMovesElements) {
    LifetimeProbe probe;
    InlineVector<Tracked, 4> source;
    source.emplace_back(probe, 1);
    source.emplace_back(probe, 2);

    InlineVector<Tracked, 4> moved;
    moved = std::move(source);

    ASSERT_EQ(moved.size(), 2u);
    EXPECT_EQ(moved[1].Value, 2);
    EXPECT_TRUE(source.empty());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(probe.Alive, 2);
}

TEST(InlineVectorTest, GrowthCopiesElementsWhoseMoveMayThrow) {
    LifetimeProbe probe;
    InlineVector<ThrowingMove, 2> values;
    values.emplace_back(probe, 1);
    values.emplace_back(probe, 2);

    values.emplace_back(probe, 3);

    EXPECT_EQ(probe.Moves, 0);
    EXPECT_EQ(probe.Copies, 2);
    EXPECT_EQ(values[0].Value, 1);
    EXPECT_EQ(values[2].Value, 3);
}

TEST(InlineVectorTest, OverAlignedElementsStayAlignedAcrossGrowth) {
    InlineVector<OverAligned, 2> values;
    EXPECT_TRUE(IsAligned(values));

    for (int i = 0; i < 9; ++i) values.emplace_back(i);
    const auto moved = std::move(values);

    EXPECT_TRUE(IsAligned(moved));
    EXPECT_TRUE(IsAligned(values));  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(moved[8].Value, 8);
}

TEST(InlineVectorTest, ReserveBeyondMaxSizeAborts) {
    using Vector = InlineVector<int, 2>;
    EXPECT_DEATH((Vector{}.reserve(Vector::max_size() + 1)), "");
}

}  // namespace
