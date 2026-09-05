#include <type_traits>

#include <gtest/gtest.h>

#include <radray/nullable.h>
#include <radray/runtime/service_registry.h>
#include <radray/types.h>

namespace radray {
namespace service_registry_test {

class PlainService {
public:
    int Value{7};
};

class IPrimaryService {
public:
    virtual ~IPrimaryService() noexcept = default;
    virtual int PrimaryValue() const noexcept = 0;
};

class ISecondaryService {
public:
    virtual ~ISecondaryService() noexcept = default;
    virtual int SecondaryValue() const noexcept = 0;
};

class MultiService final : public IPrimaryService, public ISecondaryService {
public:
    int PrimaryValue() const noexcept override { return 11; }
    int SecondaryValue() const noexcept override { return 22; }
    void OnInitialize() { ++InitializeCount; }

    uint32_t InitializeCount{0};
};

class NeedsPlainService {
public:
    void SetPlainService(PlainService* service) noexcept { Plain = service; }

    PlainService* Plain{nullptr};
};

class CycleB;

class CycleA {
public:
    void SetB(CycleB* service) noexcept { B = service; }
    CycleB* B{nullptr};
};

class CycleB {
public:
    void SetA(CycleA* service) noexcept { A = service; }
    CycleA* A{nullptr};
};

class FirstInitialize {
public:
    explicit FirstInitialize(vector<int>* order) noexcept : Order(order) {}
    void OnInitialize() { Order->push_back(1); }
    vector<int>* Order;
};

class SecondInitialize {
public:
    explicit SecondInitialize(vector<int>* order) noexcept : Order(order) {}
    void OnInitialize() { Order->push_back(2); }
    vector<int>* Order;
};

class ThirdInitialize {
public:
    explicit ThirdInitialize(vector<int>* order) noexcept : Order(order) {}
    void OnInitialize() { Order->push_back(3); }
    vector<int>* Order;
};

}  // namespace service_registry_test

template <>
struct ServiceTraits<service_registry_test::NeedsPlainService> {
    static constexpr auto Inject =
        std::tuple{&service_registry_test::NeedsPlainService::SetPlainService};
};

template <>
struct ServiceTraits<service_registry_test::CycleA> {
    static constexpr auto Inject = std::tuple{&service_registry_test::CycleA::SetB};
};

template <>
struct ServiceTraits<service_registry_test::CycleB> {
    static constexpr auto Inject = std::tuple{&service_registry_test::CycleB::SetA};
};

namespace {

using namespace service_registry_test;

static_assert(std::same_as<
              decltype(std::declval<const ServiceRegistry&>().Resolve<PlainService>()),
              Nullable<PlainService*>>);
static_assert(std::same_as<
              decltype(std::declval<const ServiceRegistry&>().Resolve<const PlainService>()),
              Nullable<const PlainService*>>);

TEST(ServiceRegistryTest, ResolvesNonPolymorphicServiceByExplicitStaticKey) {
    PlainService service;
    ServiceRegistry registry;
    registry.Add(&service);

    Nullable<PlainService*> resolved = registry.Resolve<PlainService>();
    Nullable<const PlainService*> constResolved = registry.Resolve<const PlainService>();
    ASSERT_TRUE(resolved);
    ASSERT_TRUE(constResolved);
    EXPECT_EQ(resolved.Get(), &service);
    EXPECT_EQ(constResolved.Get(), &service);
    EXPECT_EQ(resolved->Value, 7);
}

TEST(ServiceRegistryTest, RegistersOnlyNamedInterfacesAndAdjustsMultipleInheritancePointers) {
    MultiService service;
    ServiceRegistry registry;
    registry.Add<IPrimaryService, ISecondaryService>(&service);

    Nullable<MultiService*> concrete = registry.Resolve<MultiService>();
    Nullable<IPrimaryService*> primary = registry.Resolve<IPrimaryService>();
    Nullable<ISecondaryService*> secondary = registry.Resolve<ISecondaryService>();
    ASSERT_TRUE(concrete);
    ASSERT_TRUE(primary);
    ASSERT_TRUE(secondary);
    EXPECT_EQ(concrete.Get(), &service);
    EXPECT_EQ(primary.Get(), static_cast<IPrimaryService*>(&service));
    EXPECT_EQ(secondary.Get(), static_cast<ISecondaryService*>(&service));
    EXPECT_NE(
        static_cast<const void*>(secondary.Get()),
        static_cast<const void*>(&service));
    EXPECT_EQ(primary->PrimaryValue(), 11);
    EXPECT_EQ(secondary->SecondaryValue(), 22);

    registry.Initialize();
    EXPECT_EQ(service.InitializeCount, 1u)
        << "interface bindings must not duplicate lifecycle entries";
}

TEST(ServiceRegistryTest, DoesNotResolveAnInterfaceThatWasNotRegistered) {
    MultiService service;
    ServiceRegistry registry;
    registry.Add(&service);

    EXPECT_NE(registry.Resolve<MultiService>(), nullptr);
    EXPECT_EQ(registry.Resolve<IPrimaryService>(), nullptr);
    EXPECT_EQ(registry.Resolve<ISecondaryService>(), nullptr);
}

TEST(ServiceRegistryTest, WiresCyclesAfterAllServicesAreRegistered) {
    CycleA a;
    CycleB b;
    ServiceRegistry registry;
    registry.Add(&a);
    registry.Add(&b);

    registry.Wire();

    EXPECT_EQ(a.B, &b);
    EXPECT_EQ(b.A, &a);
}

TEST(ServiceRegistryTest, InitializesInServiceRegistrationOrder) {
    vector<int> order;
    FirstInitialize first{&order};
    SecondInitialize second{&order};
    ThirdInitialize third{&order};
    ServiceRegistry registry;
    registry.Add(&second);
    registry.Add(&first);
    registry.Add(&third);

    registry.Initialize();

    EXPECT_EQ(order, (vector<int>{2, 1, 3}));
}

TEST(ServiceRegistryDeathTest, RejectsNullDuplicateAndMissingRequiredDependency) {
    EXPECT_DEATH_IF_SUPPORTED(
        {
            ServiceRegistry registry;
            PlainService* service = nullptr;
            registry.Add(service);
        },
        "");

    EXPECT_DEATH_IF_SUPPORTED(
        {
            ServiceRegistry registry;
            PlainService first;
            PlainService second;
            registry.Add(&first);
            registry.Add(&second);
        },
        "");

    EXPECT_DEATH_IF_SUPPORTED(
        {
            ServiceRegistry registry;
            MultiService first;
            MultiService second;
            registry.Add<IPrimaryService>(&first);
            registry.Add<IPrimaryService>(&second);
        },
        "");

    EXPECT_DEATH_IF_SUPPORTED(
        {
            ServiceRegistry registry;
            NeedsPlainService service;
            registry.Add(&service);
            registry.Wire();
        },
        "");
}

}  // namespace
}  // namespace radray
