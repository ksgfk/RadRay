#include <cstdint>
#include <type_traits>
#include <typeinfo>

#include <gtest/gtest.h>

#include <radray/nullable.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray {
namespace {

class ComponentFacet {
public:
    virtual ~ComponentFacet() noexcept = default;
};

class ComponentPaddingFacet {
public:
    virtual ~ComponentPaddingFacet() noexcept = default;
    uint64_t Padding{0x123456789abcdef0ull};
};

class SharedComponentFacet {
public:
    virtual ~SharedComponentFacet() noexcept = default;
};

class LeftComponentFacet : public virtual SharedComponentFacet {
public:
    ~LeftComponentFacet() noexcept override = default;
};

class RightComponentFacet : public virtual SharedComponentFacet {
public:
    ~RightComponentFacet() noexcept override = default;
};

class UnrelatedComponentFacet {
public:
    virtual ~UnrelatedComponentFacet() noexcept = default;
};

class ComplexComponent final : public SceneComponent,
                               public ComponentPaddingFacet,
                               public ComponentFacet,
                               public LeftComponentFacet,
                               public RightComponentFacet {
};

class LaterFacetComponent final : public ActorComponent, public ComponentFacet {
};

static_assert(std::same_as<
              decltype(std::declval<Actor&>().FindComponent<ComponentFacet>()),
              Nullable<ComponentFacet*>>);
static_assert(std::same_as<
              decltype(std::declval<const Actor&>().FindComponent<ComponentFacet>()),
              Nullable<const ComponentFacet*>>);

TEST(ComponentRttiTest, FindsFirstConvertibleObjectAcrossRealInheritance) {
    Actor actor;
    ComplexComponent* first = actor.AddComponent<ComplexComponent>();
    LaterFacetComponent* second = actor.AddComponent<LaterFacetComponent>();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    Nullable<ComplexComponent*> exact = actor.FindComponent<ComplexComponent>();
    Nullable<ActorComponent*> base = actor.FindComponent<ActorComponent>();
    Nullable<ComponentFacet*> facet = actor.FindComponent<ComponentFacet>();
    Nullable<LeftComponentFacet*> left = actor.FindComponent<LeftComponentFacet>();
    Nullable<RightComponentFacet*> right = actor.FindComponent<RightComponentFacet>();
    Nullable<SharedComponentFacet*> shared = actor.FindComponent<SharedComponentFacet>();

    ASSERT_TRUE(exact);
    ASSERT_TRUE(base);
    ASSERT_TRUE(facet);
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(shared);
    EXPECT_EQ(exact.Get(), first);
    EXPECT_EQ(base.Get(), static_cast<ActorComponent*>(first));
    EXPECT_EQ(facet.Get(), static_cast<ComponentFacet*>(first));
    EXPECT_NE(
        static_cast<const void*>(facet.Get()),
        static_cast<const void*>(base.Get()));
    EXPECT_EQ(dynamic_cast<SharedComponentFacet*>(left.Get()), shared.Get());
    EXPECT_EQ(dynamic_cast<SharedComponentFacet*>(right.Get()), shared.Get());
    ActorComponent* baseObject = base.Get();
    EXPECT_EQ(typeid(*baseObject), typeid(ComplexComponent));
    EXPECT_FALSE(actor.FindComponent<UnrelatedComponentFacet>());

    const Actor& constActor = actor;
    Nullable<const ComponentFacet*> constFacet =
        constActor.FindComponent<ComponentFacet>();
    ASSERT_TRUE(constFacet);
    EXPECT_EQ(constFacet.Get(), static_cast<const ComponentFacet*>(first));
}

TEST(ComponentRttiTest, RemovingSceneDerivedComponentDetachesItFromParent) {
    Actor actor;
    SceneComponent* parent = actor.AddComponent<SceneComponent>();
    ComplexComponent* child = actor.AddComponent<ComplexComponent>();
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);

    child->AttachTo(parent);
    ASSERT_EQ(parent->GetAttachChildren().size(), 1u);
    actor.RemoveComponent(child);

    EXPECT_TRUE(parent->GetAttachChildren().empty());
    EXPECT_FALSE(actor.FindComponent<ComplexComponent>());
}

}  // namespace
}  // namespace radray
