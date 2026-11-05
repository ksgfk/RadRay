#include <radray/intrusive_ptr.h>

#include <gtest/gtest.h>

namespace {

struct Probe {
    int Destroyed{0};
};

// The count starts at one so a new object is handed over with AdoptRef.
class Counted {
public:
    explicit Counted(Probe& probe) noexcept : _probe(probe) {}
    virtual ~Counted() noexcept { ++_probe.Destroyed; }

    uint32_t RefCount() const noexcept { return _refCount; }

private:
    friend void IntrusivePtrAddRef(const Counted* object) noexcept { ++object->_refCount; }
    friend void IntrusivePtrRelease(Counted* object) noexcept {
        if (--object->_refCount == 0) delete object;
    }

    Probe& _probe;
    mutable uint32_t _refCount{1};
};

class Derived final : public Counted {
public:
    using Counted::Counted;
};

using radray::AdoptRef;
using radray::IntrusivePtr;
using radray::RetainRef;

IntrusivePtr<Counted> MakeCounted(Probe& probe) {
    return AdoptRef(new Counted{probe});
}

TEST(IntrusivePtrTest, DefaultIsNull) {
    const IntrusivePtr<Counted> ptr;

    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.Get(), nullptr);
    EXPECT_TRUE(ptr == nullptr);
}

TEST(IntrusivePtrTest, AdoptRefTakesExistingReference) {
    Probe probe;

    const auto ptr = MakeCounted(probe);

    ASSERT_TRUE(ptr);
    EXPECT_EQ(ptr->RefCount(), 1u);
}

TEST(IntrusivePtrTest, RetainRefAddsReference) {
    Probe probe;
    const auto owner = MakeCounted(probe);
    {
        const auto retained = RetainRef(owner.Get());
        EXPECT_EQ(owner->RefCount(), 2u);
    }

    EXPECT_EQ(owner->RefCount(), 1u);
    EXPECT_EQ(probe.Destroyed, 0);
}

TEST(IntrusivePtrTest, CopyAddsReferenceAndDestructionReleasesIt) {
    Probe probe;
    const auto first = MakeCounted(probe);
    {
        const auto second = first;
        EXPECT_EQ(second.Get(), first.Get());
        EXPECT_EQ(first->RefCount(), 2u);
    }

    EXPECT_EQ(first->RefCount(), 1u);
}

TEST(IntrusivePtrTest, MoveTransfersReferenceWithoutCounting) {
    Probe probe;
    auto source = MakeCounted(probe);
    Counted* raw = source.Get();

    const auto target = std::move(source);

    EXPECT_FALSE(source);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(target.Get(), raw);
    EXPECT_EQ(target->RefCount(), 1u);
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

TEST(IntrusivePtrTest, ReleaseHandsReferenceToCaller) {
    Probe probe;
    auto ptr = MakeCounted(probe);

    Counted* raw = ptr.Release();
    ptr.Reset();
    EXPECT_EQ(probe.Destroyed, 0);

    { const auto reclaimed = AdoptRef(raw); }
    EXPECT_EQ(probe.Destroyed, 1);
}

TEST(IntrusivePtrTest, SelfAssignmentKeepsObjectAlive) {
    Probe probe;
    auto ptr = MakeCounted(probe);
    const auto& alias = ptr;

    ptr = alias;

    ASSERT_TRUE(ptr);
    EXPECT_EQ(ptr->RefCount(), 1u);
    EXPECT_EQ(probe.Destroyed, 0);
}

TEST(IntrusivePtrTest, ConvertsToBaseSharingTheSameCount) {
    Probe probe;
    IntrusivePtr<Derived> derived = AdoptRef(new Derived{probe});

    IntrusivePtr<Counted> copied = derived;
    EXPECT_EQ(copied.Get(), derived.Get());
    EXPECT_EQ(derived->RefCount(), 2u);

    IntrusivePtr<Counted> moved = std::move(derived);
    EXPECT_FALSE(derived);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(moved->RefCount(), 2u);

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

}  // namespace
