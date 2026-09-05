#include <type_traits>

#include <gtest/gtest.h>

#include <radray/runtime/service_registry.h>

namespace radray {
namespace service_registry_test {

struct Plain {
    int Value{7};
};

struct Primary {
    virtual ~Primary() noexcept = default;
    virtual int Value() const noexcept { return 11; }
};

struct Secondary {
    virtual ~Secondary() noexcept = default;
    virtual int OtherValue() const noexcept { return 22; }
};

struct Multi : Primary, Secondary {
    int Starts{0};
    int Stops{0};
    bool Fail{false};
};

struct OtherProvider : Primary {};
struct BadExport {};
struct PrivateExport : private Primary {};
struct DuplicateExport : Primary {};

struct NeedsPlain {
    Nullable<const Plain*> Dependency;
};

struct OptionalConsumer {
    Nullable<Secondary*> Dependency;
    int Wires{0};
    int Starts{0};
    int Stops{0};
    int ObservedStarts{0};
};

struct OptionalLinkConsumer {
    Nullable<Secondary*> Dependency;
};

struct CycleB;
struct CycleA {
    Nullable<CycleB*> Dependency;
};
struct CycleB {
    Nullable<CycleA*> Dependency;
};

struct HardA {};
struct HardB {};
struct BadInject {};
struct CopyingInject {};
struct FallibleInject {};
struct BadInitialize {};
struct MissingShutdown {};
struct FallibleShutdown {};
struct LooksLikeService {
    int Starts{0};
    void OnInitialize() noexcept { ++Starts; }
};

struct Trace {
    array<int, 64> Events{};
    size_t Count{0};
    void Record(int value) noexcept { Events[Count++] = value; }
    vector<int> Values() const { return {Events.begin(), Events.begin() + Count}; }
};

template <int Id>
struct Node {
    explicit Node(Trace& trace, bool fail = false) noexcept : Events(trace), Fail(fail) {}
    Trace& Events;
    bool Fail{false};
    bool Wired{false};
    bool Started{false};
};

template <int Id>
struct NodeLifecycle {
    static constexpr array<std::string_view, 4> kNames{"", "Node1", "Node2", "Node3"};
    static constexpr std::string_view Name{kNames[Id]};
    static ServiceStatus Initialize(Node<Id>& self) {
        self.Events.Record(Id);
        self.Started = true;
        if (self.Fail) return ServiceStatus::Failure("probe failed after allocating resources");
        return {};
    }
    static void Shutdown(Node<Id>& self) noexcept {
        self.Events.Record(-Id);
        self.Started = false;
    }
    static void Unwire(Node<Id>& self) noexcept {
        self.Events.Record(200 + Id);
        self.Wired = false;
    }
};

struct Reentry {
    Nullable<ServiceRegistry<Reentry>*> Registry;
    ServiceError NestedInitialize{ServiceError::None};
    bool NestedShutdown{true};
    bool NestedShutdownDuringStop{true};
};

}  // namespace service_registry_test

template <>
struct ServiceTraits<service_registry_test::Multi> {
    using T = service_registry_test::Multi;
    static constexpr std::string_view Name{"Multi"};
    using Provides = TypeList<service_registry_test::Primary, service_registry_test::Secondary>;
    static ServiceStatus Initialize(T& self) {
        ++self.Starts;
        if (self.Fail) return ServiceStatus::Failure("optional provider failed");
        return {};
    }
    static void Shutdown(T& self) noexcept { ++self.Stops; }
};

template <>
struct ServiceTraits<service_registry_test::OtherProvider> {
    using Provides = TypeList<service_registry_test::Primary>;
};

template <>
struct ServiceTraits<service_registry_test::BadExport> {
    using Provides = TypeList<service_registry_test::Primary>;
};

template <>
struct ServiceTraits<service_registry_test::PrivateExport> {
    using Provides = TypeList<service_registry_test::Primary>;
};

template <>
struct ServiceTraits<service_registry_test::DuplicateExport> {
    using Provides = TypeList<service_registry_test::Primary, service_registry_test::Primary>;
};

template <>
struct ServiceTraits<service_registry_test::NeedsPlain> {
    using T = service_registry_test::NeedsPlain;
    using Dependencies = TypeList<Required<const service_registry_test::Plain>>;
    static void Inject(T& self, const service_registry_test::Plain& dependency) noexcept { self.Dependency = &dependency; }
    static void Unwire(T& self) noexcept { self.Dependency = nullptr; }
};

template <>
struct ServiceTraits<service_registry_test::OptionalConsumer> {
    using T = service_registry_test::OptionalConsumer;
    using Dependencies = TypeList<Optional<service_registry_test::Secondary>>;
    static void Inject(T& self, Nullable<service_registry_test::Secondary*> dependency) noexcept {
        self.Dependency = dependency;
        ++self.Wires;
    }
    static ServiceStatus Initialize(T& self) {
        ++self.Starts;
        if (self.Dependency) self.ObservedStarts = static_cast<service_registry_test::Multi*>(self.Dependency.Get())->Starts;
        return {};
    }
    static void Shutdown(T& self) noexcept { ++self.Stops; }
    static void Unwire(T& self) noexcept { self.Dependency = nullptr; }
};

template <>
struct ServiceTraits<service_registry_test::OptionalLinkConsumer> {
    using T = service_registry_test::OptionalLinkConsumer;
    using Dependencies = TypeList<OptionalLink<service_registry_test::Secondary>>;
    static void Inject(T& self, Nullable<service_registry_test::Secondary*> dependency) noexcept { self.Dependency = dependency; }
};

template <>
struct ServiceTraits<service_registry_test::CycleA> {
    using T = service_registry_test::CycleA;
    using Dependencies = TypeList<Link<service_registry_test::CycleB>>;
    static void Inject(T& self, service_registry_test::CycleB& dependency) noexcept { self.Dependency = &dependency; }
    static void Unwire(T& self) noexcept { self.Dependency = nullptr; }
};

template <>
struct ServiceTraits<service_registry_test::CycleB> {
    using T = service_registry_test::CycleB;
    using Dependencies = TypeList<Link<service_registry_test::CycleA>>;
    static void Inject(T& self, service_registry_test::CycleA& dependency) noexcept { self.Dependency = &dependency; }
    static void Unwire(T& self) noexcept { self.Dependency = nullptr; }
};

template <>
struct ServiceTraits<service_registry_test::HardA> {
    using Dependencies = TypeList<Required<service_registry_test::HardB>>;
    static void Inject(service_registry_test::HardA&, service_registry_test::HardB&) noexcept {}
};

template <>
struct ServiceTraits<service_registry_test::HardB> {
    using Dependencies = TypeList<Optional<service_registry_test::HardA>>;
    static void Inject(service_registry_test::HardB&, Nullable<service_registry_test::HardA*>) noexcept {}
};

template <>
struct ServiceTraits<service_registry_test::BadInject> {
    using Dependencies = TypeList<Required<service_registry_test::Plain>>;
    static void Inject(service_registry_test::BadInject&, service_registry_test::Plain*) noexcept {}
};

template <>
struct ServiceTraits<service_registry_test::CopyingInject> {
    using Dependencies = TypeList<Required<service_registry_test::Plain>>;
    static void Inject(service_registry_test::CopyingInject&, service_registry_test::Plain) noexcept {}
};

template <>
struct ServiceTraits<service_registry_test::FallibleInject> {
    static void Inject(service_registry_test::FallibleInject&) {}
};

template <>
struct ServiceTraits<service_registry_test::BadInitialize> {
    static bool Initialize(service_registry_test::BadInitialize&) { return true; }
    static void Shutdown(service_registry_test::BadInitialize&) noexcept {}
};

template <>
struct ServiceTraits<service_registry_test::MissingShutdown> {
    static ServiceStatus Initialize(service_registry_test::MissingShutdown&) { return {}; }
};

template <>
struct ServiceTraits<service_registry_test::FallibleShutdown> {
    static void Shutdown(service_registry_test::FallibleShutdown&) {}
};

template <>
struct ServiceTraits<service_registry_test::Node<1>> : service_registry_test::NodeLifecycle<1> {
    static void Inject(service_registry_test::Node<1>& self) noexcept {
        self.Wired = true;
        self.Events.Record(101);
    }
};

template <>
struct ServiceTraits<service_registry_test::Node<2>> : service_registry_test::NodeLifecycle<2> {
    using Dependencies = TypeList<Required<service_registry_test::Node<1>>>;
    static void Inject(service_registry_test::Node<2>& self, service_registry_test::Node<1>&) noexcept {
        self.Wired = true;
        self.Events.Record(102);
    }
};

template <>
struct ServiceTraits<service_registry_test::Node<3>> : service_registry_test::NodeLifecycle<3> {
    using Dependencies = TypeList<Required<service_registry_test::Node<2>>>;
    static void Inject(service_registry_test::Node<3>& self, service_registry_test::Node<2>&) noexcept {
        self.Wired = true;
        self.Events.Record(103);
    }
};

template <>
struct ServiceTraits<service_registry_test::Reentry> {
    using T = service_registry_test::Reentry;
    static ServiceStatus Initialize(T& self);
    static void Shutdown(T& self) noexcept;
};

ServiceStatus ServiceTraits<service_registry_test::Reentry>::Initialize(T& self) {
    self.NestedInitialize = self.Registry->Initialize().Code;
    self.NestedShutdown = self.Registry->Shutdown();
    return {};
}

void ServiceTraits<service_registry_test::Reentry>::Shutdown(T& self) noexcept {
    self.NestedShutdownDuringStop = self.Registry->Shutdown();
}

namespace {

using namespace service_registry_test;

static_assert(kValidServiceRegistry<>);
static_assert(kValidServiceRegistry<NeedsPlain, Plain>);
static_assert(kValidServiceRegistry<CycleA, CycleB>);
static_assert(kValidServiceRegistry<OptionalConsumer>);
static_assert(kValidServiceRegistry<OptionalConsumer, OptionalService<Multi>>);
static_assert(!kValidServiceRegistry<NeedsPlain>);
static_assert(!kValidServiceRegistry<NeedsPlain, OptionalService<Plain>>);
static_assert(!kValidServiceRegistry<CycleA, OptionalService<CycleB>>);
static_assert(!kValidServiceRegistry<Plain, Plain>);
static_assert(!kValidServiceRegistry<Plain, OptionalService<Plain>>);
static_assert(!kValidServiceRegistry<Multi, OtherProvider>);
static_assert(!kValidServiceRegistry<BadExport>);
static_assert(!kValidServiceRegistry<PrivateExport>);
static_assert(!kValidServiceRegistry<DuplicateExport>);
static_assert(!kValidServiceRegistry<HardA, HardB>);
static_assert(!kValidServiceRegistry<HardA, HardB, Plain>);
static_assert(!kValidServiceRegistry<BadInject, Plain>);
static_assert(!kValidServiceRegistry<CopyingInject, Plain>);
static_assert(!kValidServiceRegistry<FallibleInject>);
static_assert(!kValidServiceRegistry<BadInitialize>);
static_assert(!kValidServiceRegistry<MissingShutdown>);
static_assert(!kValidServiceRegistry<FallibleShutdown>);
static_assert(!std::is_constructible_v<ServiceRegistry<Plain>, Plain*>);
static_assert(!std::is_constructible_v<ServiceRegistry<Plain>, std::nullptr_t>);
static_assert(std::same_as<decltype(std::declval<const ServiceRegistry<Plain>&>().Resolve<const Plain>()), Nullable<const Plain*>>);

using Chain = ServiceRegistry<Node<3>, Node<1>, Node<2>>;
static_assert(Chain::kInitializationOrder == array<size_t, 3>{1, 2, 0});
static_assert(ServiceRegistry<OptionalConsumer, OptionalService<Multi>>::kInitializationOrder == array<size_t, 2>{1, 0});
static_assert(ServiceRegistry<OptionalLinkConsumer, OptionalService<Multi>>::kInitializationOrder == array<size_t, 2>{0, 1});
static_assert(ServiceRegistry<Multi, Plain>::kInitializationOrder == array<size_t, 2>{0, 1});

TEST(ServiceRegistryTest, ResolvesStaticKeysAndConstViews) {
    Plain plain;
    ServiceRegistry registry{plain};
    EXPECT_EQ(&registry.Get<Plain>(), &plain);
    EXPECT_EQ(&registry.Get<const Plain>(), &plain);
    EXPECT_EQ(registry.Resolve<Plain>().Get(), &plain);
    EXPECT_EQ(registry.Resolve<const Plain>().Get(), &plain);
    EXPECT_FALSE(registry.Resolve<Primary>());
}

TEST(ServiceRegistryTest, InterfaceAliasesAdjustMultipleInheritanceAndShareLifecycle) {
    Multi multi;
    ServiceRegistry registry{multi};
    EXPECT_EQ(&registry.Get<Primary>(), static_cast<Primary*>(&multi));
    EXPECT_EQ(&registry.Get<Secondary>(), static_cast<Secondary*>(&multi));
    EXPECT_NE(static_cast<void*>(&registry.Get<Secondary>()), static_cast<void*>(&multi));
    EXPECT_EQ(registry.Get<Secondary>().OtherValue(), 22);
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(multi.Starts, 1);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(multi.Stops, 1);
}

TEST(ServiceRegistryTest, RequiredDependencyUsesDeclaredConstReference) {
    Plain plain;
    NeedsPlain consumer;
    ServiceRegistry registry{consumer, plain};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(consumer.Dependency.Get(), &plain);
    EXPECT_EQ(consumer.Dependency->Value, 7);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_FALSE(consumer.Dependency);
}

TEST(ServiceRegistryTest, AllowsReferenceCyclesAndUnwiresBothSides) {
    CycleA a;
    CycleB b;
    ServiceRegistry registry{a, b};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(a.Dependency.Get(), &b);
    EXPECT_EQ(b.Dependency.Get(), &a);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_FALSE(a.Dependency);
    EXPECT_FALSE(b.Dependency);
}

TEST(ServiceRegistryTest, OptionalDependencyCanBeAbsentFromTheStaticSet) {
    Multi stale;
    OptionalConsumer consumer;
    consumer.Dependency = &stale;
    ServiceRegistry registry{consumer};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_FALSE(consumer.Dependency);
    EXPECT_EQ(consumer.Wires, 1);
    EXPECT_EQ(consumer.Starts, 1);
    EXPECT_TRUE(registry.Shutdown());
}

TEST(ServiceRegistryTest, EmptyOptionalSlotSkipsProviderHooks) {
    OptionalConsumer consumer;
    ServiceRegistry<OptionalConsumer, OptionalService<Multi>> registry{consumer, nullptr};
    EXPECT_FALSE(registry.Resolve<Multi>());
    EXPECT_FALSE(registry.Resolve<Secondary>());
    ASSERT_TRUE(registry.Initialize());
    EXPECT_FALSE(consumer.Dependency);
    EXPECT_EQ(consumer.Starts, 1);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(consumer.Stops, 1);
}

TEST(ServiceRegistryTest, PresentOptionalInterfaceInitializesBeforeConsumer) {
    Multi multi;
    OptionalConsumer consumer;
    ServiceRegistry<OptionalConsumer, OptionalService<Multi>> registry{consumer, &multi};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(consumer.Dependency.Get(), static_cast<Secondary*>(&multi));
    EXPECT_EQ(consumer.ObservedStarts, 1);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(multi.Stops, 1);
    EXPECT_FALSE(consumer.Dependency);
}

TEST(ServiceRegistryTest, OptionalPresenceIsCapturedAtBinding) {
    Multi multi;
    Nullable<Multi*> slot{&multi};
    ServiceRegistry<OptionalService<Multi>> registry{slot};
    slot = nullptr;
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(registry.Resolve<Multi>().Get(), &multi);
    EXPECT_EQ(multi.Starts, 1);
    EXPECT_TRUE(registry.Shutdown());
}

TEST(ServiceRegistryTest, PresentOptionalProviderFailureIsNotTreatedAsAbsence) {
    Multi multi;
    multi.Fail = true;
    OptionalConsumer consumer;
    ServiceRegistry<OptionalConsumer, OptionalService<Multi>> registry{consumer, &multi};
    const auto result = registry.Initialize();
    EXPECT_FALSE(result);
    EXPECT_EQ(result.Service, "Multi");
    EXPECT_EQ(multi.Starts, 1);
    EXPECT_EQ(multi.Stops, 1);
    EXPECT_EQ(consumer.Starts, 0);
    EXPECT_EQ(consumer.Stops, 0);
    EXPECT_EQ(consumer.Wires, 1);
    EXPECT_FALSE(consumer.Dependency);
}

TEST(ServiceRegistryTest, OptionalLinkDoesNotImposeLifecycleOrder) {
    Multi multi;
    OptionalLinkConsumer consumer;
    ServiceRegistry<OptionalLinkConsumer, OptionalService<Multi>> registry{consumer, &multi};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(consumer.Dependency.Get(), static_cast<Secondary*>(&multi));
    EXPECT_TRUE(registry.Shutdown());
}

TEST(ServiceRegistryTest, ExecutesCompiledOrderAndReverseCleanup) {
    Trace trace;
    Node<1> first{trace};
    Node<2> second{trace};
    Node<3> third{trace};
    Chain registry{third, first, second};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(trace.Values(), (vector<int>{103, 101, 102, 1, 2, 3}));
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(trace.Values(), (vector<int>{103, 101, 102, 1, 2, 3, -3, -2, -1, 202, 201, 203}));
    EXPECT_FALSE(first.Wired);
    EXPECT_FALSE(second.Wired);
    EXPECT_FALSE(third.Wired);
}

TEST(ServiceRegistryTest, FailureCleansCurrentPartialServiceAndEarlierServices) {
    Trace trace;
    Node<1> first{trace};
    Node<2> second{trace, true};
    Node<3> third{trace};
    Chain registry{third, first, second};
    const auto result = registry.Initialize();
    EXPECT_FALSE(result);
    EXPECT_EQ(result.Code, ServiceError::InitializationFailed);
    EXPECT_EQ(result.Service, "Node2");
    EXPECT_EQ(result.Message, "probe failed after allocating resources");
    EXPECT_EQ(trace.Values(), (vector<int>{103, 101, 102, 1, 2, -2, -1, 202, 201, 203}));
    EXPECT_FALSE(first.Started);
    EXPECT_FALSE(second.Started);
    EXPECT_FALSE(third.Started);
    EXPECT_FALSE(third.Wired);
    EXPECT_EQ(registry.GetState(), ServiceRegistryState::Failed);
    const auto count = trace.Count;
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(registry.Initialize().Code, ServiceError::InvalidState);
    EXPECT_EQ(trace.Count, count);
}

TEST(ServiceRegistryTest, FailureAtFirstServiceDoesNotStopUnattemptedServices) {
    Trace trace;
    Node<1> first{trace, true};
    Node<2> second{trace};
    Node<3> third{trace};
    Chain registry{third, first, second};
    EXPECT_FALSE(registry.Initialize());
    EXPECT_EQ(trace.Values(), (vector<int>{103, 101, 102, 1, -1, 202, 201, 203}));
}

TEST(ServiceRegistryTest, FailureAtLastServiceCleansTheFullAttemptedPrefix) {
    Trace trace;
    Node<1> first{trace};
    Node<2> second{trace};
    Node<3> third{trace, true};
    Chain registry{third, first, second};
    EXPECT_FALSE(registry.Initialize());
    EXPECT_EQ(trace.Values(), (vector<int>{103, 101, 102, 1, 2, 3, -3, -2, -1, 202, 201, 203}));
}

TEST(ServiceRegistryTest, RollbackSkipsAbsentSlotsWithinTheAttemptedPrefix) {
    Trace trace;
    Node<1> first{trace};
    Node<2> second{trace, true};
    ServiceRegistry<OptionalService<Multi>, Node<1>, Node<2>> registry{nullptr, first, second};
    EXPECT_FALSE(registry.Initialize());
    EXPECT_EQ(trace.Values(), (vector<int>{101, 102, 1, 2, -2, -1, 202, 201}));
}

TEST(ServiceRegistryTest, RepeatedLifecycleCallsDoNotRepeatSideEffects) {
    Multi multi;
    ServiceRegistry registry{multi};
    EXPECT_EQ(registry.GetState(), ServiceRegistryState::Ready);
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(registry.GetState(), ServiceRegistryState::Running);
    EXPECT_EQ(registry.Initialize().Code, ServiceError::InvalidState);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(registry.GetState(), ServiceRegistryState::Stopped);
    EXPECT_EQ(registry.Initialize().Code, ServiceError::InvalidState);
    EXPECT_EQ(multi.Starts, 1);
    EXPECT_EQ(multi.Stops, 1);
}

TEST(ServiceRegistryTest, ShutdownBeforeInitializationDoesNotCallHooks) {
    Multi multi;
    ServiceRegistry registry{multi};
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_EQ(multi.Starts, 0);
    EXPECT_EQ(multi.Stops, 0);
    EXPECT_EQ(registry.Initialize().Code, ServiceError::InvalidState);
}

TEST(ServiceRegistryTest, LifecycleReentryIsRejected) {
    Reentry service;
    ServiceRegistry registry{service};
    service.Registry = &registry;
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(service.NestedInitialize, ServiceError::InvalidState);
    EXPECT_FALSE(service.NestedShutdown);
    EXPECT_TRUE(registry.Shutdown());
    EXPECT_FALSE(service.NestedShutdownDuringStop);
}

TEST(ServiceRegistryTest, EmptyCompositionHasValidLifecycle) {
    ServiceRegistry<> registry;
    ASSERT_TRUE(registry.Initialize());
    EXPECT_TRUE(registry.Shutdown());
}

TEST(ServiceRegistryTest, ServiceMemberNamesDoNotCreateImplicitHooks) {
    LooksLikeService service;
    ServiceRegistry registry{service};
    ASSERT_TRUE(registry.Initialize());
    EXPECT_EQ(service.Starts, 0);
    EXPECT_TRUE(registry.Shutdown());
}

TEST(ServiceRegistryTest, RegistryDestructionLeavesBorrowedServicesToTheirOwner) {
    Multi multi;
    {
        ServiceRegistry registry{multi};
        ASSERT_TRUE(registry.Initialize());
    }
    EXPECT_EQ(multi.Starts, 1);
    EXPECT_EQ(multi.Stops, 0);
}

}  // namespace
}  // namespace radray
