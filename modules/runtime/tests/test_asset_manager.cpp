#include <cstdlib>
#include <functional>

#include <gtest/gtest.h>
#include <radray/runtime/asset_manager.h>

namespace radray {
namespace {

static_assert(sizeof(AssetHandle<int>) == 8 && sizeof(UntypedAssetHandle) == 8);
static_assert(std::is_trivially_copyable_v<AssetHandle<int>>);
static_assert(sizeof(void*) != 8 || sizeof(AssetRef<int>) == 16);

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
    EXPECT_FALSE(assets.HasPendingGarbage());
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
        EXPECT_FALSE(assets.Find(id));
        EXPECT_FALSE(assets.Find<int>(id).IsValid());
        EXPECT_FALSE(assets.Emplace(2, {}, id));
        for (int i = 0; i != 40; ++i) assets.Emplace(i);
        return std::nullopt;
    }));
    const auto value = assets.Emplace(3, {}, id);
    ASSERT_TRUE(value.Get());
    EXPECT_NE(reserved, value.GetHandle());
    EXPECT_EQ(assets.Find(id), value.Untyped());
    EXPECT_FALSE(assets.Find<double>(id).IsValid());
    EXPECT_FALSE(assets.Acquire(reserved));
    EXPECT_EQ(assets.Find<int>(id), value.GetHandle());
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

TEST(AssetManagerTest, CommitPreservesIdentityAndRetiresThePreviousValue) {
    AssetManager assets;
    auto ref = assets.Emplace(42);
    const auto previous = ref.Get();
    AssetTransaction tx(assets);
    ASSERT_TRUE(tx.StageReplace(ref, 43));
    AssetGarbageBatch retired;
    auto result = assets.Commit(std::move(tx), retired);
    ASSERT_TRUE(result.Succeeded);
    ASSERT_EQ(result.Events.size(), 1u);
    EXPECT_EQ(result.Events[0].Revision, 2u);
    EXPECT_EQ(result.Events[0].Handle, ref.Untyped());
    EXPECT_EQ(*ref.Get(), 43);
    EXPECT_EQ(*previous, 42);
    EXPECT_EQ(retired.Size(), 1u);
}

TEST(AssetManagerTest, RetiredParentKeepsItsChildUntilItsDestructorFinishes) {
    for (bool delayed : {false, true}) {
        bool destroyed = false;
        AssetManager assets;
        auto child = assets.Emplace(15);
        auto weak = child.GetHandle();
        auto parent = assets.Emplace(MakeScopeGuard([&, weak]() noexcept {
                                         ASSERT_TRUE(assets.Get(weak));
                                         EXPECT_EQ(*assets.Get(weak), 15);
                                         destroyed = true;
                                     }),
                                     array{child.Untyped()});
        child.Reset();
        parent.Reset();
        if (delayed) {
            AssetGarbageBatch batch;
            EXPECT_EQ(assets.CollectGarbage(batch), 1u);
            EXPECT_FALSE(destroyed);
            EXPECT_TRUE(assets.GetDependents(weak.Untyped()).empty());
            auto moved = std::move(batch);
            EXPECT_TRUE(batch.Empty());
            moved.Clear();
            EXPECT_EQ(assets.CollectGarbage(), 1u);
        } else {
            EXPECT_EQ(assets.CollectGarbage(), 2u);
        }
        EXPECT_TRUE(destroyed);
        EXPECT_EQ(assets.GetRevision(weak.Untyped()), 0u);
    }
}

TEST(AssetManagerTest, FlipsDependencyAndOrdersDestructionAndEvents) {
    vector<int> destroyed;
    AssetManager assets;
    using Probe = ScopeGuard<std::function<void()>>;
    const auto probe = [&](int id) { return Probe{std::function<void()>{[&, id] { destroyed.push_back(id); }}}; };
    auto b = assets.Emplace(probe(1));
    auto a = assets.Emplace(probe(2), array{b.Untyped()});
    AssetTransaction tx(assets);
    ASSERT_TRUE(tx.StageReplace(b, probe(3), array{a.Untyped(), a.Untyped()}));
    ASSERT_TRUE(tx.StageReplace(a, probe(4)));
    AssetGarbageBatch batch;
    const auto result = assets.Commit(std::move(tx), batch);
    ASSERT_TRUE(result.Succeeded);
    ASSERT_EQ(result.Events.size(), 2u);
    EXPECT_EQ(result.Events[0].Handle, a.Untyped());
    EXPECT_EQ(result.Events[1].Handle, b.Untyped());
    EXPECT_TRUE(assets.GetDependents(b.Untyped()).empty());
    ASSERT_EQ(assets.GetDependents(a.Untyped()).size(), 1u);
    EXPECT_EQ(assets.GetDependents(a.Untyped())[0], b.Untyped());
    EXPECT_EQ(assets.GetRefCount(b.Untyped()), 2u);
    batch.Clear();
    EXPECT_EQ(destroyed, (vector<int>{2, 1}));
    EXPECT_EQ(assets.GetRefCount(b.Untyped()), 1u);
    a.Reset();
    b.Reset();
    EXPECT_EQ(assets.CollectGarbage(), 2u);
    EXPECT_EQ(destroyed, (vector<int>{2, 1, 3, 4}));
}

TEST(AssetManagerTest, RejectsCyclePoisonForeignAndExpiredTargetsAtomically) {
    AssetManager assets, foreign;
    auto a = assets.Emplace(1);
    auto b = assets.Emplace(2);
    AssetGarbageBatch retired;
    AssetTransaction cycle(assets);
    ASSERT_TRUE(cycle.StageReplace(a, 3, array{b.Untyped()}));
    ASSERT_TRUE(cycle.StageReplace(b, 4, array{a.Untyped()}));
    EXPECT_FALSE(assets.Commit(std::move(cycle), retired).Succeeded);
    AssetTransaction duplicate(assets);
    ASSERT_TRUE(duplicate.StageReplace(a, 3));
    EXPECT_FALSE(duplicate.StageReplace(a, 4));
    EXPECT_FALSE(assets.Commit(std::move(duplicate), retired).Succeeded);
    AssetTransaction wrong(foreign);
    EXPECT_FALSE(wrong.StageReplace(a, 5));
    EXPECT_FALSE(foreign.Commit(std::move(wrong), retired).Succeeded);
    AssetTransaction empty(assets);
    EXPECT_FALSE(foreign.Commit(std::move(empty), retired).Succeeded);
    EXPECT_TRUE(assets.Commit(std::move(empty), retired).Succeeded);
    AssetTransaction stale(assets);
    ASSERT_TRUE(stale.StageReplace(a, 7, array{b.Untyped()}));
    b.Reset();
    ASSERT_EQ(assets.CollectGarbage(), 1u);
    auto reused = assets.Emplace(9);
    EXPECT_FALSE(assets.Commit(std::move(stale), retired).Succeeded);
    EXPECT_EQ(*a.Get(), 1);
    EXPECT_EQ(assets.GetRevision(a.Untyped()), 1u);
    EXPECT_TRUE(assets.GetDependents(a.Untyped()).empty());
    EXPECT_EQ(assets.GetRefCount(a.Untyped()), 1u);
    EXPECT_TRUE(retired.Empty());
}

TEST(AssetManagerTest, BuildersSeeOnlyDeclaredFinalDependencies) {
    AssetManager assets;
    auto leaf = assets.Emplace(2);
    auto mid = assets.Emplace(0);
    auto parent = assets.Emplace(0);
    AssetTransaction tx(assets);
    ASSERT_TRUE(tx.StageBuild(parent, array{mid.Untyped()}, [&](const AssetTransaction& view) {
        EXPECT_FALSE(view.Get(leaf.GetHandle()));
        return std::optional{*view.Get(mid.GetHandle()) + 1};
    }));
    EXPECT_FALSE(tx.Get(parent.GetHandle()));
    ASSERT_TRUE(tx.StageBuild(mid, array{leaf.Untyped()}, [&](const AssetTransaction& view) {
        return std::optional{*view.Get(leaf.GetHandle()) + 1};
    }));
    ASSERT_TRUE(tx.StageReplace(leaf, 10));
    AssetGarbageBatch batch;
    ASSERT_TRUE(assets.Commit(std::move(tx), batch).Succeeded);
    EXPECT_EQ(*parent.Get(), 12);
    EXPECT_FALSE(assets.HasPendingGarbage());
}

TEST(AssetManagerTest, PreflightRejectsReentrantMutation) {
    AssetManager assets;
    auto ref = assets.Emplace(1);
    auto child = assets.Emplace(3);
    AssetGarbageBatch batch;
    AssetTransaction tx(assets);
    int operation = 0;
    AssetTransaction unrelated(assets);
    ASSERT_TRUE(tx.StageBuild(ref, {}, [&](const AssetTransaction&) {
        if (operation == 0) assets.Emplace(1);
        if (operation == 1) child.Reset();
        if (operation == 2) tx.StageReplace(child, 5);
        if (operation == 3) {
            auto moved = std::move(tx);
        }
        if (operation == 4) child.Get();
        if (operation == 5) unrelated.Get(child.GetHandle());
        return std::optional{2};
    }));
    for (; operation < 6; ++operation) {
        EXPECT_DEATH(assets.Commit(std::move(tx), batch), "");
    }
    struct Classified {
        AssetManager* Manager;
        AssetReloadDecision ClassifyReload(const Classified&) const {
            Manager->CollectGarbage();
            return {};
        }
    };
    auto classified = assets.Emplace(Classified{&assets});
    AssetTransaction check(assets);
    ASSERT_TRUE(check.StageReplace(classified, Classified{&assets}));
    EXPECT_DEATH(assets.Commit(std::move(check), batch), "");
}

TEST(AssetManagerTest, OutstandingRetiredBatchRequiresItsManager) {
    EXPECT_DEATH({
        AssetGarbageBatch batch;
        AssetManager assets;
        assets.Emplace(1);
        assets.CollectGarbage(batch); }, "");
    EXPECT_DEATH({
        AssetRef<int> external;
        AssetManager assets;
        external = assets.Emplace(1); }, "");
}
}  // namespace
}  // namespace radray
