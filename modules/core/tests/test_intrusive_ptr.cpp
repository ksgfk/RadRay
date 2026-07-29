#include <radray/intrusive_ptr.h>

#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace {

/// 最简接入方式: 继承 IntrusiveRefCounted, 归零即 delete。
class Simple : public radray::IntrusiveRefCounted<Simple> {
public:
    explicit Simple(int value, int* destroyCount) noexcept
        : Value(value), _destroyCount(destroyCount) {}
    ~Simple() noexcept {
        if (_destroyCount != nullptr) {
            ++(*_destroyCount);
        }
    }

    int Value{0};

private:
    int* _destroyCount{nullptr};
};

class Base : public radray::IntrusiveRefCounted<Base> {
public:
    explicit Base(int* destroyCount) noexcept : _destroyCount(destroyCount) {}
    virtual ~Base() noexcept {
        if (_destroyCount != nullptr) {
            ++(*_destroyCount);
        }
    }

private:
    int* _destroyCount{nullptr};
};

class Derived : public Base {
public:
    explicit Derived(int* destroyCount) noexcept : Base(destroyCount) {}
};

/// 自定义钩子: 归零时先通知宿主再销毁。这是 PipelineLayoutCache 的形状 —— 宿主可能
/// 先死, 那时 _owner 为空, 直接销毁。
class OwnerTracked;

class Owner {
public:
    void Detach(OwnerTracked* obj) noexcept {
        ++DetachCount;
        Last = obj;
    }

    int DetachCount{0};
    OwnerTracked* Last{nullptr};
};

class OwnerTracked {
public:
    explicit OwnerTracked(Owner* owner, int* destroyCount) noexcept
        : _owner(owner), _destroyCount(destroyCount) {}
    ~OwnerTracked() noexcept {
        if (_destroyCount != nullptr) {
            ++(*_destroyCount);
        }
    }

    void DetachOwner() noexcept { _owner = nullptr; }

    uint32_t GetRefCount() const noexcept { return _refCount; }

private:
    friend void IntrusivePtrAddRef(const OwnerTracked* obj) noexcept;
    friend void IntrusivePtrRelease(const OwnerTracked* obj) noexcept;

    Owner* _owner{nullptr};
    int* _destroyCount{nullptr};
    mutable uint32_t _refCount{1};
};

void IntrusivePtrAddRef(const OwnerTracked* obj) noexcept {
    ++obj->_refCount;
}

void IntrusivePtrRelease(const OwnerTracked* obj) noexcept {
    if (--obj->_refCount == 0) {
        auto* mutableObj = const_cast<OwnerTracked*>(obj);
        if (mutableObj->_owner != nullptr) {
            mutableObj->_owner->Detach(mutableObj);
        }
        delete mutableObj;
    }
}

}  // namespace

TEST(IntrusivePtr, DefaultIsEmpty) {
    radray::IntrusivePtr<Simple> ptr;
    EXPECT_FALSE(ptr.HasValue());
    EXPECT_FALSE(static_cast<bool>(ptr));
    EXPECT_EQ(ptr.Get(), nullptr);
    EXPECT_TRUE(ptr == nullptr);
}

TEST(IntrusivePtr, MakeIntrusiveStartsAtOne) {
    int destroyed = 0;
    auto ptr = radray::MakeIntrusive<Simple>(7, &destroyed);
    ASSERT_TRUE(ptr.HasValue());
    EXPECT_EQ(ptr->Value, 7);
    EXPECT_EQ(ptr->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, DestroysOnLastRelease) {
    int destroyed = 0;
    {
        auto ptr = radray::MakeIntrusive<Simple>(1, &destroyed);
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, CopyAddsRef) {
    int destroyed = 0;
    auto first = radray::MakeIntrusive<Simple>(1, &destroyed);
    {
        auto second = first;
        EXPECT_EQ(first->GetRefCount(), 2u);
        EXPECT_EQ(second.Get(), first.Get());
    }
    EXPECT_EQ(first->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, MoveTransfersWithoutAddRef) {
    int destroyed = 0;
    auto first = radray::MakeIntrusive<Simple>(1, &destroyed);
    Simple* raw = first.Get();
    auto second = std::move(first);
    EXPECT_FALSE(first.HasValue());
    EXPECT_EQ(second.Get(), raw);
    EXPECT_EQ(second->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, SelfAssignmentIsSafe) {
    int destroyed = 0;
    auto ptr = radray::MakeIntrusive<Simple>(1, &destroyed);
    const radray::IntrusivePtr<Simple>& alias = ptr;
    ptr = alias;
    ASSERT_TRUE(ptr.HasValue());
    EXPECT_EQ(ptr->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, ResetAndAssignNullptr) {
    int destroyed = 0;
    auto ptr = radray::MakeIntrusive<Simple>(1, &destroyed);
    ptr.Reset();
    EXPECT_FALSE(ptr.HasValue());
    EXPECT_EQ(destroyed, 1);

    destroyed = 0;
    auto other = radray::MakeIntrusive<Simple>(2, &destroyed);
    other = nullptr;
    EXPECT_FALSE(other.HasValue());
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ResetTwiceReleasesOnce) {
    int destroyed = 0;
    auto ptr = radray::MakeIntrusive<Simple>(1, &destroyed);
    ptr.Reset();
    ptr.Reset();
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, RetainRefAddsRefAdoptRefDoesNot) {
    int destroyed = 0;
    auto owned = radray::MakeIntrusive<Simple>(1, &destroyed);
    {
        auto retained = radray::RetainRef(owned.Get());
        EXPECT_EQ(owned->GetRefCount(), 2u);
    }
    EXPECT_EQ(owned->GetRefCount(), 1u);

    Simple* fresh = new Simple(2, &destroyed);
    {
        auto adopted = radray::AdoptRef(fresh);
        EXPECT_EQ(adopted->GetRefCount(), 1u);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ReleaseHandsOffTheCount) {
    int destroyed = 0;
    Simple* raw = nullptr;
    {
        auto ptr = radray::MakeIntrusive<Simple>(1, &destroyed);
        raw = ptr.Release();
        EXPECT_FALSE(ptr.HasValue());
    }
    EXPECT_EQ(destroyed, 0);
    { const auto reclaimed = radray::AdoptRef(raw); }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ConvertsToBase) {
    int destroyed = 0;
    auto derived = radray::MakeIntrusive<Derived>(&destroyed);
    {
        radray::IntrusivePtr<Base> base = derived;
        EXPECT_EQ(base.Get(), derived.Get());
        EXPECT_EQ(derived->GetRefCount(), 2u);
    }
    EXPECT_EQ(derived->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, MoveConvertsToBaseWithoutAddRef) {
    int destroyed = 0;
    auto derived = radray::MakeIntrusive<Derived>(&destroyed);
    Base* raw = derived.Get();
    radray::IntrusivePtr<Base> base = std::move(derived);
    EXPECT_FALSE(derived.HasValue());
    EXPECT_EQ(base.Get(), raw);
    EXPECT_EQ(base->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, ComparisonAndHash) {
    int destroyed = 0;
    auto first = radray::MakeIntrusive<Simple>(1, &destroyed);
    auto alias = first;
    auto second = radray::MakeIntrusive<Simple>(2, &destroyed);

    EXPECT_TRUE(first == alias);
    EXPECT_FALSE(first == second);
    EXPECT_TRUE((first <=> alias) == std::strong_ordering::equal);

    std::unordered_set<radray::IntrusivePtr<Simple>> set;
    set.insert(first);
    set.insert(alias);
    set.insert(second);
    EXPECT_EQ(set.size(), 2u);
}

TEST(IntrusivePtr, SwapExchangesOwnership) {
    int destroyed = 0;
    auto first = radray::MakeIntrusive<Simple>(1, &destroyed);
    auto second = radray::MakeIntrusive<Simple>(2, &destroyed);
    swap(first, second);
    EXPECT_EQ(first->Value, 2);
    EXPECT_EQ(second->Value, 1);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, WorksInVector) {
    int destroyed = 0;
    {
        std::vector<radray::IntrusivePtr<Simple>> items;
        auto shared = radray::MakeIntrusive<Simple>(1, &destroyed);
        for (int i = 0; i < 4; ++i) {
            items.push_back(shared);
        }
        EXPECT_EQ(shared->GetRefCount(), 5u);
        items.clear();
        EXPECT_EQ(shared->GetRefCount(), 1u);
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, AtomicCounterPolicy) {
    class Threaded : public radray::IntrusiveRefCounted<Threaded, radray::IntrusiveAtomicCounter> {};
    auto ptr = radray::MakeIntrusive<Threaded>();
    EXPECT_EQ(ptr->GetRefCount(), 1u);
    auto copy = ptr;
    EXPECT_EQ(ptr->GetRefCount(), 2u);
}

TEST(IntrusivePtr, CustomHookNotifiesOwnerOnZero) {
    Owner owner;
    int destroyed = 0;
    OwnerTracked* raw = new OwnerTracked(&owner, &destroyed);
    {
        auto ptr = radray::AdoptRef(raw);
        auto copy = ptr;
        EXPECT_EQ(ptr->GetRefCount(), 2u);
        EXPECT_EQ(owner.DetachCount, 0);
    }
    EXPECT_EQ(owner.DetachCount, 1);
    EXPECT_EQ(owner.Last, raw);
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, CustomHookSurvivesOwnerDyingFirst) {
    int destroyed = 0;
    OwnerTracked* raw = nullptr;
    radray::IntrusivePtr<OwnerTracked> ptr;
    {
        Owner owner;
        raw = new OwnerTracked(&owner, &destroyed);
        ptr = radray::AdoptRef(raw);
        // 宿主先死 —— 照 RenderSystem 先于 AssetManager 关停的顺序。
        raw->DetachOwner();
    }
    EXPECT_EQ(destroyed, 0);
    ptr.Reset();
    EXPECT_EQ(destroyed, 1);
}
