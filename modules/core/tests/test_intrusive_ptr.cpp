#include <radray/intrusive_ptr.h>

#include <memory>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

namespace {

/// 最简接入方式: 自持计数, 归零即销毁。
///
/// 【计数从 1 起】: 配 AdoptRef 使用 —— 新建对象交给第一个 IntrusivePtr 时不再 +1,
/// 免去"创建后立刻 retain"那一步以及中途返回时的漏减。
class Simple {
public:
    explicit Simple(int value, int* destroyCount) noexcept
        : Value(value), _destroyCount(destroyCount) {}

    ~Simple() noexcept {
        if (_destroyCount != nullptr) {
            ++(*_destroyCount);
        }
    }

    uint32_t GetRefCount() const noexcept { return _refCount; }

    int Value{0};

private:
    friend void IntrusivePtrAddRef(const Simple* obj) noexcept;
    friend void IntrusivePtrRelease(Simple* obj) noexcept;

    int* _destroyCount{nullptr};
    /// mutable 因 AddRef 收 const —— retain 不改变对象的逻辑状态。
    mutable uint32_t _refCount{1};
};

void IntrusivePtrAddRef(const Simple* obj) noexcept {
    ++obj->_refCount;
}

void IntrusivePtrRelease(Simple* obj) noexcept {
    if (--obj->_refCount != 0) {
        return;
    }
    std::unique_ptr<Simple> owner{obj};
}

/// 派生类场景: 计数【必须放在基类】, 否则向基类转换后 Release 找不到同一份计数。
/// 归零时经虚析构销毁, 故 Release 里 delete 基类指针是安全的。
class Base {
public:
    explicit Base(int* destroyCount) noexcept : _destroyCount(destroyCount) {}

    virtual ~Base() noexcept {
        if (_destroyCount != nullptr) {
            ++(*_destroyCount);
        }
    }

    uint32_t GetRefCount() const noexcept { return _refCount; }

private:
    friend void IntrusivePtrAddRef(const Base* obj) noexcept;
    friend void IntrusivePtrRelease(Base* obj) noexcept;

    int* _destroyCount{nullptr};
    mutable uint32_t _refCount{1};
};

void IntrusivePtrAddRef(const Base* obj) noexcept {
    ++obj->_refCount;
}

void IntrusivePtrRelease(Base* obj) noexcept {
    if (--obj->_refCount != 0) {
        return;
    }
    std::unique_ptr<Base> owner{obj};
}

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
    friend void IntrusivePtrRelease(OwnerTracked* obj) noexcept;

    Owner* _owner{nullptr};
    int* _destroyCount{nullptr};
    mutable uint32_t _refCount{1};
};

void IntrusivePtrAddRef(const OwnerTracked* obj) noexcept {
    ++obj->_refCount;
}

/// 【本类型的存在意义】: 它是"归零时要做额外动作"的参考实现 —— 摘除缓存反向指针,
/// 然后销毁。Release 收非 const 故无需 const_cast; 若签名回退成 const T*, 这里会立刻
/// 需要一个 cast, 那就是签名错了的信号。
void IntrusivePtrRelease(OwnerTracked* obj) noexcept {
    if (--obj->_refCount != 0) {
        return;
    }
    std::unique_ptr<OwnerTracked> owner{obj};
    if (owner->_owner != nullptr) {
        owner->_owner->Detach(owner.get());
    }
}

/// 归零【不销毁】的接入方式: 对象由容器拥有, 计数只是租约。DescriptorSetLayoutCacheVulkan
/// 的形状 —— 计数从 0 起, 一律经 RetainRef 交出, 归零时从宿主容器擦除。
class Pooled;

class Pool {
public:
    Pooled* Acquire() noexcept;

    void Evict(Pooled* obj) noexcept;

    size_t Size() const noexcept { return _items.size(); }

    int EvictCount{0};

private:
    /// unique_ptr 而非按值: 使用者长期持有对象地址, 必须在容器增长下稳定。
    std::vector<std::unique_ptr<Pooled>> _items;
};

class Pooled {
public:
    explicit Pooled(Pool* pool) noexcept : _pool(pool) {}

    uint32_t GetRefCount() const noexcept { return _refCount; }

private:
    friend class Pool;
    friend void IntrusivePtrAddRef(const Pooled* obj) noexcept;
    friend void IntrusivePtrRelease(Pooled* obj) noexcept;

    Pool* _pool{nullptr};
    /// 【从 0 起】: 只数外部使用者, 容器自己的所有权不计入。
    mutable uint32_t _refCount{0};
};

void IntrusivePtrAddRef(const Pooled* obj) noexcept {
    ++obj->_refCount;
}

void IntrusivePtrRelease(Pooled* obj) noexcept {
    if (--obj->_refCount != 0) {
        return;
    }
    obj->_pool->Evict(obj);
}

Pooled* Pool::Acquire() noexcept {
    _items.push_back(std::make_unique<Pooled>(this));
    return _items.back().get();
}

void Pool::Evict(Pooled* obj) noexcept {
    ++EvictCount;
    for (auto it = _items.begin(); it != _items.end(); ++it) {
        if (it->get() == obj) {
            _items.erase(it);
            return;
        }
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

TEST(IntrusivePtr, AdoptRefDoesNotAddRef) {
    int destroyed = 0;
    auto ptr = radray::AdoptRef(std::make_unique<Simple>(7, &destroyed).release());
    ASSERT_TRUE(ptr.HasValue());
    EXPECT_EQ(ptr->Value, 7);
    EXPECT_EQ(ptr->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, DestroysOnLastRelease) {
    int destroyed = 0;
    {
        auto ptr = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, CopyAddsRef) {
    int destroyed = 0;
    auto first = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
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
    auto first = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    Simple* raw = first.Get();
    auto second = std::move(first);
    EXPECT_FALSE(first.HasValue());
    EXPECT_EQ(second.Get(), raw);
    EXPECT_EQ(second->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, SelfAssignmentIsSafe) {
    int destroyed = 0;
    auto ptr = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    const radray::IntrusivePtr<Simple>& alias = ptr;
    ptr = alias;
    ASSERT_TRUE(ptr.HasValue());
    EXPECT_EQ(ptr->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, ResetAndAssignNullptr) {
    int destroyed = 0;
    auto ptr = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    ptr.Reset();
    EXPECT_FALSE(ptr.HasValue());
    EXPECT_EQ(destroyed, 1);

    destroyed = 0;
    auto other = radray::AdoptRef(std::make_unique<Simple>(2, &destroyed).release());
    other = nullptr;
    EXPECT_FALSE(other.HasValue());
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ResetTwiceReleasesOnce) {
    int destroyed = 0;
    auto ptr = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    ptr.Reset();
    ptr.Reset();
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, RetainRefAddsRefAdoptRefDoesNot) {
    int destroyed = 0;
    auto owned = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    {
        auto retained = radray::RetainRef(owned.Get());
        EXPECT_EQ(owned->GetRefCount(), 2u);
    }
    EXPECT_EQ(owned->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);

    {
        auto adopted = radray::AdoptRef(std::make_unique<Simple>(2, &destroyed).release());
        EXPECT_EQ(adopted->GetRefCount(), 1u);
    }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ReleaseHandsOffTheCount) {
    int destroyed = 0;
    Simple* raw = nullptr;
    {
        auto ptr = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
        raw = ptr.Release();
        EXPECT_FALSE(ptr.HasValue());
    }
    EXPECT_EQ(destroyed, 0);
    { const auto reclaimed = radray::AdoptRef(raw); }
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ConvertsToBase) {
    int destroyed = 0;
    auto derived = radray::AdoptRef(std::make_unique<Derived>(&destroyed).release());
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
    auto derived = radray::AdoptRef(std::make_unique<Derived>(&destroyed).release());
    Base* raw = derived.Get();
    radray::IntrusivePtr<Base> base = std::move(derived);
    EXPECT_FALSE(derived.HasValue());
    EXPECT_EQ(base.Get(), raw);
    EXPECT_EQ(base->GetRefCount(), 1u);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, BaseRefKeepsDerivedAlive) {
    int destroyed = 0;
    radray::IntrusivePtr<Base> base;
    {
        auto derived = radray::AdoptRef(std::make_unique<Derived>(&destroyed).release());
        base = derived;
    }
    // 派生指针已放手, 但基类引用仍在 —— 计数在基类里, 故仍是同一份。
    EXPECT_EQ(destroyed, 0);
    EXPECT_EQ(base->GetRefCount(), 1u);
    base.Reset();
    EXPECT_EQ(destroyed, 1);
}

TEST(IntrusivePtr, ComparisonAndHash) {
    int destroyed = 0;
    auto first = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    auto alias = first;
    auto second = radray::AdoptRef(std::make_unique<Simple>(2, &destroyed).release());

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
    auto first = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
    auto second = radray::AdoptRef(std::make_unique<Simple>(2, &destroyed).release());
    swap(first, second);
    EXPECT_EQ(first->Value, 2);
    EXPECT_EQ(second->Value, 1);
    EXPECT_EQ(destroyed, 0);
}

TEST(IntrusivePtr, WorksInVector) {
    int destroyed = 0;
    {
        std::vector<radray::IntrusivePtr<Simple>> items;
        auto shared = radray::AdoptRef(std::make_unique<Simple>(1, &destroyed).release());
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

/// 【计数从 0 起 + 一律 RetainRef】: 容器拥有对象时的接法。第一个使用者也走 RetainRef,
/// 若误用 AdoptRef 则计数少一次递增, 对象在第一个使用者手上就已经是"无人使用"。
TEST(IntrusivePtr, PooledStartsAtZeroAndRetainRefLeases) {
    Pool pool;
    Pooled* raw = pool.Acquire();
    EXPECT_EQ(raw->GetRefCount(), 0u);
    EXPECT_EQ(pool.Size(), 1u);

    {
        auto lease = radray::RetainRef(raw);
        EXPECT_EQ(raw->GetRefCount(), 1u);
        {
            auto second = lease;
            EXPECT_EQ(raw->GetRefCount(), 2u);
            EXPECT_EQ(pool.EvictCount, 0);
        }
        EXPECT_EQ(raw->GetRefCount(), 1u);
        EXPECT_EQ(pool.EvictCount, 0);
    }
    // 最后一个租约归还 —— 归零即驱逐。
    EXPECT_EQ(pool.EvictCount, 1);
    EXPECT_EQ(pool.Size(), 0u);
}

/// 归零后再次 RetainRef 同一份内容会拿到【新】对象 —— 归零即驱逐意味着旧的已经消失。
TEST(IntrusivePtr, PooledReacquireAfterEviction) {
    Pool pool;
    {
        auto lease = radray::RetainRef(pool.Acquire());
        EXPECT_EQ(pool.Size(), 1u);
    }
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(pool.EvictCount, 1);

    {
        auto again = radray::RetainRef(pool.Acquire());
        EXPECT_EQ(again->GetRefCount(), 1u);
        EXPECT_EQ(pool.Size(), 1u);
    }
    EXPECT_EQ(pool.Size(), 0u);
    EXPECT_EQ(pool.EvictCount, 2);
}
