#include <gtest/gtest.h>
#include <radray/runtime/render_framework/cpu_draw_record.h>

namespace radray {
namespace {
bool ResolveTestContract(const ShaderProgram&, MeshBindingPlan&) { return true; }
bool ResolveOtherContract(const ShaderProgram&, MeshBindingPlan&) { return true; }
bool CompileTestPass(const StaticPassCompileInput&, StaticPassCompileResult&) { return true; }

TEST(MeshBindingContract, MultipleSlotsPerScopeAndGroupHaveIndependentDestinations) {
    MeshBindingPlan plan;
    plan.Slots = {{17, MeshParameterScope::View, MeshParameterKind::CBuffer, 91, 64},
                  {18, MeshParameterScope::View, MeshParameterKind::CBuffer, 92, 32},
                  {42, MeshParameterScope::Material, MeshParameterKind::Texture},
                  {43, MeshParameterScope::Pass, MeshParameterKind::GraphBuffer}};
    plan.Bindings = {{0, 7, 2, 0, 0}, {1, 7, 4, 0, 1}, {2, 3, 9, 0}, {2, 3, 9, 1}, {3, 12, 0, 0}};
    ASSERT_TRUE(plan.Finalize());
    EXPECT_EQ(plan.Groups, (vector<uint32_t>{3, 7, 12}));
    EXPECT_EQ(plan.Bindings.size(), 5u);
    EXPECT_TRUE(plan.Valid);
    const auto frozen = plan;
    plan.Slots[0].Size = 128;
    EXPECT_EQ(frozen.Slots[0].Size, 64u);
}

TEST(MeshBindingContract, RejectsDuplicateDestinationsAndInvalidSlotsWithoutPublishingValidity) {
    MeshBindingPlan plan;
    plan.Slots = {{8, MeshParameterScope::Primitive, MeshParameterKind::CBuffer, 9, 64}};
    plan.Bindings = {{0, 5, 7, 0, 0}};
    ASSERT_TRUE(plan.Finalize());
    plan.Bindings.push_back(plan.Bindings.front());
    EXPECT_FALSE(plan.Finalize());
    EXPECT_FALSE(plan.Valid);
    plan.Bindings.pop_back();
    plan.Bindings[0].Slot = 1;
    EXPECT_FALSE(plan.Finalize());
    plan.Bindings[0].Slot = 0;
    plan.Slots[0].WireLayout = 0;
    EXPECT_FALSE(plan.Finalize());
    plan.Slots[0].WireLayout = 9;
    EXPECT_TRUE(plan.Finalize());
}

TEST(MeshBindingContract, RegisterNumbersAreScopedByResourceClass) {
    MeshBindingPlan plan;
    plan.Slots = {{1, MeshParameterScope::Material, MeshParameterKind::CBuffer, 3, 64},
                  {2, MeshParameterScope::Material, MeshParameterKind::Texture},
                  {3, MeshParameterScope::Material, MeshParameterKind::Sampler}};
    plan.Bindings = {{0, 4, 0, 0, 0}, {1, 4, 0, 0}, {2, 4, 0, 0}};
    ASSERT_TRUE(plan.Finalize());
    plan.Slots.push_back({4, MeshParameterScope::Pass, MeshParameterKind::GraphTexture});
    plan.Bindings.push_back({3, 4, 0, 0});
    EXPECT_FALSE(plan.Finalize());
}

TEST(MeshBindingContract, ContractIdentityConflictsCannotOverwriteRegisteredCompiler) {
    const MeshBindingContract contract{71, 1, 4, ResolveTestContract};
    array<PassPolicy, 2> policies{{{{1}, 1, "CustomSurface", CompileTestPass, contract},
                                   {{2}, 1, "CustomDepth", CompileTestPass, contract}}};
    CpuDrawStore store;
    ASSERT_TRUE(store.SetActivePolicies(1, policies));
    policies[1].BindingContract.Resolve = ResolveOtherContract;
    EXPECT_FALSE(store.SetActivePolicies(2, policies));
    policies[0].BindingContract.Resolve = ResolveOtherContract;
    EXPECT_FALSE(store.SetActivePolicies(2, policies));
    for (auto& policy : policies) {
        ++policy.Revision;
        ++policy.BindingContract.Revision;
    }
    EXPECT_TRUE(store.SetActivePolicies(2, policies));
}
}  // namespace
}  // namespace radray
