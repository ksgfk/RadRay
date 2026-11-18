#include <cstdlib>
#include <functional>

#include <gtest/gtest.h>
#include <radray/runtime/asset_manager.h>

namespace radray {
namespace {

static_assert(sizeof(AssetHandle<int>) == 8 && sizeof(UntypedAssetHandle) == 8);
static_assert(std::is_trivially_copyable_v<AssetHandle<int>>);
static_assert(sizeof(void*) != 8 || sizeof(AssetRef<int>) == 16);

class ManualGate {
public:
    task<bool> Wait(stop_token stop) {
        co_return co_await Awaiter{*this, stop};
    }
    void Open(AssetManager& assets) {
        assets.DispatchExternal([&] { _waiters.DispatchReady([](const auto&) { return true; }); });
    }
    size_t Count() const noexcept { return _waiters.Count(); }

private:
    struct Awaiter {
        ManualGate& Gate;
        stop_token Stop;
        Nullable<ManualCoroutineRecord*> Record{nullptr};
        bool await_ready() const noexcept { return Stop.stop_requested(); }
        void await_suspend(std::coroutine_handle<> h) {
            Record = Gate._waiters.Enqueue(Stop, h);
        }
        bool await_resume() noexcept {
            if (!Record) return false;
            const bool completed = !Record->Canceled;
            Gate._waiters.Erase(Record.Get());
            return completed;
        }
    };
    ManualCoroutineScheduler<ManualCoroutineRecord> _waiters;
};

class NumberLoader final : public AssetLoader<int> {
public:
    using Load = std::function<task<std::optional<int>>(AssetLoadContext&)>;
    explicit NumberLoader(Load load) : _load(std::move(load)) {}
    task<std::optional<int>> LoadAsync(AssetLoadContext& ctx) override {
        ++Calls;
        co_return co_await _load(ctx);
    }
    Nullable<const int*> GetFallback() const noexcept override { return &FallbackValue; }
    size_t Calls{};
    int FallbackValue{-1};

private:
    Load _load;
};

// 外层 optional 区分 stopped 与正常返回的空 AssetRef。
task<void> Observe(AssetManager& assets, AssetId id, stop_token stop, std::optional<AssetRef<int>>& out, bool& finished) {
    out = co_await AwaitWithStopToken(assets.LoadAsync<int>(id), stop);
    finished = true;
}

TEST(AssetManagerTest, RevivedAssetCanBeCollectedAfterItsFinalRelease) {
    AssetManager assets;
    auto owner = assets.Emplace(42);
    const auto handle = owner.GetHandle();
    auto copy = owner;
    auto moved = std::move(copy);
    EXPECT_FALSE(copy);
    EXPECT_EQ(assets.GetRefCount(handle.Untyped()), 2u);
    moved.Reset();
    owner.Reset();
    auto revived = assets.Acquire(handle);
    EXPECT_EQ(assets.CollectGarbage(), 0u);
    ASSERT_TRUE(revived.Get());
    EXPECT_EQ(*revived.Get(), 42);
    revived.Reset();
    EXPECT_EQ(assets.CollectGarbage(), 1u);
    EXPECT_FALSE(assets.Acquire(handle));
    const auto replacement = assets.Emplace(12);
    EXPECT_EQ(replacement.GetHandle().GetIndex(), handle.GetIndex());
    EXPECT_NE(replacement.GetHandle().GetGeneration(), handle.GetGeneration());
    EXPECT_FALSE(assets.Get(handle));
}

TEST(AssetManagerTest, FormatsReusedSlotAsAssetHandle) {
    AssetManager assets;
    auto first = assets.Emplace(1);
    first.Reset();
    ASSERT_EQ(assets.CollectGarbage(), 1u);
    const auto second = assets.Emplace(2);
    EXPECT_EQ(fmt::format("{}", second.GetHandle()), "AssetHandle{Index=0, Generation=1}");
}

TEST(AssetManagerTest, NamedFactoryReservationRollsBackAndRejectsReentry) {
    AssetManager assets;
    const auto id = MakeAssetId("named");
    AssetHandle<int> reserved;
    EXPECT_FALSE(assets.EmplaceWithHandle<int>({}, id, [&](AssetHandle<int> self) -> std::optional<int> {
        reserved = self;
        EXPECT_FALSE(assets.Acquire(self));
        EXPECT_FALSE(assets.FindLoaded(id));
        EXPECT_FALSE(assets.Request<int>(id));
        EXPECT_FALSE(assets.Emplace(2, {}, id));
        for (int i = 0; i != 40; ++i) assets.Emplace(i);
        return std::nullopt;
    }));
    const auto value = assets.Emplace(3, {}, id);
    ASSERT_TRUE(value.Get());
    EXPECT_NE(reserved, value.GetHandle());
    EXPECT_EQ(assets.FindLoaded(id), value.Untyped());
    EXPECT_FALSE(assets.Request<double>(id));
    EXPECT_FALSE(assets.Acquire(reserved));
    EXPECT_EQ(assets.Request<int>(id).GetHandle(), value.GetHandle());
}

TEST(AssetManagerTest, DestructorCannotReacquireRetiredAsset) {
    AssetManager assets;
    using Probe = ScopeGuard<std::function<void()>>;
    AssetHandle<Probe> old;
    auto owner = assets.EmplaceWithHandle<Probe>({}, {}, [&](AssetHandle<Probe> self) {
        old = self;
        return std::optional<Probe>{std::in_place, std::function<void()>{[&] {
                                        EXPECT_FALSE(assets.Acquire(old));
                                        for (int i = 0; i != 32; ++i) assets.Emplace(i);
                                    }}};
    });
    ASSERT_TRUE(owner);
    owner.Reset();
    EXPECT_EQ(assets.CollectGarbage(), 33u);
}

TEST(AssetManagerTest, RequestIsLazyAndCancellationIsIsolatedFromOtherWaiters) {
    AssetManager assets;
    ManualGate gate;
    const auto id = MakeAssetId("number");
    auto& loader = assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
        EXPECT_TRUE(ctx.GetTargetHandle<int>().IsValid());
        EXPECT_FALSE(ctx.GetTargetHandle<double>().IsValid());
        if (!co_await gate.Wait(ctx.GetStopToken())) co_await StopCurrentTask();
        co_return 42;
    });
    EXPECT_FALSE(assets.RegisterMeta<int>({{}, {}, {}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({id, {}, {}}));
    EXPECT_FALSE(assets.RegisterMeta<double>({id, {}, {}}));
    EXPECT_FALSE(assets.Request<double>(id));
    EXPECT_FALSE(assets.Request<int>(MakeAssetId("missing")));
    stop_source firstStop, secondStop;
    std::optional<AssetRef<int>> first, second;
    bool firstDone = false, secondDone = false;
    TaskScope callers;
    callers.Spawn(Observe(assets, id, firstStop.get_token(), first, firstDone));
    callers.Spawn(Observe(assets, id, secondStop.get_token(), second, secondDone));
    auto owner = assets.Request<int>(id);
    EXPECT_FALSE(owner.Get());
    EXPECT_EQ(owner.Resolve().Get(), &loader.FallbackValue);
    EXPECT_EQ(loader.Calls, 0u);
    assets.Pump();
    EXPECT_EQ(gate.Count(), 1u);
    EXPECT_EQ(loader.Calls, 1u);
    firstStop.request_stop();
    EXPECT_TRUE(firstDone);
    EXPECT_FALSE(first);
    EXPECT_FALSE(secondDone);
    EXPECT_TRUE(assets.HasPendingWork());
    gate.Open(assets);
    assets.Pump();
    ASSERT_TRUE(secondDone && second && second->Get());
    EXPECT_EQ(*second->Get(), 42);
    EXPECT_FALSE(assets.HasPendingWork());
    firstDone = false;
    callers.Spawn(Observe(assets, id, firstStop.get_token(), first, firstDone));
    EXPECT_TRUE(firstDone);
    EXPECT_FALSE(first);
    secondDone = false;
    callers.Spawn(Observe(assets, id, {}, second, secondDone));
    EXPECT_TRUE(secondDone);
    EXPECT_EQ(loader.Calls, 1u);
}

TEST(AssetManagerTest, LoaderFailureAndStoppedBothPublishFailureAndPermitRetryAfterCollection) {
    AssetManager assets;
    const auto id = MakeAssetId("failed");
    int attempt = 0;
    auto& loader = assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext&) -> task<std::optional<int>> {
        if (++attempt == 1) co_await StopCurrentTask();
        co_return std::nullopt;
    });
    ASSERT_TRUE(assets.RegisterMeta<int>({id, {}, {}}));
    TaskScope callers;
    for (int i = 0; i < 2; ++i) {
        auto owner = assets.Request<int>(id);
        std::optional<AssetRef<int>> out;
        bool done = false;
        callers.Spawn(Observe(assets, id, {}, out, done));
        assets.Pump();
        EXPECT_EQ(owner.GetState(), AssetState::Failed);
        EXPECT_EQ(owner.Resolve().Get(), &loader.FallbackValue);
        ASSERT_TRUE(done && out);
        EXPECT_FALSE(*out);
        done = false;
        callers.Spawn(Observe(assets, id, {}, out, done));
        EXPECT_TRUE(done);
        owner.Reset();
        EXPECT_EQ(assets.CollectGarbage(), 1u);
    }
    EXPECT_EQ(attempt, 2);
}

TEST(AssetManagerTest, StaticAndDynamicDependenciesShareOneChildAndCompleteInOnePump) {
    AssetManager assets;
    const auto parent = MakeAssetId("parent"), branch = MakeAssetId("branch"), leaf = MakeAssetId("leaf");
    assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
        if (ctx.GetMeta().Id == leaf) co_return 10;
        auto child = co_await ctx.LoadDependencyAsync<int>(leaf);
        if (!child.IsValid()) co_return std::nullopt;
        int value = *ctx.GetDependency<int>(leaf);
        if (ctx.GetMeta().Id == parent) value += *ctx.GetDependency<int>(branch);
        co_return value;
    });
    ASSERT_TRUE(assets.RegisterMeta<int>({parent, {}, {branch, leaf, leaf}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({branch, {}, {}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({leaf, {}, {}}));
    TaskScope callers;
    std::optional<AssetRef<int>> out;
    bool done = false;
    callers.Spawn(Observe(assets, parent, {}, out, done));
    assets.Pump();
    ASSERT_TRUE(done && out && out->Get());
    EXPECT_EQ(*out->Get(), 20);
    EXPECT_EQ(assets.GetRefCount(*assets.FindLoaded(leaf)), 2u);
    out.reset();
    EXPECT_EQ(assets.CollectGarbage(), 3u);
}

TEST(AssetManagerTest, StaticDependencyRetainsNamedMemoryAssetOfAnotherType) {
    AssetManager assets;
    const auto parent = MakeAssetId("parent"), leaf = MakeAssetId("memory");
    auto memory = assets.Emplace(23L, {}, leaf);
    ASSERT_TRUE(memory);
    assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
        EXPECT_FALSE(ctx.GetDependency<int>(leaf));
        auto value = ctx.GetDependency<long>(leaf);
        if (!value) co_return std::nullopt;
        co_return static_cast<int>(*value);
    });
    ASSERT_TRUE(assets.RegisterMeta<int>({parent, {}, {leaf}}));
    auto root = assets.Request<int>(parent);
    assets.Pump();
    ASSERT_TRUE(root.Get());
    EXPECT_EQ(*root.Get(), 23);
    memory.Reset();
    EXPECT_EQ(assets.CollectGarbage(), 0u);
    root.Reset();
    EXPECT_EQ(assets.CollectGarbage(), 2u);
}

TEST(AssetManagerTest, CyclesAndFailedSiblingReleaseAcquiredChildrenOnce) {
    AssetManager assets;
    const auto a = MakeAssetId("a"), b = MakeAssetId("b"), good = MakeAssetId("good"), self = MakeAssetId("self");
    assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
        if (ctx.GetMeta().Id == b) (void)co_await ctx.LoadDependencyAsync<int>(a);
        co_return 1;
    });
    ASSERT_TRUE(assets.RegisterMeta<int>({a, {}, {good, b}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({b, {}, {}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({good, {}, {}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({self, {}, {self}}));
    auto first = assets.Request<int>(a), recursive = assets.Request<int>(self);
    auto keep = assets.Request<int>(good);
    assets.Pump();
    EXPECT_EQ(first.GetState(), AssetState::Failed);
    EXPECT_EQ(recursive.GetState(), AssetState::Failed);
    EXPECT_TRUE(keep.IsReady());
    EXPECT_EQ(assets.GetRefCount(keep.Untyped()), 1u);
    first.Reset();
    recursive.Reset();
    EXPECT_EQ(assets.CollectGarbage(), 3u);
    EXPECT_EQ(assets.GetRefCount(keep.Untyped()), 1u);
}

TEST(AssetManagerTest, ZeroReferencesDuringLoadAreCollectedOnCompletion) {
    AssetManager assets;
    ManualGate gate;
    const auto id = MakeAssetId("gate");
    assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
        co_await gate.Wait(ctx.GetStopToken());
        co_return 7;
    });
    ASSERT_TRUE(assets.RegisterMeta<int>({id, {}, {}}));
    const auto weak = assets.Request<int>(id).GetHandle();
    assets.Pump();
    EXPECT_EQ(assets.CollectGarbage(), 0u);
    gate.Open(assets);
    EXPECT_EQ(assets.GetState(weak.Untyped()), AssetState::Ready);
    EXPECT_EQ(assets.CollectGarbage(), 1u);
}

TEST(AssetManagerTest, ShutdownStopsPendingAndSuspendedLoadsAndPreservesHeldAssets) {
    AssetManager assets;
    ManualGate gate;
    const auto a = MakeAssetId("a"), b = MakeAssetId("b"), pending = MakeAssetId("pending");
    assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
        if (!co_await gate.Wait(ctx.GetStopToken())) co_await StopCurrentTask();
        co_return 1;
    });
    ASSERT_TRUE(assets.RegisterMeta<int>({a, {}, {b}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({b, {}, {}}));
    ASSERT_TRUE(assets.RegisterMeta<int>({pending, {}, {}}));
    auto ready = assets.Emplace(17);
    auto first = assets.Request<int>(a);
    assets.Pump();
    ASSERT_EQ(gate.Count(), 1u);
    auto last = assets.Request<int>(pending);
    assets.Shutdown();
    assets.Shutdown();
    EXPECT_EQ(gate.Count(), 0u);
    EXPECT_FALSE(assets.HasPendingWork());
    EXPECT_EQ(first.GetState(), AssetState::Failed);
    EXPECT_EQ(last.GetState(), AssetState::Failed);
    EXPECT_FALSE(assets.Request<int>(a));
    EXPECT_FALSE(assets.Acquire(ready.GetHandle()));
    auto copied = ready;
    EXPECT_EQ(*copied.Get(), 17);
}

TEST(AssetManagerTest, RejectsShutdownFromExternallyResumedLoader) {
    EXPECT_DEATH(([] {
                     AssetManager assets;
                     ManualGate gate;
                     const auto id = MakeAssetId("gate");
                     assets.RegisterLoader<int, NumberLoader>([&](AssetLoadContext& ctx) -> task<std::optional<int>> {
                         co_await gate.Wait(ctx.GetStopToken());
                         assets.Shutdown();
                         std::_Exit(0);
                         co_return 7;
                     });
                     assets.RegisterMeta<int>({id, {}, {}});
                     auto owner = assets.Request<int>(id);
                     assets.Pump();
                     gate.Open(assets);
                 }()),
                 "");
}

TEST(AssetManagerTest, RejectsShutdownFromCanceledWaiterContinuation) {
    EXPECT_DEATH(([] {
                     AssetManager assets;
                     assets.RegisterLoader<int, NumberLoader>([](AssetLoadContext&) -> task<std::optional<int>> { co_return 1; });
                     const auto id = MakeAssetId("never-started");
                     assets.RegisterMeta<int>({id, {}, {}});
                     stop_source stop;
                     TaskScope callers;
                     callers.Spawn([](AssetManager& manager, AssetId target, stop_token token) -> task<void> {
                         (void)co_await AwaitWithStopToken(manager.LoadAsync<int>(target), token);
                         manager.Shutdown();
                         std::_Exit(0);
                     }(assets, id, stop.get_token()));
                     stop.request_stop();
                 }()),
                 "");
}

TEST(AssetManagerTest, RejectsManagerDestructionWithExternalReferences) {
    EXPECT_DEATH({
        AssetRef<int> owner;
        { AssetManager assets; owner = assets.Emplace(1); }
        std::_Exit(0); }, "");
}

}  // namespace
}  // namespace radray
