#include <radray/intrusive_ptr.h>

#include <gtest/gtest.h>

#include <latch>
#include <thread>

namespace intrusive_ptr_test {

class Incomplete;

}  // namespace intrusive_ptr_test

namespace radray {

template <>
struct IntrusivePtrTraits<intrusive_ptr_test::Incomplete> {
    static void Retain(intrusive_ptr_test::Incomplete* object) noexcept;
    static void Release(intrusive_ptr_test::Incomplete* object) noexcept;
};

}  // namespace radray

namespace intrusive_ptr_test {

radray::IntrusivePtr<Incomplete> CopyIncomplete(const radray::IntrusivePtr<Incomplete>& source) {
    return source;
}

void ResetIncomplete(radray::IntrusivePtr<Incomplete>& pointer) noexcept {
    pointer.Reset();
}

class Incomplete final {
public:
    explicit Incomplete(int& destroyed) noexcept : _destroyed(destroyed) {}
    ~Incomplete() noexcept { ++_destroyed; }
    void Hold() noexcept { ++_references; }
    void Drop() noexcept {
        if (--_references == 0) delete this;
    }

private:
    int& _destroyed;
    uint32_t _references{0};
};

}  // namespace intrusive_ptr_test

void radray::IntrusivePtrTraits<intrusive_ptr_test::Incomplete>::Retain(intrusive_ptr_test::Incomplete* object) noexcept { object->Hold(); }
void radray::IntrusivePtrTraits<intrusive_ptr_test::Incomplete>::Release(intrusive_ptr_test::Incomplete* object) noexcept { object->Drop(); }

namespace {

struct Probe {
    int Destroyed{0};
};

class Counted : public radray::RefCountedBase<Counted> {
public:
    explicit Counted(Probe& probe) noexcept : _probe(probe) {}
    ~Counted() noexcept override { ++_probe.Destroyed; }

private:
    Probe& _probe;
};

class Derived final : public Counted {
public:
    using Counted::Counted;
};

using radray::IntrusivePtr;

IntrusivePtr<Counted> MakeCounted(Probe& probe) {
    return IntrusivePtr<Counted>{new Counted{probe}};
}

TEST(IntrusivePtrTest, DefaultIsNull) {
    const IntrusivePtr<Counted> ptr;

    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.Get(), nullptr);
    EXPECT_TRUE(ptr == nullptr);
}

TEST(IntrusivePtrTest, ConstructionAcquiresFirstReference) {
    Probe probe;
    auto* raw = new Counted{probe};
    EXPECT_EQ(raw->UseCount(), 0u);
    const IntrusivePtr<Counted> ptr{raw};

    ASSERT_TRUE(ptr);
    EXPECT_EQ(ptr->UseCount(), 1u);
}

TEST(IntrusivePtrTest, ConstructionFromBorrowedPointerAddsReference) {
    Probe probe;
    const auto owner = MakeCounted(probe);
    {
        const IntrusivePtr<Counted> retained{owner.Get()};
        EXPECT_EQ(owner->UseCount(), 2u);
    }

    EXPECT_EQ(owner->UseCount(), 1u);
    EXPECT_EQ(probe.Destroyed, 0);
}

TEST(IntrusivePtrTest, CopyAddsReferenceAndDestructionReleasesIt) {
    Probe probe;
    const auto first = MakeCounted(probe);
    {
        const auto second = first;
        EXPECT_EQ(second.Get(), first.Get());
        EXPECT_EQ(first->UseCount(), 2u);
    }

    EXPECT_EQ(first->UseCount(), 1u);
}

TEST(IntrusivePtrTest, MoveTransfersReferenceWithoutCounting) {
    Probe probe;
    auto source = MakeCounted(probe);
    Counted* raw = source.Get();

    const auto target = std::move(source);

    EXPECT_FALSE(source);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(target.Get(), raw);
    EXPECT_EQ(target->UseCount(), 1u);
}

TEST(IntrusivePtrTest, LastReleaseDestroysObject) {
    Probe probe;
    {
        const auto first = MakeCounted(probe);
        const auto second = first;
    }

    EXPECT_EQ(probe.Destroyed, 1);
}

TEST(IntrusivePtrTest, ResetAndNullAssignmentReleaseOnce) {
    Probe probe;
    auto reset = MakeCounted(probe);
    auto assigned = MakeCounted(probe);

    reset.Reset();
    reset.Reset();
    assigned = nullptr;

    EXPECT_FALSE(reset);
    EXPECT_FALSE(assigned);
    EXPECT_EQ(probe.Destroyed, 2);
}

TEST(IntrusivePtrTest, NullableConstructionAndEmptyCopiesRemainEmpty) {
    const IntrusivePtr<Counted> ptr{radray::Nullable<Counted*>{nullptr}};
    auto copy = ptr;
    IntrusivePtr<Counted> moved{std::move(copy)};
    EXPECT_FALSE(ptr);
    EXPECT_FALSE(copy);  // NOLINT(bugprone-use-after-move)
    EXPECT_FALSE(moved);
}

TEST(IntrusivePtrTest, SelfAssignmentKeepsObjectAlive) {
    Probe probe;
    auto ptr = MakeCounted(probe);
    const auto& alias = ptr;

    ptr = alias;

    ASSERT_TRUE(ptr);
    EXPECT_EQ(ptr->UseCount(), 1u);
    EXPECT_EQ(probe.Destroyed, 0);
}

TEST(IntrusivePtrTest, ConvertsToBaseSharingTheSameCount) {
    Probe probe;
    IntrusivePtr<Derived> derived{new Derived{probe}};

    IntrusivePtr<Counted> copied = derived;
    EXPECT_EQ(copied.Get(), derived.Get());
    EXPECT_EQ(derived->UseCount(), 2u);

    IntrusivePtr<Counted> moved = std::move(derived);
    EXPECT_FALSE(derived);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(moved->UseCount(), 2u);

    copied.Reset();
    moved.Reset();
    EXPECT_EQ(probe.Destroyed, 1);
}

TEST(IntrusivePtrTest, ComparesAndHashesByPointer) {
    Probe probe;
    const auto first = MakeCounted(probe);
    const auto alias = first;
    const auto second = MakeCounted(probe);

    radray::unordered_set<IntrusivePtr<Counted>> set{first, alias, second};

    EXPECT_TRUE(first == alias);
    EXPECT_FALSE(first == second);
    EXPECT_EQ(first <=> alias, std::strong_ordering::equal);
    EXPECT_EQ(set.size(), 2u);
}

TEST(IntrusivePtrTest, SwapExchangesObjects) {
    Probe probe;
    auto first = MakeCounted(probe);
    auto second = MakeCounted(probe);
    Counted* firstRaw = first.Get();
    Counted* secondRaw = second.Get();

    swap(first, second);

    EXPECT_EQ(first.Get(), secondRaw);
    EXPECT_EQ(second.Get(), firstRaw);
}

TEST(IntrusivePtrTest, TraitsAllowReferenceOperationsBeforeTypeDefinition) {
    int destroyed = 0;
    IntrusivePtr<intrusive_ptr_test::Incomplete> pointer{new intrusive_ptr_test::Incomplete{destroyed}};
    auto copy = intrusive_ptr_test::CopyIncomplete(pointer);
    intrusive_ptr_test::ResetIncomplete(pointer);
    EXPECT_EQ(destroyed, 0);
    intrusive_ptr_test::ResetIncomplete(copy);
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtrTest, ConstConversionKeepsTheSameObjectAlive) {
    Probe probe;
    auto pointer = MakeCounted(probe);
    IntrusivePtr<const Counted> constant = pointer;
    pointer.Reset();
    EXPECT_EQ(probe.Destroyed, 0);
    EXPECT_EQ(constant->UseCount(), 1u);
    constant.Reset();
    EXPECT_EQ(probe.Destroyed, 1);
}

TEST(IntrusivePtrTest, SelfMoveKeepsObjectAlive) {
    Probe probe;
    auto pointer = MakeCounted(probe);
    auto& alias = pointer;
    pointer = std::move(alias);
    ASSERT_TRUE(pointer);
    EXPECT_EQ(pointer->UseCount(), 1u);
    EXPECT_EQ(probe.Destroyed, 0);
}

class Reentrant final : public radray::RefCountedBase<Reentrant> {
public:
    Reentrant(IntrusivePtr<Reentrant>& owner, Probe& probe) noexcept : _owner(owner), _probe(probe) {}
    ~Reentrant() noexcept override {
        EXPECT_FALSE(_owner);
        _owner.Reset();
        ++_probe.Destroyed;
    }

private:
    IntrusivePtr<Reentrant>& _owner;
    Probe& _probe;
};

TEST(IntrusivePtrTest, ResetClearsPointerBeforeReentrantDestruction) {
    Probe probe;
    IntrusivePtr<Reentrant> pointer;
    pointer = IntrusivePtr<Reentrant>{new Reentrant{pointer, probe}};
    pointer.Reset();
    EXPECT_EQ(probe.Destroyed, 1);
}

class ThreadCounted final : public radray::ThreadSafeRefCountedBase<ThreadCounted> {
public:
    explicit ThreadCounted(Probe& probe) noexcept : _probe(probe) {}
    ~ThreadCounted() noexcept override { ++_probe.Destroyed; }

private:
    Probe& _probe;
};

TEST(IntrusivePtrTest, ConcurrentOwnersReleaseExactlyOnce) {
    Probe probe;
    auto* raw = new ThreadCounted{probe};
    EXPECT_EQ(raw->UseCount(), 0u);
    IntrusivePtr<ThreadCounted> owner{raw};
    std::latch start{1};
    radray::vector<std::jthread> threads;
    for (uint32_t i = 0; i < 8; ++i) {
        threads.emplace_back([pointer = owner, &start]() mutable {
            start.wait();
            for (uint32_t iteration = 0; iteration < 1000; ++iteration) {
                auto copy = pointer;
                copy.Reset();
            }
            pointer.Reset();
        });
    }
    owner.Reset();
    start.count_down();
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(probe.Destroyed, 1);
}

TEST(IntrusivePtrDeathTest, RejectsReleaseWithoutAReference) {
    Probe probe;
    Counted counted{probe};
    ThreadCounted threaded{probe};
    EXPECT_DEATH(counted.Release(), "");
    EXPECT_DEATH(threaded.Release(), "");
}

TEST(IntrusivePtrDeathTest, RejectsDestructionWithOutstandingReferences) {
    Probe probe;
    EXPECT_DEATH({ Counted counted{probe}; counted.Retain(); }, "");
    EXPECT_DEATH({ ThreadCounted counted{probe}; counted.Retain(); }, "");
}

}  // namespace
