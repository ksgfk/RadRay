// AssetManager 的槽位生命周期。这一层的规则全部围绕【引用计数是生命周期的唯一权威】:
//
//   1. 引用归零才卸载 —— 没有 Unload / CollectUnreferenced 之类能无视引用的入口;
//   2. 销毁对齐到 Pump 这一确定时刻, 不就地发生在引用归零处;
//   3. OnUnload 先于析构, 且在那里能经 DeferDestroy 交出需延迟销毁的数据;
//   4. AssetManager 先死时仍无条件走一遍 OnUnload —— 泄漏 GPU 资源比悬垂更难查。
//
// 【为何不需要 device】: 以上全是槽位与引用的行为, 与 GPU 无关。本文件用一个假资产
// (计数它自己的 OnUnload / 析构) 代替真资产, 于是可在无显卡的机器上跑。真资产把 GPU
// 对象交给延迟队列的路径由 test_shader_program 覆盖。

#include <radray/runtime/asset_manager.h>

#include <radray/coroutine.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/wait_frame.h>
#include <radray/types.h>

#include <gtest/gtest.h>

#include <coroutine>

namespace radray {
namespace {
class ProbeAsset;
}  // namespace

/// 【必须在 ProbeAsset 定义之前】: 它的 GetTypeId 体内用了 runtime_type_id_v<ProbeAsset>,
/// 而非模板成员函数的体在类定义处就编译 —— 特化放在后面会先实例化出默认的空 Guid 特化,
/// 撞上 runtime_type.h 的 static_assert。
template <>
struct RuntimeTypeTrait<ProbeAsset> {
    static constexpr RuntimeTypeId value{0x7c31a05e, 0x2a44, 0x4f13, 0x9d, 0x62, 0x11, 0x8b, 0x40, 0xe7, 0x55, 0x0a};
    using Bases = std::tuple<Asset>;
};

namespace {

/// 一个不碰 GPU 的假资产。它把 OnUnload 与析构的次数记到外部计数器上, 使"OnUnload
/// 先于析构"与"引用归零才卸载"可被断言。
class ProbeAsset : public Asset {
public:
    struct Counters {
        uint32_t Unloaded{0};
        uint32_t Destroyed{0};
        /// OnUnload 时 Destroyed 的值。为 0 才说明 OnUnload 确实跑在析构之前。
        uint32_t DestroyedAtUnload{0xffffffff};
        /// DeferDestroy 交出的 payload 被销毁的次数。
        uint32_t PayloadDestroyed{0};
    };

    /// 【计数器必须是 shared_ptr, 不能是指向用例栈上局部量的裸指针】: 用例结束时若仍有
    /// 存活引用, 槽位要到 fixture 的 ~AssetManager 才回收 —— 那已经在用例函数返回【之后】,
    /// 栈上的 Counters 早已死亡, 于是 OnUnload 写进已失效的栈帧。那种写入不一定立刻崩,
    /// 表现为随机的挂死或串扰, 极难定位。共享所有权让计数器活到最后一个写入者之后。
    ProbeAsset(shared_ptr<Counters> counters, bool wantsDeferredDestroy) noexcept
        : _counters(std::move(counters)), _wantsDeferredDestroy(wantsDeferredDestroy) {}

    ~ProbeAsset() noexcept override {
        if (_counters != nullptr) {
            ++_counters->Destroyed;
        }
    }

    void OnUnload(AssetManager& manager) override {
        if (_counters != nullptr) {
            ++_counters->Unloaded;
            _counters->DestroyedAtUnload = _counters->Destroyed;
        }
        if (!_wantsDeferredDestroy) {
            return;
        }
        // 真资产在此把整包 GPU 对象交出去。这里交一个记账用的守卫即可 —— 被测的是路径,
        // 不是资源类型。
        manager.DeferDestroy([guard = DestroyGuard{_counters}]() noexcept {});
    }

    RuntimeTypeId GetTypeId() const noexcept override { return runtime_type_id_v<ProbeAsset>; }

private:
    /// 析构时给 PayloadDestroyed +1, 被移走的那份不再记账。
    ///
    /// 【为何必须显式写移动构造】: DeferDestroy 把 payload 移动进类型擦除的包装, 而
    /// 用户声明的析构函数会【抑制隐式移动构造】—— 那时移动静默退化为拷贝, 于是临时对象
    /// 与存下来的那份各记一次账, 计数翻倍。这正是本文件第一版失败的原因。
    class DestroyGuard {
    public:
        explicit DestroyGuard(shared_ptr<Counters> sink) noexcept : _sink(std::move(sink)) {}
        DestroyGuard(DestroyGuard&& other) noexcept : _sink(std::move(other._sink)) {}
        DestroyGuard(const DestroyGuard&) = delete;
        DestroyGuard& operator=(DestroyGuard&&) = delete;
        DestroyGuard& operator=(const DestroyGuard&) = delete;

        ~DestroyGuard() noexcept {
            if (_sink != nullptr) {
                ++_sink->PayloadDestroyed;
            }
        }

    private:
        shared_ptr<Counters> _sink;
    };

    shared_ptr<Counters> _counters;
    bool _wantsDeferredDestroy{false};
};

using Counters = ProbeAsset::Counters;

shared_ptr<Counters> MakeCounters() { return make_shared<Counters>(); }

/// 立即恢复的帧边界等待器, 并记录被等待过的次数。
///
/// 【为何可以立即恢复】: 测试里没有 GPU 在跑 —— 没有已录制的命令列表, "等到已录制的
/// work 完成"这个条件平凡成立。真实实现必须等 fence, 见 GpuSystem::Wait。
class CountingWaitFrame : public IWaitFrameProcessor {
public:
    task<void> Wait() override {
        ++Count;
        co_return;
    }

    uint32_t Count{0};
};

constexpr AssetId MakeId(uint32_t n) noexcept {
    return AssetId{n, 0x0001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
}

/// 一个由测试手动放行的挂起点。
///
/// 【为何必须有它】: async_scope::spawn 会【同步】推进协程直到第一个真正的挂起点, 而
/// 一个只有 co_return 的加载 task 没有挂起点 —— 它在 Load() 返回之前就跑完了。于是所有
/// 关心"在飞期间发生了什么"的用例都无从下手。这个 gate 给加载协程造一个可控的停留处。
///
/// 【为何是 `co_await gate.Wait()` 而不是 `co_await gate`】: exec::task 的 promise 有
/// await_transform, 它会把 awaitable 【按值】转发一遍; 若 operator co_await 定义在 gate
/// 自己身上, 那个 this 就指向那份副本, 于是 handle 记进了副本而原 gate 永远 IsPending()
/// 为 false。Wait() 返回的 awaiter 只带一个指向真 gate 的指针, 复制它无害。
class ManualGate {
public:
    ManualGate() noexcept = default;
    ManualGate(const ManualGate&) = delete;
    ManualGate(ManualGate&&) = delete;
    ManualGate& operator=(const ManualGate&) = delete;
    ManualGate& operator=(ManualGate&&) = delete;

    /// 【析构即放行】: 用例中途 ASSERT 失败会直接 return, 若此时还有协程停在门口,
    /// ~AssetManager 的 WaitUntilEmpty 会永久阻塞 —— 一个失败的断言就变成一次挂死,
    /// 掩盖真正的失败信息。gate 是用例的局部量, 声明序保证它先于 fixture 析构。
    ~ManualGate() { Resume(); }

    bool IsPending() const noexcept { return static_cast<bool>(_handle); }

    void Resume() {
        std::coroutine_handle<> handle = _handle;
        _handle = {};
        if (handle) {
            handle.resume();
        }
    }

    struct Awaiter {
        ManualGate* Gate{nullptr};
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> continuation) const noexcept {
            Gate->_handle = continuation;
        }
        void await_resume() const noexcept {}
    };

    Awaiter Wait() noexcept { return Awaiter{this}; }

private:
    std::coroutine_handle<> _handle{};
};

/// 【声明序即析构序的反面】: _waitFrame 必须比 _assets 活得久 —— 资产在 OnUnload 里经
/// manager 用它, 而那发生在 manager 析构期间。故 _waitFrame 声明在前 (逆序析构时后死)。
class AssetSlotTest : public ::testing::Test {
protected:
    void SetUp() override { _assets.SetWaitFrameProcessor(&_waitFrame); }

    AssetManager& Assets() noexcept { return _assets; }
    CountingWaitFrame& WaitFrame() noexcept { return _waitFrame; }

private:
    CountingWaitFrame _waitFrame;
    AssetManager _assets;
};

// ════════════════════════════════════════════════════════════
//  引用计数是生命周期的唯一权威
// ════════════════════════════════════════════════════════════

/// 最后一份引用消失才卸载。这条是整个模型的地基: 正因为它成立, 缓存了资产内部指针的
/// 地方只需存一份 ref, 不必再有第二层引用计数的"内容"对象。
TEST_F(AssetSlotTest, SlotSurvivesUntilTheLastReferenceGoesAway) {
    shared_ptr<Counters> counters = MakeCounters();
    const AssetId id = MakeId(1);

    {
        StreamingAssetRef<ProbeAsset> first =
            Assets().AddReady<ProbeAsset>(id, make_unique<ProbeAsset>(counters, false));
        ASSERT_TRUE(first.IsReady());
        EXPECT_EQ(Assets().GetAssetCount(), 1u);

        {
            // 同 id 再取一份 —— 是同一个槽位, 只多一份计数。
            StreamingAssetRef<ProbeAsset> second = Assets().Find<ProbeAsset>(id);
            ASSERT_TRUE(second.IsReady());
            EXPECT_EQ(second.Get(), first.Get());
            EXPECT_TRUE(second == first) << "dedup by id must land on the same slot";
            EXPECT_EQ(Assets().GetAssetCount(), 1u);
        }

        // 放掉一份不足以卸载, Pump 也不该动它。
        Assets().Pump();
        EXPECT_EQ(counters->Unloaded, 0u);
        EXPECT_EQ(Assets().GetAssetCount(), 1u);
        EXPECT_TRUE(first.IsReady());
    }

    // 引用归零, 但销毁对齐到 Pump —— 归零那一刻【还没有】发生任何事。
    EXPECT_EQ(counters->Unloaded, 0u) << "destruction is aligned to Pump, not to the last release";
    EXPECT_EQ(Assets().GetAssetCount(), 1u);

    Assets().Pump();
    EXPECT_EQ(counters->Unloaded, 1u);
    EXPECT_EQ(counters->Destroyed, 1u);
    EXPECT_EQ(Assets().GetAssetCount(), 0u) << "the slot should be gone once Pump collects it";
    EXPECT_FALSE(Assets().Find<ProbeAsset>(id).IsValid());
}

/// 【销毁不就地发生】这条单独立一个用例, 因为它是 ~StreamingAssetRefAny 得以保持
/// noexcept 且可在遍历资产表时安全调用的全部依据。就地销毁会让那条路径跑资产析构 ——
/// 而资产析构会放开它自己持有的引用, 从而递归销毁别的槽位、令正在遍历的迭代器失效。
TEST_F(AssetSlotTest, ReleasingTheLastReferenceDoesNotDestroyInPlace) {
    shared_ptr<Counters> counters = MakeCounters();
    {
        StreamingAssetRef<ProbeAsset> ref =
            Assets().AddReady<ProbeAsset>(MakeId(2), make_unique<ProbeAsset>(counters, false));
        ASSERT_TRUE(ref.IsReady());
    }
    EXPECT_EQ(counters->Unloaded, 0u);
    EXPECT_EQ(counters->Destroyed, 0u);
    EXPECT_EQ(Assets().GetAssetCount(), 1u) << "the slot must outlive the last reference until Pump";

    Assets().Pump();
    EXPECT_EQ(counters->Destroyed, 1u);
}

/// 引用归零后再 Find 回来即可复活槽位 —— 只要发生在 Pump 之前。这不是一个要依赖的特性,
/// 而是"销毁对齐到 Pump"的直接推论, 固定住它可防止有人把销毁改回归零处。
TEST_F(AssetSlotTest, ReacquiringBeforePumpKeepsTheSameAsset) {
    shared_ptr<Counters> counters = MakeCounters();
    const AssetId id = MakeId(3);
    const ProbeAsset* raw = nullptr;

    {
        StreamingAssetRef<ProbeAsset> ref =
            Assets().AddReady<ProbeAsset>(id, make_unique<ProbeAsset>(counters, false));
        ASSERT_TRUE(ref.IsReady());
        raw = ref.Get();
    }

    StreamingAssetRef<ProbeAsset> revived = Assets().Find<ProbeAsset>(id);
    ASSERT_TRUE(revived.IsReady());
    EXPECT_EQ(revived.Get(), raw);

    Assets().Pump();
    EXPECT_EQ(counters->Unloaded, 0u) << "the slot was referenced again before Pump ran";
    EXPECT_TRUE(revived.IsReady());
}

/// OnUnload 跑在析构【之前】。这不是风格问题: OnUnload 要拿 AssetManager 才能交出需要
/// 延迟销毁的数据, 而析构函数里拿不到, 故顺序反了就等于没有延迟释放。
TEST_F(AssetSlotTest, OnUnloadRunsBeforeTheDestructor) {
    shared_ptr<Counters> counters = MakeCounters();
    {
        StreamingAssetRef<ProbeAsset> ref =
            Assets().AddReady<ProbeAsset>(MakeId(4), make_unique<ProbeAsset>(counters, false));
        ASSERT_TRUE(ref.IsReady());
    }
    Assets().Pump();

    EXPECT_EQ(counters->Unloaded, 1u);
    EXPECT_EQ(counters->Destroyed, 1u);
    EXPECT_EQ(counters->DestroyedAtUnload, 0u) << "OnUnload must observe an alive asset";
}

/// 资产在 OnUnload 里把需延迟销毁的数据整包交给 DeferDestroy, 那一包在【一个帧边界之后】
/// 才销毁 —— 而不是随资产析构一起。
TEST_F(AssetSlotTest, OnUnloadHandsPayloadsToTheWaitFrameProcessor) {
    shared_ptr<Counters> counters = MakeCounters();
    {
        StreamingAssetRef<ProbeAsset> ref =
            Assets().AddReady<ProbeAsset>(MakeId(5), make_unique<ProbeAsset>(counters, true));
        ASSERT_TRUE(ref.IsReady());
        EXPECT_EQ(WaitFrame().Count, 0u);
    }
    Assets().Pump();

    EXPECT_EQ(counters->Unloaded, 1u);
    EXPECT_EQ(WaitFrame().Count, 1u) << "the payload must go through a frame boundary";
    EXPECT_EQ(counters->PayloadDestroyed, 1u);
}

/// 【一帧一个协程帧】: 同一次 Pump 里归零的多个资产共用一个等待帧边界的协程。故大量
/// 资产同时归零不会产生大量协程。
TEST_F(AssetSlotTest, PayloadsFromOnePumpShareOneFrameWait) {
    shared_ptr<Counters> a = MakeCounters();
    shared_ptr<Counters> b = MakeCounters();
    shared_ptr<Counters> c = MakeCounters();
    {
        StreamingAssetRef<ProbeAsset> r1 = Assets().AddReady<ProbeAsset>(MakeId(6), make_unique<ProbeAsset>(a, true));
        StreamingAssetRef<ProbeAsset> r2 = Assets().AddReady<ProbeAsset>(MakeId(7), make_unique<ProbeAsset>(b, true));
        StreamingAssetRef<ProbeAsset> r3 = Assets().AddReady<ProbeAsset>(MakeId(8), make_unique<ProbeAsset>(c, true));
        ASSERT_TRUE(r1.IsReady() && r2.IsReady() && r3.IsReady());
        EXPECT_EQ(Assets().GetAssetCount(), 3u);
    }
    Assets().Pump();

    EXPECT_EQ(Assets().GetAssetCount(), 0u);
    EXPECT_EQ(WaitFrame().Count, 1u) << "three assets, one frame-boundary wait";
    EXPECT_EQ(a->PayloadDestroyed, 1u);
    EXPECT_EQ(b->PayloadDestroyed, 1u);
    EXPECT_EQ(c->PayloadDestroyed, 1u);
}

/// 一个资产持有别的资产引用时, 前者被回收会令后者归零。收集要循环到不动点, 否则被依赖
/// 的那个要多等一次 Pump 才死。
TEST_F(AssetSlotTest, CollectingCascadesToDependenciesWithinOnePump) {
    shared_ptr<Counters> ownerCounters = MakeCounters();
    shared_ptr<Counters> heldCounters = MakeCounters();
    const AssetId heldId = MakeId(9);

    /// 一个持有另一份资产引用的资产。析构时那份引用随之放开。
    class OwnerAsset : public ProbeAsset {
    public:
        OwnerAsset(shared_ptr<Counters> counters, StreamingAssetRefAny held) noexcept
            : ProbeAsset(std::move(counters), false), _held(std::move(held)) {}

    private:
        StreamingAssetRefAny _held;
    };

    {
        StreamingAssetRef<ProbeAsset> held =
            Assets().AddReady<ProbeAsset>(heldId, make_unique<ProbeAsset>(heldCounters, false));
        ASSERT_TRUE(held.IsReady());

        // OwnerAsset 是 ProbeAsset 的派生类, 但运行时类型元数据必须报 ProbeAsset ——
        // GetTypeId 未被覆写, 而 CommitLoadResult 会核对两者一致。
        StreamingAssetRef<ProbeAsset> owner = Assets().AddReady<ProbeAsset>(
            MakeId(10),
            unique_ptr<ProbeAsset>{make_unique<OwnerAsset>(ownerCounters, held.AsAny())});
        ASSERT_TRUE(owner.IsReady());
        EXPECT_EQ(Assets().GetAssetCount(), 2u);
    }

    Assets().Pump();
    EXPECT_EQ(ownerCounters->Destroyed, 1u);
    EXPECT_EQ(heldCounters->Destroyed, 1u) << "collection must iterate to a fixed point";
    EXPECT_EQ(Assets().GetAssetCount(), 0u);
}

/// 引用归零、Pump 回收之后, 同 id 可以重新建槽位。验的是回收真的把索引项擦掉了 ——
/// 若只清了对象而留着索引, 这里会命中一个空槽位。
TEST_F(AssetSlotTest, SameIdCanBeReoccupiedAfterCollection) {
    shared_ptr<Counters> first = MakeCounters();
    shared_ptr<Counters> second = MakeCounters();
    const AssetId id = MakeId(11);

    {
        StreamingAssetRef<ProbeAsset> ref =
            Assets().AddReady<ProbeAsset>(id, make_unique<ProbeAsset>(first, false));
        ASSERT_TRUE(ref.IsReady());
    }
    Assets().Pump();
    ASSERT_EQ(Assets().GetAssetCount(), 0u);

    StreamingAssetRef<ProbeAsset> again =
        Assets().AddReady<ProbeAsset>(id, make_unique<ProbeAsset>(second, false));
    ASSERT_TRUE(again.IsReady());
    EXPECT_EQ(Assets().GetAssetCount(), 1u);
    EXPECT_EQ(second->Unloaded, 0u);
    EXPECT_EQ(first->Unloaded, 1u);
}

/// 空引用不指向槽位, 且各项查询都给出安全的答案 (不解引用 nullptr)。
TEST_F(AssetSlotTest, EmptyReferenceAnswersEverythingSafely) {
    StreamingAssetRef<ProbeAsset> empty;
    EXPECT_FALSE(empty.IsValid());
    EXPECT_FALSE(empty.IsReady());
    EXPECT_FALSE(empty.IsCompleted());
    EXPECT_FALSE(empty.IsFaulted());
    EXPECT_FALSE(empty.IsCanceled());
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_EQ(empty.Get(), nullptr);
    empty.Cancel();  // 无操作, 不得崩。
    empty.Reset();
}

/// 相等【比较槽位身份, 不比较 AssetId】。两个空引用的 id 都是空, 若按 id 比就会相等 ——
/// 那会让"两个空引用指向同一资产"这种荒谬结论成立。
TEST_F(AssetSlotTest, EqualityComparesSlotIdentityNotAssetId) {
    shared_ptr<Counters> a = MakeCounters();
    shared_ptr<Counters> b = MakeCounters();
    StreamingAssetRef<ProbeAsset> first =
        Assets().AddReady<ProbeAsset>(MakeId(12), make_unique<ProbeAsset>(a, false));
    StreamingAssetRef<ProbeAsset> second =
        Assets().AddReady<ProbeAsset>(MakeId(13), make_unique<ProbeAsset>(b, false));
    ASSERT_TRUE(first.IsReady() && second.IsReady());

    EXPECT_TRUE(first == first);
    EXPECT_FALSE(first == second);

    StreamingAssetRef<ProbeAsset> emptyA;
    StreamingAssetRef<ProbeAsset> emptyB;
    EXPECT_TRUE(emptyA == emptyB) << "two empty refs are both 'no slot' — that is the same slot";
    EXPECT_FALSE(emptyA == first);
}

/// 类型判定走 RuntimeTypeTrait 的 Bases 图, 故基类也匹配; CastTo 在类型不符时给空引用。
TEST_F(AssetSlotTest, TypedReferenceRefusesAMismatchedType) {
    shared_ptr<Counters> counters = MakeCounters();
    StreamingAssetRefAny any = Assets().AddReady(
        MakeId(14),
        unique_ptr<Asset>{make_unique<ProbeAsset>(counters, false)},
        runtime_type_info_v<ProbeAsset>);
    ASSERT_TRUE(any.IsReady());

    EXPECT_TRUE(any.Is<ProbeAsset>());
    EXPECT_TRUE(any.Is<Asset>()) << "Bases should make the base type match too";

    StreamingAssetRef<ProbeAsset> typed = any.CastTo<ProbeAsset>();
    ASSERT_TRUE(typed.IsReady());
    EXPECT_EQ(typed.Get(), any.Get());
    EXPECT_TRUE(typed == any);
}

// ════════════════════════════════════════════════════════════
//  在飞加载
// ════════════════════════════════════════════════════════════

/// 加载期间 manager 自持一份引用, 故调用方可以立刻丢弃返回值做预热。
TEST_F(AssetSlotTest, LoadHoldsItsOwnReferenceWhileInFlight) {
    shared_ptr<Counters> counters = MakeCounters();
    const AssetId id = MakeId(15);

    ManualGate gate;
    StreamingAssetRef<ProbeAsset> ref = Assets().Load<ProbeAsset>(AssetLoadRequest{
        .Id = id,
        .Task = [](ManualGate* g) -> task<AssetLoadResult> {
            co_await g->Wait();
            co_return AssetLoadResult::Failure("deliberate failure");
        }(&gate),
        .DebugName = "probe"});
    ASSERT_TRUE(gate.IsPending());
    EXPECT_TRUE(ref.IsValid());
    EXPECT_FALSE(ref.IsCompleted()) << "still Loading";
    EXPECT_EQ(ref.Get(), nullptr) << "Loading must not hand out the asset";
    EXPECT_EQ(counters->Unloaded, 0u);

    gate.Resume();
    Assets().Pump();
    // 这次是失败, 故槽位是 Faulted 而非 Ready。
    EXPECT_TRUE(ref.IsFaulted());
    EXPECT_FALSE(ref.IsReady());
    EXPECT_TRUE(ref.IsCompleted());
}

/// 【外部引用全部放手不会取消在飞加载】。这是一条刻意的策略: 加载多半已花掉大半代价
/// (IO 已完成、GPU 上传已提交), 半途取消既救不回那部分开销, 又要求每个 loader 都写
/// "取消后如何回退"。让它跑完, 之后按常规归零回收。
///
/// 若哪天有人改成自动取消, 这个用例会失败并把讨论拉到该改的地方。
TEST_F(AssetSlotTest, DroppingEveryExternalRefLetsTheLoadRunToCompletion) {
    shared_ptr<Counters> counters = MakeCounters();
    bool taskObservedStop = false;

    ManualGate gate;
    {
        StreamingAssetRef<ProbeAsset> ref = Assets().Load<ProbeAsset>(AssetLoadRequest{
            .Id = MakeId(16),
            .Task = [](ManualGate* g, bool* observed, shared_ptr<Counters> c) -> task<AssetLoadResult> {
                co_await g->Wait();
                stop_token stop = co_await CurrentStopToken();
                *observed = stop.stop_requested();
                co_return AssetLoadResult::Success(make_unique<ProbeAsset>(c, false));
            }(&gate, &taskObservedStop, counters)});
        ASSERT_TRUE(gate.IsPending());
        EXPECT_EQ(Assets().GetAssetCount(), 1u);
    }
    // 外部放手了, 但 manager 自持的那份引用还在 —— 槽位存活, 加载继续。
    EXPECT_EQ(Assets().GetAssetCount(), 1u);
    EXPECT_TRUE(gate.IsPending());

    gate.Resume();
    EXPECT_FALSE(taskObservedStop) << "dropping external refs must NOT request_stop the load";

    // Pump 让结果落地并放开加载那份引用; 此刻已无人持有, 同一次 Pump 里即被回收。
    Assets().Pump();
    EXPECT_EQ(Assets().GetAssetCount(), 0u);
    EXPECT_EQ(counters->Unloaded, 1u) << "the completed asset is collected once nobody wants it";
    EXPECT_EQ(counters->Destroyed, 1u);
}

/// 同 id 的第二次 Load 不再启动加载, 直接复用在飞槽位。第二个 task 【一次都不能跑】——
/// 否则同一份资源会被解析两遍, 而那是最贵也最难看出来的一类浪费。
TEST_F(AssetSlotTest, LoadDeduplicatesByAssetId) {
    shared_ptr<Counters> counters = MakeCounters();
    const AssetId id = MakeId(17);
    uint32_t started = 0;
    auto makeTask = [counters, &started]() -> task<AssetLoadResult> {
        ++started;
        co_return AssetLoadResult::Success(make_unique<ProbeAsset>(counters, false));
    };

    StreamingAssetRef<ProbeAsset> first =
        Assets().Load<ProbeAsset>(AssetLoadRequest{.Id = id, .Task = makeTask()});
    StreamingAssetRef<ProbeAsset> second =
        Assets().Load<ProbeAsset>(AssetLoadRequest{.Id = id, .Task = makeTask()});
    EXPECT_EQ(Assets().GetAssetCount(), 1u);
    EXPECT_EQ(started, 1u) << "the deduplicated request's task must never be spawned";
    EXPECT_TRUE(first == second) << "dedup is observable as reference equality";

    Assets().Pump();
    ASSERT_TRUE(first.IsReady());
    ASSERT_TRUE(second.IsReady());
    EXPECT_EQ(first.Get(), second.Get());
}

/// 显式 Cancel 让槽位进入 Canceled 终态, 而不是 Faulted —— 两者要能区分, 否则调用方
/// 分不清"加载失败要报错"与"我自己取消了, 不必报错"。
TEST_F(AssetSlotTest, ExplicitCancelEndsInCanceledNotFaulted) {
    ManualGate gate;
    StreamingAssetRef<ProbeAsset> ref = Assets().Load<ProbeAsset>(AssetLoadRequest{
        .Id = MakeId(18),
        .Task = [](ManualGate* g) -> task<AssetLoadResult> {
            co_await g->Wait();
            stop_token stop = co_await CurrentStopToken();
            if (stop.stop_requested()) {
                co_await StopCurrentTask();
            }
            co_return AssetLoadResult::Failure("not reached");
        }(&gate)});
    ASSERT_TRUE(gate.IsPending());

    ref.Cancel();
    gate.Resume();
    Assets().Pump();

    EXPECT_TRUE(ref.IsCompleted());
    EXPECT_TRUE(ref.IsCanceled());
    EXPECT_FALSE(ref.IsFaulted());
    EXPECT_FALSE(ref.IsReady());
    EXPECT_EQ(ref.Get(), nullptr);
}

/// 【取消是协作式的】: Cancel 只 request_stop, 它不能中止一个不看 stop token 的加载 ——
/// 那个 task 仍会跑完并把结果交上来, 槽位进 Ready 而非 Canceled。
///
/// 这条看似在"断言一个缺点", 但它固定的是 loader 的编写契约: 想让 Cancel 真正生效, loader
/// 必须在自己的挂起点上查 stop token (或经 AwaitWithStopToken 等待 sender)。
TEST_F(AssetSlotTest, CancelDoesNotAbortALoadThatIgnoresItsStopToken) {
    shared_ptr<Counters> counters = MakeCounters();
    ManualGate gate;
    StreamingAssetRef<ProbeAsset> ref = Assets().Load<ProbeAsset>(AssetLoadRequest{
        .Id = MakeId(19),
        .Task = [](ManualGate* g, shared_ptr<Counters> c) -> task<AssetLoadResult> {
            co_await g->Wait();  // 刻意不查 stop token。
            co_return AssetLoadResult::Success(make_unique<ProbeAsset>(c, false));
        }(&gate, counters)});
    ref.Cancel();
    gate.Resume();
    Assets().Pump();

    EXPECT_TRUE(ref.IsReady());
    EXPECT_FALSE(ref.IsCanceled());
}

/// 加载结果的运行时类型元数据与最终实例不符时置 Faulted, 不上架一个类型说谎的资产。
TEST_F(AssetSlotTest, MismatchedTypeMetadataFaultsTheSlot) {
    shared_ptr<Counters> counters = MakeCounters();
    StreamingAssetRefAny ref = Assets().Load(AssetLoadRequest{
        .Id = MakeId(20),
        .Task = [](shared_ptr<Counters> c) -> task<AssetLoadResult> {
            // 实例是 ProbeAsset, 却报 Asset 的类型描述符。
            co_return AssetLoadResult::Success(
                unique_ptr<Asset>{make_unique<ProbeAsset>(c, false)},
                runtime_type_info_v<Asset>);
        }(counters)});
    Assets().Pump();

    EXPECT_TRUE(ref.IsFaulted());
    EXPECT_EQ(ref.Get(), nullptr);
    // 元数据校验发生在上架之前, 故那个实例根本没进槽位 —— 它随 AssetLoadResult 一起析构,
    // 也就不会走 OnUnload。
    EXPECT_EQ(counters->Unloaded, 0u);
    EXPECT_EQ(counters->Destroyed, 1u);
}

/// 没人持有的 Faulted 槽位会被回收, id 随之释放 —— 故拿对参数重试不会被 dedup 命中一个
/// 坏槽位。这是"引用计数是唯一权威"买到的一个具体好处。
TEST_F(AssetSlotTest, UnreferencedFaultedSlotIsCollectedSoTheIdIsFree) {
    const AssetId id = MakeId(21);
    {
        StreamingAssetRefAny ref = Assets().Load(AssetLoadRequest{
            .Id = id,
            .Task = []() -> task<AssetLoadResult> {
                co_return AssetLoadResult::Failure("deliberate");
            }()});
        Assets().Pump();
        ASSERT_TRUE(ref.IsFaulted());
    }
    Assets().Pump();
    EXPECT_EQ(Assets().GetAssetCount(), 0u);

    shared_ptr<Counters> counters = MakeCounters();
    StreamingAssetRef<ProbeAsset> retried =
        Assets().AddReady<ProbeAsset>(id, make_unique<ProbeAsset>(counters, false));
    EXPECT_TRUE(retried.IsReady()) << "the id must be free after the faulted slot is collected";
}

// ════════════════════════════════════════════════════════════
//  co_await 一份引用
// ════════════════════════════════════════════════════════════

/// 等待者在 Pump 提交结果之后被恢复。
TEST_F(AssetSlotTest, AwaitingAReferenceResumesAfterPump) {
    shared_ptr<Counters> counters = MakeCounters();
    bool resumed = false;
    bool sawReady = false;

    ManualGate gate;
    StreamingAssetRef<ProbeAsset> ref = Assets().Load<ProbeAsset>(AssetLoadRequest{
        .Id = MakeId(22),
        .Task = [](ManualGate* g, shared_ptr<Counters> c) -> task<AssetLoadResult> {
            co_await g->Wait();
            co_return AssetLoadResult::Success(make_unique<ProbeAsset>(c, false));
        }(&gate, counters)});

    TaskScope waiters;
    waiters.Spawn([](StreamingAssetRef<ProbeAsset> r, bool* resumedOut, bool* readyOut) -> task<void> {
        const bool completed = co_await r;
        *resumedOut = completed;
        *readyOut = r.IsReady();
    }(ref, &resumed, &sawReady));

    EXPECT_FALSE(resumed) << "the waiter must not resume before the load completes";

    gate.Resume();
    // 加载协程已写好结果, 但槽位仍是 Loading —— 终态只在 Pump 里发生, 等待者也只在那里恢复。
    EXPECT_FALSE(resumed) << "leaving Loading is Pump's job, not the load coroutine's";

    Assets().Pump();
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(sawReady) << "the asset must already be published when the waiter resumes";
    waiters.WaitUntilEmpty();
}

/// 已是终态的引用不挂起 (await_ready 为真)。
TEST_F(AssetSlotTest, AwaitingACompletedReferenceDoesNotSuspend) {
    shared_ptr<Counters> counters = MakeCounters();
    StreamingAssetRef<ProbeAsset> ref =
        Assets().AddReady<ProbeAsset>(MakeId(23), make_unique<ProbeAsset>(counters, false));
    ASSERT_TRUE(ref.IsReady());

    bool resumed = false;
    TaskScope waiters;
    waiters.Spawn([](StreamingAssetRef<ProbeAsset> r, bool* out) -> task<void> {
        *out = co_await r;
    }(ref, &resumed));
    EXPECT_TRUE(resumed);
    waiters.WaitUntilEmpty();
}

/// 空引用也算"等到了" —— 没有东西可等。这条防的是把空引用当成永久挂起。
TEST_F(AssetSlotTest, AwaitingAnEmptyReferenceCompletesImmediately) {
    bool resumed = false;
    TaskScope waiters;
    waiters.Spawn([](bool* out) -> task<void> {
        StreamingAssetRef<ProbeAsset> empty;
        *out = co_await empty;
    }(&resumed));
    EXPECT_TRUE(resumed);
    waiters.WaitUntilEmpty();
}

/// 【等待者被取消 ≠ 资产加载失败】。await_resume 返回 false 表达的是前者, 而
/// AssetManager::Wait 会把它转成对当前 task 的 stop 传播 —— 于是等待方不会误把
/// "我自己不等了"读成"资产坏了"。取消等待者【不】取消底层加载。
TEST_F(AssetSlotTest, CancelingAWaiterDoesNotCancelTheLoad) {
    shared_ptr<Counters> counters = MakeCounters();
    bool waiterFinished = false;
    bool waiterSawCompletion = true;

    ManualGate gate;
    StreamingAssetRef<ProbeAsset> ref = Assets().Load<ProbeAsset>(AssetLoadRequest{
        .Id = MakeId(24),
        .Task = [](ManualGate* g, shared_ptr<Counters> c) -> task<AssetLoadResult> {
            co_await g->Wait();
            co_return AssetLoadResult::Success(make_unique<ProbeAsset>(c, false));
        }(&gate, counters)});

    {
        TaskScope waiters;
        waiters.Spawn([](StreamingAssetRef<ProbeAsset> r, bool* finished, bool* sawCompletion) -> task<void> {
            *sawCompletion = co_await r;
            *finished = true;
        }(ref, &waiterFinished, &waiterSawCompletion));
        ASSERT_FALSE(waiterFinished);
        // ~TaskScope 会 RequestStop + WaitUntilEmpty: 等待者被取消并就地恢复。
    }
    EXPECT_TRUE(waiterFinished);
    EXPECT_FALSE(waiterSawCompletion) << "false means 'the waiter was canceled', not 'the load failed'";

    // 加载本身没被牵连。
    ASSERT_TRUE(gate.IsPending());
    gate.Resume();
    Assets().Pump();
    EXPECT_TRUE(ref.IsReady()) << "canceling a waiter must not cancel the underlying load";
}

// ════════════════════════════════════════════════════════════
//  关停
// ════════════════════════════════════════════════════════════

/// AssetManager 先死时仍无条件对每个槽位走一遍 OnUnload。持有外部引用不能阻止它 ——
/// 那是使用错误 (会记 error log), 但 GPU 资源必须在 device 之前交出去, 否则泄漏比悬垂
/// 更难查。这条固定的是"关停时选悬垂而非泄漏"这个决定。
TEST_F(AssetSlotTest, ManagerDestructionUnloadsEverySlotEvenWhenStillReferenced) {
    shared_ptr<Counters> counters = MakeCounters();
    CountingWaitFrame waitFrame;
    StreamingAssetRef<ProbeAsset> leaked;

    {
        AssetManager assets;
        assets.SetWaitFrameProcessor(&waitFrame);
        leaked = assets.AddReady<ProbeAsset>(MakeId(25), make_unique<ProbeAsset>(counters, true));
        ASSERT_TRUE(leaked.IsReady());
    }

    EXPECT_EQ(counters->Unloaded, 1u);
    EXPECT_EQ(counters->Destroyed, 1u);
    // payload 交出后已无从等待帧边界 (关停时 _loadScope 已停), 故就地销毁 —— 关停路径
    // 在此之前已 device wait-idle 过。
    EXPECT_EQ(counters->PayloadDestroyed, 1u) << "payloads must not leak past the device";
    // 残留引用此时是悬垂的。放掉它不得崩 —— 计数减到 0 后没有任何后续动作 (销毁对齐到
    // Pump, 而 manager 已经没了)。
    leaked.Reset();
}

/// 关停时【已写好但还没 Pump 的结果】仍会落地, 那个资产照样走 OnUnload。
///
/// 【为何这条值得单独测】: ~AssetManager 里刻意不调 Pump 而是手动走 PumpLoadResults +
/// CollectZeroRefSlots (Pump 的第三步会 spawn 一个必然立刻被取消的协程)。漏掉第一步的
/// 后果正是这里断言的东西: 资产留在 PendingResult 里随 optional 析构, 永远不走 OnUnload,
/// 于是它持有的 GPU 对象活过 device。
TEST_F(AssetSlotTest, ShutdownCommitsPendingResultsSoTheyStillUnload) {
    shared_ptr<Counters> counters = MakeCounters();
    CountingWaitFrame waitFrame;

    {
        AssetManager assets;
        assets.SetWaitFrameProcessor(&waitFrame);
        // 无挂起点的 task 在 Load 返回前就跑完, 结果已写进 PendingResult 但尚未 Pump。
        StreamingAssetRefAny ref = assets.Load(AssetLoadRequest{
            .Id = MakeId(26),
            .Task = [](shared_ptr<Counters> c) -> task<AssetLoadResult> {
                co_return AssetLoadResult::Success(make_unique<ProbeAsset>(c, true));
            }(counters)});
        ASSERT_TRUE(ref.IsValid());
        ASSERT_FALSE(ref.IsCompleted()) << "the result is pending; committing it is Pump's job";
        EXPECT_EQ(counters->Unloaded, 0u);
    }

    EXPECT_EQ(counters->Unloaded, 1u) << "a pending result must still be committed and unloaded";
    EXPECT_EQ(counters->Destroyed, 1u);
    EXPECT_EQ(counters->PayloadDestroyed, 1u);
}

}  // namespace
}  // namespace radray
