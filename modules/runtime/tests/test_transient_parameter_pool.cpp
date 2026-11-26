#include "../../gpu_impl/tests/gpu_test_fixture.h"

#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/transient_parameter_pool.h>

#include "test_fakes.h"

namespace radray {
namespace {

class TransientParameterPoolTest : public gpu::test::BackendTest {};

class EmptyFramePipeline final : public FramePipeline {
public:
    void Extract(FrameSlot&) noexcept override {}
    void Prepare(FrameSlot&) noexcept override {}
    void Record(FrameSlot&) noexcept override {}
    bool Submit(FrameSlot&) noexcept override { return true; }
    void Wait(FrameSlot&) noexcept override {}
};

TEST_P(TransientParameterPoolTest, ReusesEquivalentBindingsAndUnbindsBeforeFrameResourcesDie) {
    auto& device = GetDevice();
    DeviceContext context{shared_ptr<gpu::Device>{&device, [](gpu::Device*) {}}, &gpu::test::DirectQueue(device)};
    const gpu::BindingDescriptor binding{.BindingId = 1, .Kind = gpu::BindingKind::CBuffer, .Count = 1, .Stages = gpu::ShaderStage::Compute,
                                         .D3D12 = gpu::D3D12BindingDescriptor{gpu::BindingRegisterClass::CBV, 0, 0}, .Vulkan = gpu::VulkanBindingDescriptor{0, 0}};
    shared_ptr<gpu::BindingGroupLayout> layout = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    auto scheduler = FrameScheduler::Create(context, make_unique<EmptyFramePipeline>(), 1).Unwrap();
    for (uint32_t frame = 0; frame < 2; ++frame) {
        auto& slot = scheduler->BeginFrame();
        auto& buffer = *slot.Emplace<unique_ptr<gpu::Buffer>>(device.CreateBuffer({.Size = 512, .Memory = gpu::MemoryType::DeviceLocal, .Usage = gpu::BufferUse::CBuffer}).Unwrap());
        TransientParameterBinding input{1, 0, gpu::ShaderBufferBinding{buffer.get(), {0, 256}, 0}};
        auto first = slot.AcquireParameterSet(layout, std::span{&input, 1});
        ASSERT_TRUE(first);
        EXPECT_EQ(first.Get(), slot.AcquireParameterSet(layout, std::span{&input, 1}).Get());
        input.Value = gpu::ShaderBufferBinding{buffer.get(), {256, 256}, 0};
        auto second = slot.AcquireParameterSet(layout, std::span{&input, 1});
        ASSERT_TRUE(second);
        EXPECT_NE(second.Get(), first.Get());
        if (frame == 0) scheduler->RunFrame();
    }
    scheduler->WaitIdle();
}

TEST_P(TransientParameterPoolTest, ColdLayoutsReleaseOwnersAfterPeakLeavesWindow) {
    auto& device = GetDevice();
    TransientParameterSetPool pool{device};
    shared_ptr<gpu::BindingGroupLayout> layout = device.CreateBindingGroupLayout({{}}).Unwrap();
    weak_ptr<gpu::BindingGroupLayout> owner = layout;
    for (size_t i = 0; i < 24; ++i) ASSERT_TRUE(pool.AcquireUninitializedParameterSet(layout));
    layout.reset();
    pool.BeginFrame();
    EXPECT_FALSE(owner.expired());
    for (size_t i = 0; i < 64; ++i) pool.BeginFrame();
    EXPECT_TRUE(owner.expired());
}

TEST_P(TransientParameterPoolTest, DynamicUniformSlicesRespectDeviceAlignmentAndDeduplicateUnorderedWrites) {
    auto& device = GetDevice();
    DynamicBufferArena arena{device};
    const array<byte, 12> payload{byte{13}, byte{14}};
    auto first = AllocateDynamicUniform(arena, payload);
    auto second = AllocateDynamicUniform(arena, payload);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(second->Offset % std::max<uint64_t>(1, device.GetCapabilities().Limits.CBufferOffsetAlignment), 0u);
    EXPECT_GE(second->Offset, first->Offset + first->ReservedSize);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), second->Data.Get()));
    const gpu::BindingDescriptor descriptor{.BindingId = 2, .Kind = gpu::BindingKind::CBuffer, .Count = 2, .Stages = gpu::ShaderStage::Compute,
                                            .D3D12 = gpu::D3D12BindingDescriptor{gpu::BindingRegisterClass::CBV, 0, 0}, .Vulkan = gpu::VulkanBindingDescriptor{0, 0}};
    shared_ptr<gpu::BindingGroupLayout> layout = device.CreateBindingGroupLayout({std::span{&descriptor, 1}}).Unwrap();
    array bindings{TransientParameterBinding{2, 0, gpu::ShaderBufferBinding{first->Buffer.Get(), {first->Offset, first->Size}}},
                   TransientParameterBinding{2, 1, gpu::ShaderBufferBinding{second->Buffer.Get(), {second->Offset, second->Size}}}};
    TransientParameterSetPool pool{device};
    const auto set = pool.AcquireParameterSet(layout, bindings);
    ASSERT_TRUE(set);
    std::swap(bindings[0], bindings[1]);
    EXPECT_EQ(pool.AcquireParameterSet(layout, bindings).Get(), set.Get());
    bindings[0].ArrayElement = bindings[1].ArrayElement;
    EXPECT_FALSE(pool.AcquireParameterSet(layout, bindings));
}

INSTANTIATE_TEST_SUITE_P(Backends, TransientParameterPoolTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

class FakeBindingGroupLayout final : public gpu::BindingGroupLayout {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {}; }
};

struct ParameterSetProbe {
    uint32_t CreatedSets{0};
    uint32_t SetCalls{0};
    uint32_t FlushCalls{0};
    uint32_t ResetCalls{0};
    uint32_t FailSetCall{0};
    bool FailNextFlush{false};
};

class ProbedParameterSet final : public gpu::ShaderParameterSet {
public:
    explicit ProbedParameterSet(ParameterSetProbe& probe) noexcept : _probe(probe) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(uint32_t id, uint32_t element, gpu::ShaderParameterValue value) noexcept override {
        ++_probe.SetCalls;
        if (_probe.SetCalls == _probe.FailSetCall) return false;
        Bindings.push_back({id, element, value});
        return true;
    }
    bool FlushWrites() noexcept override {
        ++_probe.FlushCalls;
        if (_probe.FailNextFlush) {
            _probe.FailNextFlush = false;
            return false;
        }
        return true;
    }
    void ResetBindingsForReuse() noexcept override {
        ++_probe.ResetCalls;
        Bindings.clear();
    }
    vector<TransientParameterBinding> Bindings;

private:
    ParameterSetProbe& _probe;
};

class ParameterPoolFakeDevice final : public test::FakeDevice {
public:
    explicit ParameterPoolFakeDevice(ParameterSetProbe& probe) noexcept : _probe(probe) {}
    Nullable<unique_ptr<gpu::ShaderParameterSet>> CreateShaderParameterSet(const gpu::ShaderParameterSetDescriptor&) noexcept override {
        ++_probe.CreatedSets;
        return make_unique<ProbedParameterSet>(_probe);
    }

private:
    ParameterSetProbe& _probe;
};

TEST(TransientParameterPoolFakeTest, SortedAndUnsortedKeysSurviveFailedWritesAndRetry) {
    ParameterSetProbe probe;
    ParameterPoolFakeDevice device{probe};
    test::FakeBuffer buffer{1024, gpu::BufferUse::CBuffer};
    auto layout = make_shared<FakeBindingGroupLayout>();
    TransientParameterSetPool pool{device};
    const array keyA{TransientParameterBinding{2, 0, gpu::ShaderBufferBinding{&buffer, {0, 256}, 0}},
                     TransientParameterBinding{1, 0, gpu::ShaderBufferBinding{&buffer, {256, 256}, 0}}};
    const array keyB{keyA[1], TransientParameterBinding{2, 0, gpu::ShaderBufferBinding{&buffer, {512, 256}, 0}}};
    probe.FailSetCall = 2;
    EXPECT_FALSE(pool.AcquireParameterSet(layout, keyA));
    EXPECT_EQ(probe.ResetCalls, 1u);
    const auto a = pool.AcquireParameterSet(layout, keyA);
    ASSERT_TRUE(a);
    EXPECT_EQ(probe.CreatedSets, 1u);
    probe.FailNextFlush = true;
    EXPECT_FALSE(pool.AcquireParameterSet(layout, keyB));
    EXPECT_EQ(probe.ResetCalls, 2u);
    EXPECT_EQ(pool.AcquireParameterSet(layout, keyA).Get(), a.Get());
    const auto b = pool.AcquireParameterSet(layout, keyB);
    ASSERT_TRUE(b);
    ASSERT_NE(a.Get(), b.Get());
    EXPECT_EQ(static_cast<ProbedParameterSet*>(a.Get())->Bindings, (vector<TransientParameterBinding>{keyA[1], keyA[0]}));
    EXPECT_EQ(static_cast<ProbedParameterSet*>(b.Get())->Bindings, (vector<TransientParameterBinding>{keyB[0], keyB[1]}));
    EXPECT_EQ(pool.AcquireParameterSet(layout, keyA).Get(), a.Get());
    EXPECT_EQ(pool.AcquireParameterSet(layout, keyB).Get(), b.Get());
    EXPECT_EQ(probe.CreatedSets, 2u);
    EXPECT_EQ(probe.FlushCalls, 3u);
}

TEST(TransientParameterPoolFakeTest, CollisionsGrowthReorderedReuseAndWriteFailureRollback) {
    ParameterSetProbe probe;
    ParameterPoolFakeDevice device{probe};
    TransientParameterSetPool pool{device};
    auto layout = make_shared<FakeBindingGroupLayout>();
    test::FakeBuffer buffer{65536, gpu::BufferUse::CBuffer};

    constexpr uint32_t kKeyCount = 64;
    vector<gpu::ShaderParameterSet*> frame0Sets(kKeyCount, nullptr);
    const auto makeBindings = [&](uint32_t key) {
        return array<TransientParameterBinding, 2>{
            TransientParameterBinding{2, 0, gpu::ShaderBufferBinding{&buffer, {uint64_t{key} * 256, 256}, 0}},
            TransientParameterBinding{1, 0, gpu::ShaderBufferBinding{&buffer, {uint64_t{key} * 128, 128}, 0}}};
    };

    // Frame 0: insert 64 keys (triggers table growth 16 -> 32 -> 64 -> 128 and probing collisions).
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        auto bindings = makeBindings(i);
        auto set = pool.AcquireParameterSet(layout, bindings);
        ASSERT_TRUE(set);
        frame0Sets[i] = set.Get();
    }
    EXPECT_EQ(probe.CreatedSets, kKeyCount);
    EXPECT_EQ(probe.SetCalls, kKeyCount * 2);
    EXPECT_EQ(probe.FlushCalls, kKeyCount);

    // Every key hits its original parameter set after growth (including already-sorted input).
    for (uint32_t i = kKeyCount; i > 0; --i) {
        auto bindings = makeBindings(i - 1);
        std::swap(bindings[0], bindings[1]);
        auto hit = pool.AcquireParameterSet(layout, bindings);
        ASSERT_TRUE(hit);
        EXPECT_EQ(hit.Get(), frame0Sets[i - 1]);
    }
    EXPECT_EQ(probe.SetCalls, kKeyCount * 2);
    EXPECT_EQ(probe.FlushCalls, kKeyCount);

    // Write failure rolls back Used and does not poison dedup table.
    probe.FailNextFlush = true;
    auto failingBindings = makeBindings(kKeyCount + 1);
    EXPECT_FALSE(pool.AcquireParameterSet(layout, failingBindings));
    EXPECT_EQ(probe.ResetCalls, 1u);
    auto recovered = pool.AcquireParameterSet(layout, failingBindings);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(probe.CreatedSets, kKeyCount + 1);

    // Frame 1: BeginFrame clears every occupied bucket (including post-rehash slots) and unbinds all sets.
    pool.BeginFrame();
    EXPECT_EQ(probe.ResetCalls, 1u + kKeyCount + 1u);
    const uint32_t setsBeforeFrame1 = probe.CreatedSets;
    const uint32_t writesBeforeFrame1 = probe.SetCalls;
    const uint32_t flushesBeforeFrame1 = probe.FlushCalls;

    // Re-acquire keys in reversed/permuted order: every key must miss (proving bucket was cleared), rewrite, and reuse existing sets.
    vector<gpu::ShaderParameterSet*> frame1Sets(kKeyCount, nullptr);
    for (uint32_t step = 0; step < kKeyCount; ++step) {
        const uint32_t key = (step * 17 + 13) % kKeyCount;
        auto bindings = makeBindings(key);
        auto set = pool.AcquireParameterSet(layout, bindings);
        ASSERT_TRUE(set);
        frame1Sets[key] = set.Get();
    }
    EXPECT_EQ(probe.CreatedSets, setsBeforeFrame1);
    EXPECT_EQ(probe.SetCalls, writesBeforeFrame1 + kKeyCount * 2);
    EXPECT_EQ(probe.FlushCalls, flushesBeforeFrame1 + kKeyCount);

    // Re-query within frame 1 hits frame 1's sets without extra writes.
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        auto bindings = makeBindings(i);
        auto hit = pool.AcquireParameterSet(layout, bindings);
        ASSERT_TRUE(hit);
        EXPECT_EQ(hit.Get(), frame1Sets[i]);
    }
    EXPECT_EQ(probe.SetCalls, writesBeforeFrame1 + kKeyCount * 2);
    EXPECT_EQ(probe.FlushCalls, flushesBeforeFrame1 + kKeyCount);
}

}  // namespace
}  // namespace radray
