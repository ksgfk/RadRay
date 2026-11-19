#include "gpu_test_fixture.h"

namespace radray::gpu {
namespace {

class SamplerTableTest : public test::GpuTest {
protected:
    void SetUp() override {
        device = OpenDevice(GpuBackend::D3D12).Get();
        if (!device) return;
        const BindingDescriptor binding{.BindingId = 0, .Kind = BindingKind::Sampler, .Count = 1, .Stages = ShaderStage::Pixel,
                                        .D3D12 = D3D12BindingDescriptor{BindingRegisterClass::Sampler, 0, 0}};
        layout = device->CreateBindingGroupLayout({std::span{&binding, 1}}).Release();
        ASSERT_TRUE(layout);
    }
    void TearDown() override {
        layout.reset();
        GpuTest::TearDown();
    }

    Device* device{};
    unique_ptr<BindingGroupLayout> layout;
    const SamplerDescriptor descriptor{.AddressS = AddressMode::ClampToEdge, .AddressT = AddressMode::ClampToEdge, .AddressR = AddressMode::ClampToEdge,
                                       .MinFilter = FilterMode::Nearest, .MagFilter = FilterMode::Nearest, .MipmapFilter = FilterMode::Nearest, .AnisotropyClamp = 1};
};

TEST_F(SamplerTableTest, IdenticalTablesShareShaderVisibleSlots) {
    auto sampler = device->CreateSampler(descriptor).Unwrap();
    vector<unique_ptr<ShaderParameterSet>> sets;
    for (uint32_t i = 0; i < 2050; ++i) {
        auto set = device->CreateShaderParameterSet({layout.get()}).Release();
        ASSERT_TRUE(set) << i;
        ASSERT_TRUE(set->Set(0, 0, sampler.get()));
        ASSERT_TRUE(set->FlushWrites()) << i;
        sets.push_back(std::move(set));
    }
}

TEST_F(SamplerTableTest, ExhaustedUniqueTablesRecoverAfterUnbindingOneOwner) {
    vector<unique_ptr<Sampler>> samplers;
    vector<unique_ptr<ShaderParameterSet>> sets;
    for (uint32_t i = 0; i <= 2048; ++i) {
        samplers.push_back(device->CreateSampler(descriptor).Unwrap());
        auto set = device->CreateShaderParameterSet({layout.get()}).Release();
        ASSERT_TRUE(set) << i;
        ASSERT_TRUE(set->Set(0, 0, samplers.back().get()));
        EXPECT_EQ(set->FlushWrites(), i < 2048) << i;
        sets.push_back(std::move(set));
    }
    sets.front()->ResetBindingsForReuse();
    EXPECT_TRUE(sets.back()->FlushWrites());
}

TEST_F(SamplerTableTest, ResourceTablesCanAllocateOneMillionShaderVisibleDescriptors) {
    const BindingDescriptor binding{.BindingId = 0, .Kind = BindingKind::Texture, .Count = 10000, .Stages = ShaderStage::Pixel,
                                    .D3D12 = D3D12BindingDescriptor{BindingRegisterClass::SRV, 0, 0}};
    auto resources = device->CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    vector<unique_ptr<ShaderParameterSet>> sets;
    for (uint32_t i = 0; i < 100; ++i) {
        auto set = device->CreateShaderParameterSet({resources.get()});
        ASSERT_TRUE(set) << i;
        sets.push_back(set.Release());
    }
    EXPECT_FALSE(device->CreateShaderParameterSet({resources.get()}));
    sets.pop_back();
    EXPECT_TRUE(device->CreateShaderParameterSet({resources.get()}));
}

}  // namespace
}  // namespace radray::gpu
