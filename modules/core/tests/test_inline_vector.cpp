#include <gtest/gtest.h>
#include <radray/inline_vector.h>

#include <cstdlib>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <sstream>
#include <type_traits>
#ifdef _WIN32
#include <malloc.h>
#endif

using radray::InlineVector;

// Only this executable counts calling-thread C++ allocations, outside gtest assertions.
namespace inline_vector_allocations {
thread_local bool Enabled = false;
thread_local size_t Count = 0;
void* Allocate(size_t size) {
    if (Enabled) ++Count;
    if (void* result = std::malloc(std::max(size, size_t{1}))) return result;
    std::abort();
}
void* AllocateAligned(size_t size, size_t alignment) {
    if (Enabled) ++Count;
#ifdef _WIN32
    void* result = _aligned_malloc(std::max(size, size_t{1}), alignment);
#else
    void* result = std::aligned_alloc(alignment, ((std::max(size, size_t{1}) + alignment - 1) / alignment) * alignment);
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
}  // namespace inline_vector_allocations

void* operator new(size_t size) { return inline_vector_allocations::Allocate(size); }
void* operator new[](size_t size) { return inline_vector_allocations::Allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) { return inline_vector_allocations::AllocateAligned(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return inline_vector_allocations::AllocateAligned(size, size_t(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { inline_vector_allocations::FreeAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { inline_vector_allocations::FreeAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { inline_vector_allocations::FreeAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { inline_vector_allocations::FreeAligned(value); }

namespace {
struct TrackedValue {
    static inline int Alive = 0;
    static inline int Constructed = 0;
    static inline int Destroyed = 0;
    explicit TrackedValue(int value) noexcept : Value(value) {
        ++Alive;
        ++Constructed;
    }
    TrackedValue(const TrackedValue& other) noexcept : TrackedValue(other.Value) {}
    TrackedValue(TrackedValue&& other) noexcept : TrackedValue(std::exchange(other.Value, -1)) {}
    TrackedValue& operator=(const TrackedValue&) = delete;
    TrackedValue& operator=(TrackedValue&&) = delete;
    ~TrackedValue() noexcept {
        --Alive;
        ++Destroyed;
    }
    int Value;
};

struct MoveOnlyValue {
    explicit MoveOnlyValue(int value) : Value(radray::make_unique<int>(value)) {}
    MoveOnlyValue(MoveOnlyValue&&) noexcept = default;
    MoveOnlyValue(const MoveOnlyValue&) = delete;
    MoveOnlyValue& operator=(MoveOnlyValue&&) = delete;
    MoveOnlyValue& operator=(const MoveOnlyValue&) = delete;
    radray::unique_ptr<int> Value;
};

struct CopyPreferredValue {
    static inline int Copies = 0;
    static inline int Moves = 0;
    explicit CopyPreferredValue(int value) noexcept : Value(value) {}
    CopyPreferredValue(const CopyPreferredValue& other) noexcept : Value(other.Value) { ++Copies; }
    CopyPreferredValue(CopyPreferredValue&& other) noexcept(false) : Value(std::exchange(other.Value, -1)) { ++Moves; }
    int Value;
};

struct alignas(128) AlignedValue {
    explicit AlignedValue(int value) noexcept : Value(value) {}
    int Value;
};

static_assert(std::is_nothrow_move_constructible_v<InlineVector<MoveOnlyValue, 2>>);
static_assert(std::is_nothrow_move_assignable_v<InlineVector<MoveOnlyValue, 2>>);
static_assert(!std::is_copy_constructible_v<InlineVector<MoveOnlyValue, 2>>);
static_assert(!std::is_nothrow_move_constructible_v<InlineVector<CopyPreferredValue, 2>>);
static_assert(!std::is_nothrow_move_assignable_v<InlineVector<CopyPreferredValue, 2>>);
}  // namespace

TEST(InlineVectorTest, EmptyAndInitializerList) {
    InlineVector<int, 3> empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_EQ(empty.begin(), empty.end());

    InlineVector<int, 3> three{1, 2, 3};
    EXPECT_EQ(three.size(), 3u);
    EXPECT_EQ(three[0], 1);
    EXPECT_EQ(three.front(), 1);
    EXPECT_EQ(three.back(), 3);
    EXPECT_EQ(three.data(), &three[0]);

    InlineVector<int, 2> spilled{1, 2, 3, 4};
    EXPECT_EQ(spilled.size(), 4u);
    EXPECT_EQ(spilled[0], 1);
    EXPECT_EQ(spilled[3], 4);
}

TEST(InlineVectorTest, SpillAndPopBackPreserveOrder) {
    InlineVector<int, 2> v;
    for (int i = 0; i < 6; ++i) v.push_back(i);
    ASSERT_EQ(v.size(), 6u);
    for (int i = 0; i < 6; ++i) EXPECT_EQ(v[static_cast<size_t>(i)], i);

    const auto* storage = v.data();
    const auto capacity = v.capacity();
    // Shrinking keeps the allocation and leaves references to surviving elements valid.
    v.pop_back();
    v.pop_back();
    v.pop_back();
    v.pop_back();
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 1);
    EXPECT_EQ(v.data(), storage);
    EXPECT_EQ(v.capacity(), capacity);
    v.pop_back();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0);

    // Regrow after shrinking.
    v.push_back(10);
    v.push_back(20);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 10);
    EXPECT_EQ(v[2], 20);
}

TEST(InlineVectorTest, EmplaceBackAliasingElementSurvivesSpill) {
    InlineVector<radray::string, 2> v{"alpha", "beta"};
    v.emplace_back(v[0]);  // argument aliases an inline element that is moved during the spill
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "alpha");
    EXPECT_EQ(v[1], "beta");
    EXPECT_EQ(v[2], "alpha");
}

TEST(InlineVectorTest, ClearReleasesElementOwnership) {
    InlineVector<radray::shared_ptr<int>, 2> v;
    auto tracked = radray::make_shared<int>(1);
    v.push_back(tracked);
    EXPECT_EQ(tracked.use_count(), 2);
    v.clear();
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(tracked.use_count(), 1);

    v.push_back(tracked);
    v.push_back(tracked);
    v.push_back(tracked);
    EXPECT_EQ(tracked.use_count(), 4);
    v.clear();
    EXPECT_EQ(tracked.use_count(), 1);
}

TEST(InlineVectorTest, CopyMoveAndEquality) {
    InlineVector<int, 2> a{1, 2, 3};
    InlineVector<int, 2> b = a;
    EXPECT_EQ(a, b);
    b.push_back(4);
    EXPECT_NE(a, b);

    InlineVector<int, 2> c = std::move(b);
    EXPECT_EQ(c.size(), 4u);
    EXPECT_EQ(c[3], 4);
    EXPECT_TRUE(b.empty());  // NOLINT(bugprone-use-after-move)

    InlineVector<int, 2> d;
    d = std::move(c);
    EXPECT_EQ(d.size(), 4u);
    EXPECT_TRUE(c.empty());  // NOLINT(bugprone-use-after-move)

    InlineVector<int, 2> e{1, 2};
    InlineVector<int, 2> f{1, 2};
    EXPECT_EQ(e, f);
    e = {7};
    EXPECT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0], 7);
}

TEST(InlineVectorTest, SpanView) {
    InlineVector<int, 2> v{5, 6, 7};
    std::span<const int> view{v.data(), v.size()};
    ASSERT_EQ(view.size(), 3u);
    EXPECT_EQ(view[2], 7);
    std::span<const int> fromRange{v.begin(), v.end()};
    EXPECT_EQ(fromRange.size(), 3u);
}

TEST(InlineVectorTest, NestedInlineOperationsDoNotAllocate) {
    int result = 0;
    const auto allocations = inline_vector_allocations::Measure([&] {
        InlineVector<InlineVector<int, 2>, 3> groups;
        groups = {{1, 2}, {3}, {4}};
        auto copy = groups;
        auto moved = std::move(copy);
        copy = moved;
        groups = std::move(copy);
        result = groups[0][1] + groups[2][0];
        moved.pop_back();
        moved.clear();
        moved.emplace_back(InlineVector<int, 2>{5});
        result += moved.front().front();
    });
    EXPECT_EQ(result, 11);
    EXPECT_EQ(allocations, 0u);
}

TEST(InlineVectorTest, OnlyLiveElementsAreConstructedAndDestroyed) {
    TrackedValue::Alive = TrackedValue::Constructed = TrackedValue::Destroyed = 0;
    {
        InlineVector<TrackedValue, 3> values;
        EXPECT_EQ(TrackedValue::Constructed, 0);
        values.reserve(12);
        EXPECT_EQ(TrackedValue::Constructed, 0);
        values.emplace_back(7);
        values.emplace_back(8);
        EXPECT_EQ(TrackedValue::Alive, 2);
        auto copy = values;
        EXPECT_EQ(TrackedValue::Alive, 4);
        values.pop_back();
        EXPECT_EQ(TrackedValue::Alive, 3);
        values.clear();
        EXPECT_EQ(TrackedValue::Alive, 2);
        EXPECT_EQ(copy[0].Value, 7);
        EXPECT_EQ(copy[1].Value, 8);
    }
    EXPECT_EQ(TrackedValue::Alive, 0);
    EXPECT_EQ(TrackedValue::Constructed, TrackedValue::Destroyed);
}

TEST(InlineVectorTest, MoveOnlyNonDefaultNonAssignableElements) {
    InlineVector<MoveOnlyValue, 2> values;
    for (int i = 0; i < 7; ++i) values.emplace_back(i);
    for (size_t i = 0; i < values.size(); ++i) EXPECT_EQ(*values[i].Value, int(i));
    auto* storage = values.data();
    auto moved = std::move(values);
    EXPECT_EQ(moved.data(), storage);
    EXPECT_TRUE(values.empty());
    EXPECT_EQ(values.capacity(), 2u);
    values.emplace_back(99);
    EXPECT_EQ(*values.front().Value, 99);
    moved = std::move(values);
    ASSERT_EQ(moved.size(), 1u);
    EXPECT_EQ(*moved.front().Value, 99);
    EXPECT_TRUE(values.empty());
    moved.pop_back();
    EXPECT_TRUE(moved.empty());
}

TEST(InlineVectorTest, CopyMoveAssignmentAcrossStorageStatesAndSelfAssignment) {
    TrackedValue::Alive = TrackedValue::Constructed = TrackedValue::Destroyed = 0;
    for (size_t sourceSize : {size_t{0}, size_t{1}, size_t{2}, size_t{7}}) {
        for (size_t destinationSize : {size_t{0}, size_t{1}, size_t{2}, size_t{7}}) {
            InlineVector<TrackedValue, 2> source;
            InlineVector<TrackedValue, 2> destination;
            for (size_t i = 0; i < sourceSize; ++i) source.emplace_back(int(i));
            for (size_t i = 0; i < destinationSize; ++i) destination.emplace_back(-int(i) - 1);
            destination = source;
            ASSERT_EQ(destination.size(), sourceSize);
            for (size_t i = 0; i < sourceSize; ++i) EXPECT_EQ(destination[i].Value, int(i));
            auto& alias = destination;
            destination = alias;
            destination = std::move(alias);
            ASSERT_EQ(destination.size(), sourceSize);
            destination = std::move(source);
            EXPECT_TRUE(source.empty());
            ASSERT_EQ(destination.size(), sourceSize);
            for (size_t i = 0; i < sourceSize; ++i) EXPECT_EQ(destination[i].Value, int(i));
            source.emplace_back(42);
            EXPECT_EQ(source.front().Value, 42);
        }
    }
    EXPECT_EQ(TrackedValue::Alive, 0);
    EXPECT_EQ(TrackedValue::Constructed, TrackedValue::Destroyed);
}

TEST(InlineVectorTest, ReserveAndClearReuseHeapCapacity) {
    InlineVector<int, 2> values{1, 2};
    auto* inlineStorage = values.data();
    EXPECT_EQ(inline_vector_allocations::Measure([&] { values.reserve(2); }), 0u);
    EXPECT_EQ(values.data(), inlineStorage);
    EXPECT_EQ(inline_vector_allocations::Measure([&] { values.reserve(16); }), 1u);
    EXPECT_GE(values.capacity(), 16u);
    auto* heapStorage = values.data();
    const auto allocations = inline_vector_allocations::Measure([&] {
        values.clear();
        for (int i = 0; i < 16; ++i) values.emplace_back(i);
        for (int i = 0; i < 14; ++i) values.pop_back();
        for (int i = 2; i < 16; ++i) values.emplace_back(i);
    });
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(values.data(), heapStorage);
    EXPECT_EQ(values.front(), 0);
    EXPECT_EQ(values.back(), 15);
}

TEST(InlineVectorTest, AliasedEmplacementAcrossInlineAndHeapGrowth) {
    InlineVector<radray::string, 2> values{"alpha", "beta"};
    values.emplace_back(values.front().data(), values.front().size());
    EXPECT_EQ(values.back(), "alpha");
    while (values.size() < values.capacity()) values.emplace_back("padding");
    values.emplace_back(values.front());
    EXPECT_EQ(values.front(), "alpha");
    EXPECT_EQ(values.back(), "alpha");
    while (values.size() < values.capacity()) values.emplace_back("padding");
    values.emplace_back(std::move(values.front()));
    EXPECT_EQ(values.back(), "alpha");
}

TEST(InlineVectorTest, AssignHandlesSelfRangesAndInputIterators) {
    InlineVector<radray::string, 2> values{"a", "b", "c", "d"};
    values.assign(values.begin() + 1, values.end() - 1);
    EXPECT_EQ(values, (InlineVector<radray::string, 2>{"b", "c"}));
    values.assign(std::make_reverse_iterator(values.end()), std::make_reverse_iterator(values.begin()));
    EXPECT_EQ(values, (InlineVector<radray::string, 2>{"c", "b"}));
    values.assign(values.end(), values.end());
    EXPECT_TRUE(values.empty());
    std::istringstream input{"1 2 3 4"};
    InlineVector<int, 2> numbers;
    numbers.assign(std::istream_iterator<int>{input}, std::istream_iterator<int>{});
    EXPECT_EQ(numbers, (InlineVector<int, 2>{1, 2, 3, 4}));
}

TEST(InlineVectorTest, OverAlignedElementsKeepAlignmentAcrossStorageChanges) {
    InlineVector<AlignedValue, 2> values;
    const auto aligned = [](const auto& vector) { return reinterpret_cast<std::uintptr_t>(vector.data()) % alignof(AlignedValue) == 0; };
    EXPECT_TRUE(aligned(values));
    for (int i = 0; i < 9; ++i) {
        values.emplace_back(i);
        EXPECT_TRUE(aligned(values));
    }
    auto copy = values;
    auto moved = std::move(copy);
    EXPECT_TRUE(aligned(copy));
    EXPECT_TRUE(aligned(moved));
    for (size_t i = 0; i < moved.size(); ++i) EXPECT_EQ(moved[i].Value, int(i));
}

TEST(InlineVectorTest, GrowthCopiesWhenElementMoveIsPotentiallyThrowing) {
    CopyPreferredValue::Copies = CopyPreferredValue::Moves = 0;
    InlineVector<CopyPreferredValue, 2> values;
    values.emplace_back(1);
    values.emplace_back(2);
    values.emplace_back(3);
    EXPECT_EQ(CopyPreferredValue::Copies, 2);
    EXPECT_EQ(CopyPreferredValue::Moves, 0);
    values.reserve(20);
    EXPECT_EQ(CopyPreferredValue::Copies, 5);
    EXPECT_EQ(CopyPreferredValue::Moves, 0);
    EXPECT_EQ(values[0].Value, 1);
    EXPECT_EQ(values[2].Value, 3);
}

TEST(InlineVectorTest, OversizedReserveTerminates) {
    EXPECT_DEATH((InlineVector<int, 2>{}.reserve(InlineVector<int, 2>::max_size() + 1)), "");
}
