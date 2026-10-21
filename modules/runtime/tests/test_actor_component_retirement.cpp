#include <gtest/gtest.h>

#include <optional>

#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/types.h>

namespace radray {
namespace {

struct RetirementEvents {
    vector<uint32_t> Unregistered;
    vector<uint32_t> Destroyed;
};

class RetirementProbe final : public SceneComponent {
public:
    RetirementProbe(RetirementEvents& events, uint32_t tag) noexcept : _events(events), _tag(tag) {}
    ~RetirementProbe() noexcept override { _events.Destroyed.push_back(_tag); }

    void AppendSiblingOnUnregister() noexcept { _appendSibling = true; }
    void RemoveSiblingOnUnregister(ComponentId id) noexcept { _removeSibling = id; }
    void ExpectRootClearedOnUnregister() noexcept { _expectRootCleared = true; }

    void OnUnregister() override {
        auto owner = GetOwner();
        ASSERT_TRUE(owner);
        EXPECT_TRUE(GetWorld());
        EXPECT_FALSE(IsRegistered());
        EXPECT_FALSE(owner->FindLive(GetId()));
        if (_expectRootCleared) EXPECT_FALSE(owner->GetRootComponent());
        _events.Unregistered.push_back(_tag);
        if (_appendSibling && owner->IsLive()) owner->AddComponent<RetirementProbe>(_events, _tag + 1000);
        if (_removeSibling && owner->IsLive()) {
            auto sibling = owner->FindLive(*_removeSibling);
            ASSERT_TRUE(sibling);
            EXPECT_EQ(owner->RemoveComponent(sibling.Get()), LifecycleRequestResult::Accepted);
        }
    }

private:
    RetirementEvents& _events;
    uint32_t _tag;
    bool _appendSibling{false};
    bool _expectRootCleared{false};
    std::optional<ComponentId> _removeSibling;
};

class ActorComponentRetirement : public ::testing::Test {
protected:
    void TearDown() override { _world.ShutdownWorld(); }

    RetirementEvents _events;
    World _world;
};

TEST_F(ActorComponentRetirement, StableCompactionPreservesSurvivorsAndNotificationOrder) {
    auto* actor = _world.SpawnActor(make_unique<Actor>());
    auto* first = actor->AddComponent<RetirementProbe>(_events, 1);
    auto* second = actor->AddComponent<RetirementProbe>(_events, 2);
    auto* third = actor->AddComponent<RetirementProbe>(_events, 3);
    auto* fourth = actor->AddComponent<RetirementProbe>(_events, 4);
    ASSERT_EQ(actor->RemoveComponent(fourth), LifecycleRequestResult::Accepted);
    ASSERT_EQ(actor->RemoveComponent(second), LifecycleRequestResult::Accepted);
    EXPECT_TRUE(_events.Unregistered.empty());
    EXPECT_TRUE(_events.Destroyed.empty());
    _world.FinalizeWorldGT();
    const auto components = actor->GetOwnedComponents();
    ASSERT_EQ(components.size(), 2u);
    EXPECT_EQ(components[0].get(), first);
    EXPECT_EQ(components[1].get(), third);
    EXPECT_EQ(_events.Unregistered, (vector<uint32_t>{4, 2}));
    EXPECT_EQ(_events.Destroyed.size(), 2u);
}

TEST_F(ActorComponentRetirement, RootAndIdentityAreRemovedBeforeUserCallback) {
    auto* actor = _world.SpawnActor(make_unique<Actor>());
    auto* root = actor->AddComponent<RetirementProbe>(_events, 1);
    root->ExpectRootClearedOnUnregister();
    const auto oldId = root->GetId();
    ASSERT_EQ(actor->RequestSetRootComponent(root), LifecycleRequestResult::Accepted);
    _world.FinalizeWorldGT();
    ASSERT_EQ(actor->GetRootComponent().Get(), root);
    ASSERT_EQ(actor->RemoveComponent(root), LifecycleRequestResult::Accepted);
    EXPECT_EQ(actor->GetRootComponent().Get(), root);
    _world.FinalizeWorldGT();
    EXPECT_FALSE(actor->GetRootComponent());
    EXPECT_FALSE(actor->FindLive(oldId));
    auto* replacement = actor->AddComponent<RetirementProbe>(_events, 2);
    EXPECT_NE(replacement->GetId(), oldId);
    EXPECT_FALSE(actor->FindLive(oldId));
    EXPECT_EQ(actor->FindLive(replacement->GetId()).Get(), replacement);
}

TEST_F(ActorComponentRetirement, EachComponentGroupOnlyMutatesItsOwningActor) {
    auto* first = _world.SpawnActor(make_unique<Actor>());
    auto* second = _world.SpawnActor(make_unique<Actor>());
    auto* removeFirst = first->AddComponent<RetirementProbe>(_events, 1);
    auto* keepFirst = first->AddComponent<RetirementProbe>(_events, 2);
    auto* keepSecond = second->AddComponent<RetirementProbe>(_events, 3);
    auto* removeSecond = second->AddComponent<RetirementProbe>(_events, 4);
    ASSERT_EQ(second->RemoveComponent(removeSecond), LifecycleRequestResult::Accepted);
    ASSERT_EQ(first->RemoveComponent(removeFirst), LifecycleRequestResult::Accepted);
    _world.FinalizeWorldGT();
    ASSERT_EQ(first->GetOwnedComponents().size(), 1u);
    ASSERT_EQ(second->GetOwnedComponents().size(), 1u);
    EXPECT_EQ(first->GetOwnedComponents()[0].get(), keepFirst);
    EXPECT_EQ(second->GetOwnedComponents()[0].get(), keepSecond);
    EXPECT_EQ(_events.Unregistered.size(), 2u);
    EXPECT_EQ(_events.Destroyed.size(), 2u);
}

TEST_F(ActorComponentRetirement, ActorDeletionCoversAnAlreadyQueuedComponent) {
    auto* actor = _world.SpawnActor(make_unique<Actor>());
    actor->AddComponent<RetirementProbe>(_events, 1);
    auto* component = actor->AddComponent<RetirementProbe>(_events, 2);
    const auto id = actor->GetId();
    ASSERT_EQ(actor->RemoveComponent(component), LifecycleRequestResult::Accepted);
    ASSERT_EQ(_world.DestroyActor(actor), LifecycleRequestResult::Accepted);
    _world.FinalizeWorldGT();
    EXPECT_FALSE(_world.FindLive(id));
    EXPECT_EQ(_events.Unregistered, (vector<uint32_t>{2, 1}));
    EXPECT_EQ(_events.Destroyed.size(), 2u);
}

TEST_F(ActorComponentRetirement, CallbackCanAppendAfterOwnerCompaction) {
    auto* actor = _world.SpawnActor(make_unique<Actor>());
    auto* removed = actor->AddComponent<RetirementProbe>(_events, 1);
    auto* survivor = actor->AddComponent<RetirementProbe>(_events, 2);
    removed->AppendSiblingOnUnregister();
    ASSERT_EQ(actor->RemoveComponent(removed), LifecycleRequestResult::Accepted);
    _world.FinalizeWorldGT();
    const auto components = actor->GetOwnedComponents();
    ASSERT_EQ(components.size(), 2u);
    EXPECT_EQ(components[0].get(), survivor);
    EXPECT_TRUE(components[1]->IsRegistered());
    EXPECT_TRUE(components[1]->IsLive());
    EXPECT_EQ(_events.Unregistered, (vector<uint32_t>{1}));
    EXPECT_EQ(_events.Destroyed.size(), 1u);
}

TEST_F(ActorComponentRetirement, CallbackDestructionDoesNotExpandFrozenBatch) {
    auto* actor = _world.SpawnActor(make_unique<Actor>());
    auto* first = actor->AddComponent<RetirementProbe>(_events, 1);
    auto* second = actor->AddComponent<RetirementProbe>(_events, 2);
    first->RemoveSiblingOnUnregister(second->GetId());
    ASSERT_EQ(actor->RemoveComponent(first), LifecycleRequestResult::Accepted);
    _world.FinalizeWorldGT();
    EXPECT_EQ(_events.Unregistered, (vector<uint32_t>{1}));
    EXPECT_EQ(_events.Destroyed.size(), 1u);
    ASSERT_EQ(actor->GetOwnedComponents().size(), 1u);
    EXPECT_EQ(second->GetLifecycle(), ObjectLifecycle::PendingDestroy);
    _world.FinalizeWorldGT();
    EXPECT_TRUE(actor->GetOwnedComponents().empty());
    EXPECT_EQ(_events.Unregistered, (vector<uint32_t>{1, 2}));
    EXPECT_EQ(_events.Destroyed.size(), 2u);
}

}  // namespace
}  // namespace radray
