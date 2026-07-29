#pragma once

#include <cstdint>
#include <atomic>
#include <compare>
#include <functional>
#include <type_traits>
#include <utility>

#include <radray/types.h>

// 侵入式引用计数指针。
//
// == 为何不用 shared_ptr + weak_ptr ==
//
// 本设施的第一个使用点是"缓存共享的 GPU 对象" (见 runtime 的 PipelineLayoutCache):
// 缓存按内容去重, 多个持有者共享一个对象, 归零即销毁。用 shared_ptr 让持有者持强引用、
// 缓存持 weak_ptr 也能表达共享, 但有两处不成立:
//
// 1. 【计数必须活在对象自己身上】: 关停顺序是 RenderSystem (缓存的宿主) 先死, 之后
//    AssetManager 才 force-unload 全部资产, 那时持有者才放开最后一个引用。计数放在
//    对象外的控制块里也能撑过这一步, 但"归零时做什么" (从缓存索引摘除, 然后销毁 RHI
//    对象) 必须由对象自己知道 —— 它得在缓存已死的情况下跳过摘除。计数与该逻辑同处
//    一体, 缓存就退化成纯粹的非拥有索引, 缓存先死不影响任何存活对象的正确性。
// 2. 【weak_ptr 的失效只在 lock() 时被发现】, 死条目会攒在索引里等下一次 miss 才清。
//    侵入式计数在归零的那一刻就能主动摘除, 不留墓碑。
//
// AssetManager 的 AssetRefControl 不迁移到本设施。它的计数刻意在独立控制块里 (由
// shared_ptr 管), 因为 StreamingAssetRef 必须能在 slot 死后继续存活并报告 Unloaded ——
// 那是 weak 语义, 本设施给不了。两者解决的是不同问题。
//
// == 接入方式 ==
//
// 通过 ADL 找 IntrusivePtrAddRef(const T*) / IntrusivePtrRelease(T*) 这两个自由函数, 而
// 不是硬绑一个基类。因为"归零做什么"是类型自己的事 (上面第 1 条), core 不该替它决定。
// 只需要普通计数的类型可以直接继承 IntrusiveRefCounted<Derived>, 它提供默认实现。
//
// == 两个钩子的 const 性刻意不对称 ==
//
// AddRef 收 const T*, Release 收 T*。这【不是】疏漏, 不要以"一致性"为名改齐:
//
// - AddRef 只递增计数, 对象的逻辑状态不变, 故收 const 让只有 const 视图的持有者也能
//   retain。计数成员用 mutable 表达"计数不属于逻辑状态"。
// - Release 在归零的那一刻是【唯一所有者】, 它要从缓存索引摘除自己、把 GPU 对象交给
//   延迟销毁队列、最后销毁对象 —— 全部是 mutation。把它声明成 const T* 是一个假断言:
//   每个自定义 Release 的类型都必须写 const_cast 把 const 去掉才能干活 (曾经
//   SharedPipelineLayout 与测试里的 OwnerTracked 都这么做)。那不是 const 正确性,
//   是把一个谎言分摊到每一个接入点。
//
// IntrusivePtr 内部本来就持 T* (非 const), const 是过去在 Reset 里白送给 Release 的,
// 没有任何调用方要求它 —— 全仓库没有 IntrusivePtr<const T> 的实例化。
//
// == 不提供裸指针隐式构造 ==
//
// 只能经 AdoptRef (接管一个已计为 1 的新对象, 不 +1) 或 RetainRef (共享一个已存在的
// 对象, +1) 构造。"这个裸指针有没有被 retain 过"是侵入式计数最容易出错的地方, 让它
// 在调用点显式可读比省几个字符重要。

namespace radray {

/// 非原子计数策略。runtime 层 (PipelineStateCache / RenderPassRegistry / 本设施的首个
/// 使用点) 全线单线程, 默认不付原子开销。
class IntrusiveSingleThreadCounter {
public:
    constexpr IntrusiveSingleThreadCounter() noexcept = default;

    void Increment() const noexcept { ++_count; }

    /// 返回递减【后】的值。归零由调用方处理。
    uint32_t Decrement() const noexcept { return --_count; }

    uint32_t Load() const noexcept { return _count; }

private:
    mutable uint32_t _count{1};
};

/// 原子计数策略。留给确实跨线程共享的类型, 使调用点无需随线程模型变化而改动。
class IntrusiveAtomicCounter {
public:
    constexpr IntrusiveAtomicCounter() noexcept = default;

    void Increment() const noexcept { _count.fetch_add(1, std::memory_order_relaxed); }

    /// 返回递减【后】的值。release/acquire 保证归零者能看到其他线程的全部写入。
    uint32_t Decrement() const noexcept {
        const uint32_t prev = _count.fetch_sub(1, std::memory_order_release);
        if (prev == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return prev - 1;
    }

    uint32_t Load() const noexcept { return _count.load(std::memory_order_acquire); }

private:
    mutable std::atomic<uint32_t> _count{1};
};

/// 侵入式计数基类。新建对象的计数从 1 开始 —— 配 AdoptRef 使用, 免去"创建后立刻 +1"
/// 那一步以及中途返回时的漏减。
///
/// 归零时 delete static_cast<Derived*>(this)。需要在归零时做别的事 (如从缓存索引摘除、
/// 把 GPU 对象交给延迟销毁队列) 的类型不要继承本类, 自行提供 IntrusivePtrAddRef /
/// IntrusivePtrRelease —— 那两个钩子的 const 性约定见文件头。
template <class Derived, class CounterPolicy = IntrusiveSingleThreadCounter>
class IntrusiveRefCounted {
public:
    IntrusiveRefCounted() noexcept = default;
    IntrusiveRefCounted(const IntrusiveRefCounted&) = delete;
    IntrusiveRefCounted& operator=(const IntrusiveRefCounted&) = delete;
    IntrusiveRefCounted(IntrusiveRefCounted&&) = delete;
    IntrusiveRefCounted& operator=(IntrusiveRefCounted&&) = delete;

    /// 仅用于诊断与测试。业务代码不该按计数分支。
    uint32_t GetRefCount() const noexcept { return _counter.Load(); }

protected:
    ~IntrusiveRefCounted() noexcept = default;

private:
    template <class D, class C>
    friend void IntrusivePtrAddRef(const IntrusiveRefCounted<D, C>* obj) noexcept;
    template <class D, class C>
    friend void IntrusivePtrRelease(IntrusiveRefCounted<D, C>* obj) noexcept;

    CounterPolicy _counter;
};

template <class D, class C>
void IntrusivePtrAddRef(const IntrusiveRefCounted<D, C>* obj) noexcept {
    obj->_counter.Increment();
}

template <class D, class C>
void IntrusivePtrRelease(IntrusiveRefCounted<D, C>* obj) noexcept {
    if (obj->_counter.Decrement() == 0) {
        delete static_cast<D*>(obj);
    }
}

/// T 已接入侵入式计数。
template <class T>
concept IntrusiveRefCountable = requires(T* ptr) {
    IntrusivePtrAddRef(ptr);
    IntrusivePtrRelease(ptr);
};

template <class T>
requires IntrusiveRefCountable<T>
class IntrusivePtr;

namespace detail {

struct IntrusiveAdoptTag {};
struct IntrusiveRetainTag {};

}  // namespace detail

/// 侵入式计数指针。move / copy 均维护计数, 析构减一。
template <class T>
requires IntrusiveRefCountable<T>
class IntrusivePtr {
public:
    using element_type = T;

    constexpr IntrusivePtr() noexcept = default;
    constexpr IntrusivePtr(std::nullptr_t) noexcept {}

    /// 接管一个新建对象 (计数已是 1), 不 +1。经 AdoptRef 调用。
    IntrusivePtr(T* ptr, detail::IntrusiveAdoptTag) noexcept : _ptr(ptr) {}

    /// 共享一个已存在对象, +1。经 RetainRef 调用。
    IntrusivePtr(T* ptr, detail::IntrusiveRetainTag) noexcept : _ptr(ptr) {
        if (_ptr != nullptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }

    IntrusivePtr(const IntrusivePtr& other) noexcept : _ptr(other._ptr) {
        if (_ptr != nullptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }

    IntrusivePtr(IntrusivePtr&& other) noexcept : _ptr(other._ptr) {
        other._ptr = nullptr;
    }

    /// 向基类隐式转换。派生类的计数由基类持有时同样有效 (计数就在基类里)。
    template <class U>
    requires(!std::is_same_v<U, T>) && std::is_convertible_v<U*, T*>
    IntrusivePtr(const IntrusivePtr<U>& other) noexcept : _ptr(other.Get()) {
        if (_ptr != nullptr) {
            IntrusivePtrAddRef(_ptr);
        }
    }

    template <class U>
    requires(!std::is_same_v<U, T>) && std::is_convertible_v<U*, T*>
    IntrusivePtr(IntrusivePtr<U>&& other) noexcept : _ptr(other.Release()) {}

    IntrusivePtr& operator=(const IntrusivePtr& other) noexcept {
        IntrusivePtr(other).Swap(*this);
        return *this;
    }

    IntrusivePtr& operator=(IntrusivePtr&& other) noexcept {
        IntrusivePtr(std::move(other)).Swap(*this);
        return *this;
    }

    IntrusivePtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }

    ~IntrusivePtr() noexcept { Reset(); }

    T* Get() const noexcept { return _ptr; }
    T* operator->() const noexcept { return _ptr; }
    T& operator*() const noexcept { return *_ptr; }

    bool HasValue() const noexcept { return _ptr != nullptr; }
    explicit operator bool() const noexcept { return HasValue(); }

    void Reset() noexcept {
        if (T* old = std::exchange(_ptr, nullptr); old != nullptr) {
            IntrusivePtrRelease(old);
        }
    }

    /// 交出裸指针与其那一份计数。调用方负责最终 Release。
    [[nodiscard]] T* Release() noexcept { return std::exchange(_ptr, nullptr); }

    void Swap(IntrusivePtr& other) noexcept { std::swap(_ptr, other._ptr); }

    friend void swap(IntrusivePtr& lhs, IntrusivePtr& rhs) noexcept { lhs.Swap(rhs); }

    friend bool operator==(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
        return lhs._ptr == rhs._ptr;
    }
    friend std::strong_ordering operator<=>(const IntrusivePtr& lhs, const IntrusivePtr& rhs) noexcept {
        return std::compare_three_way{}(lhs._ptr, rhs._ptr);
    }
    friend bool operator==(const IntrusivePtr& lhs, std::nullptr_t) noexcept {
        return lhs._ptr == nullptr;
    }

private:
    T* _ptr{nullptr};
};

/// 接管一个新建对象 —— 计数已是 1, 不再 +1。
///
/// 【接管即承诺】: 调用方保证自己类型的 IntrusivePtrRelease 用与本对象【分配方式匹配】的
/// 路径销毁它。core 看不到分配现场, 无从校验:
///   - 堆对象 (MakeIntrusive / make_unique 的产物) -> Release 里销毁 (收进 unique_ptr)。
///   - 容器内嵌对象 (如 DescriptorSetLayoutVulkan 按值存在 unordered_map 里, 经
///     AdoptRef(&node) 接管) -> Release 里【不得】销毁, 由容器负责。
template <class T>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> AdoptRef(T* ptr) noexcept {
    return IntrusivePtr<T>(ptr, detail::IntrusiveAdoptTag{});
}

/// 从 unique_ptr 接管 —— 计数已是 1, 不再 +1。
///
/// 【为何需要本重载】: 归零时要做额外动作 (摘除缓存、把 GPU 对象交给延迟销毁队列) 的类型
/// 不能继承 IntrusiveRefCounted, 于是也用不上 MakeIntrusive (它内部 new 后直接 Adopt)。
/// 这类类型的创建路径是 make_unique 再移交; 没有本重载, 每个调用点都要写 .release(),
/// 而项目要求所有权移交显式经 RAII 容器表达, 不出现裸 new/delete 与裸指针交接。
template <class T>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> AdoptRef(unique_ptr<T> ptr) noexcept {
    return IntrusivePtr<T>(ptr.release(), detail::IntrusiveAdoptTag{});
}

/// 共享一个已存在对象 —— +1。
template <class T>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> RetainRef(T* ptr) noexcept {
    return IntrusivePtr<T>(ptr, detail::IntrusiveRetainTag{});
}

/// new 并接管。T 的计数从 1 开始 (见 IntrusiveRefCounted)。
template <class T, class... Args>
requires IntrusiveRefCountable<T>
[[nodiscard]] IntrusivePtr<T> MakeIntrusive(Args&&... args) {
    return AdoptRef(new T(std::forward<Args>(args)...));
}

}  // namespace radray

namespace std {

template <class T>
struct hash<radray::IntrusivePtr<T>> {
    size_t operator()(const radray::IntrusivePtr<T>& ptr) const noexcept {
        return hash<T*>{}(ptr.Get());
    }
};

}  // namespace std
