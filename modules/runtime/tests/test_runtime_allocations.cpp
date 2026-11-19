#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/transient_parameter_pool.h>
#include <radray/types.h>

#include "test_fakes.h"

#ifdef _WIN32
#include <malloc.h>
#endif

// This standalone executable replaces global operator new/delete under RADRAY_ENABLE_MIMALLOC=OFF
// so O1/O2/M2 tests can count calling-thread allocations and deallocations directly.
namespace allocation_probe {

thread_local bool Enabled = false;
thread_local size_t Allocations = 0;
thread_local size_t Deallocations = 0;

void* Allocate(size_t size) {
    if (Enabled) ++Allocations;
    if (void* result = std::malloc(std::max(size, size_t{1}))) return result;
    std::abort();
}

void Deallocate(void* ptr) noexcept {
    if (ptr != nullptr && Enabled) ++Deallocations;
    std::free(ptr);
}

void* AllocateAligned(size_t size, size_t alignment) {
    if (Enabled) ++Allocations;
    size = std::max(size, size_t{1});
#ifdef _WIN32
    void* result = _aligned_malloc(size, alignment);
#else
    void* result = std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
    if (result != nullptr) return result;
    std::abort();
}

void DeallocateAligned(void* ptr) noexcept {
    if (ptr != nullptr && Enabled) ++Deallocations;
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

struct Counts {
    size_t Allocations{0};
    size_t Deallocations{0};
};

template <class F>
Counts Measure(F&& callback) {
    Allocations = 0;
    Deallocations = 0;
    Enabled = true;
    callback();
    Enabled = false;
    return {Allocations, Deallocations};
}

}  // namespace allocation_probe

void* operator new(size_t size) { return allocation_probe::Allocate(size); }
void* operator new[](size_t size) { return allocation_probe::Allocate(size); }
void operator delete(void* value) noexcept { allocation_probe::Deallocate(value); }
void operator delete[](void* value) noexcept { allocation_probe::Deallocate(value); }
void operator delete(void* value, size_t) noexcept { allocation_probe::Deallocate(value); }
void operator delete[](void* value, size_t) noexcept { allocation_probe::Deallocate(value); }
void* operator new(size_t size, std::align_val_t alignment) { return allocation_probe::AllocateAligned(size, static_cast<size_t>(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return allocation_probe::AllocateAligned(size, static_cast<size_t>(alignment)); }
void operator delete(void* value, std::align_val_t) noexcept { allocation_probe::DeallocateAligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { allocation_probe::DeallocateAligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { allocation_probe::DeallocateAligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { allocation_probe::DeallocateAligned(value); }

namespace radray {
namespace {

class AllocFakeLayout final : public gpu::BindingGroupLayout {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    gpu::BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {}; }
};

class AllocFakeDevice final : public test::FakeDevice {
public:
    AllocFakeDevice() {
        EnableHostBuffers = true;
        EnableTextures = true;
    }
    Nullable<unique_ptr<gpu::ShaderParameterSet>> CreateShaderParameterSet(const gpu::ShaderParameterSetDescriptor&) noexcept override {
        return make_unique<test::FakeParameterSet>();
    }
};

TEST(RuntimeAllocationsTest, O2ProbeCalibrationAndSteadyStateZeroAllocations) {
    // 1. Calibration: prove probe observes radray::unordered_map node allocation on emplace and deallocation on clear.
    unordered_map<uint64_t, uint64_t> calibrationMap;
    calibrationMap.reserve(64);
    const auto emplaceCounts = allocation_probe::Measure([&] { calibrationMap.emplace(1u, 42u); });
    EXPECT_GT(emplaceCounts.Allocations, 0u);
    const auto clearCounts = allocation_probe::Measure([&] { calibrationMap.clear(); });
    EXPECT_GT(clearCounts.Deallocations, 0u);

    // 2. O2 steady-state cycle on FrameSlot: BeginFrame + misses + hits + slot reset -> 0 allocs & 0 deallocs.
    auto device = make_shared<AllocFakeDevice>();
    DeviceContext context{device, &device->Queue};
    auto scheduler = FrameScheduler::Create(context, make_unique<test::CallbackPipeline>(), 1).Unwrap();
    auto layout = make_shared<AllocFakeLayout>();
    test::FakeBuffer buffer{65536, gpu::BufferUse::CBuffer};

    constexpr uint32_t kKeyCount = 32;
    vector<array<TransientParameterBinding, 2>> keys;
    keys.reserve(kKeyCount);
    for (uint32_t i = 0; i < kKeyCount; ++i) {
        keys.push_back({
            TransientParameterBinding{2, 0, gpu::ShaderBufferBinding{&buffer, {uint64_t{i} * 256, 256}, 0}},
            TransientParameterBinding{1, 0, gpu::ShaderBufferBinding{&buffer, {uint64_t{i} * 128, 128}, 0}}});
    }

    const auto runCycle = [&] {
        auto& slot = scheduler->BeginFrame();
        // Misses (unsorted input exercises sort + insert)
        for (uint32_t i = 0; i < kKeyCount; ++i) {
            ASSERT_TRUE(slot.AcquireParameterSet(layout, keys[i]));
        }
        // Hits
        for (uint32_t i = 0; i < kKeyCount; ++i) {
            ASSERT_TRUE(slot.AcquireParameterSet(layout, keys[i]));
        }
        ASSERT_TRUE(scheduler->RunFrame());
    };

    // Warmup
    for (uint32_t w = 0; w < 3; ++w) runCycle();

    const auto steady = allocation_probe::Measure([&] { runCycle(); });
    EXPECT_EQ(steady.Allocations, 0u);
    EXPECT_EQ(steady.Deallocations, 0u);
    scheduler->WaitIdle();
}

TEST(RuntimeAllocationsTest, O1SteadyStateRdgBuildAndInlineGeneratorZeroGlobalAllocations) {
    auto device = make_shared<AllocFakeDevice>();
    DeviceContext context{device, &device->Queue};
    auto scheduler = FrameScheduler::Create(context, make_unique<test::CallbackPipeline>(), 1).Unwrap();
    RDGTransientResourcePool pool{*device};
    UploadArena uploads{*device, 64 * 1024};
    const string longRaster(48, 'R');
    const string longCompute(48, 'C');
    const string longCopy(48, 'P');
    const gpu::TextureDescriptor colorDesc{
        .Dim = gpu::TextureDimension::Dim2D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = gpu::TextureFormat::RGBA8_UNORM,
        .Memory = gpu::MemoryType::Device,
        .Usage = gpu::TextureUse::RenderTarget | gpu::TextureUse::Resource};

    const auto runFrame = [&] {
        auto& slot = scheduler->BeginFrame();
        pool.BeginFrame();
        uploads.Reset();
        RDGBuilder graph{slot};
        const auto color = graph.CreateTexture(colorDesc);
        const auto dynBuf = graph.CreateBuffer(
            RDGDynamicBufferDescriptor{.MaxSize = 16, .Usage = gpu::BufferUse::CBuffer},
            RDGUploadSource::Generated(16, uint64_t{0xCAFEBABEULL},
                                       +[](const uint64_t& val, RDGUploadWriteContext& out) noexcept {
                                           std::memcpy(out.Bytes.data(), &val, sizeof(val));
                                           return true;
                                       }));
        graph.AddRasterPass(longRaster, uint32_t{1}, +[](const uint32_t&, RDGRasterPassContext&) noexcept { return true; })
            .WriteColor(0, color, gpu::LoadAction::Clear, gpu::StoreAction::Store)
            .ReadBuffer(dynBuf, gpu::BufferState::CBuffer)
            .NeverCull();
        graph.AddComputePass(longCompute, uint32_t{2}, +[](const uint32_t&, RDGComputePassContext&) noexcept { return true; })
            .ReadBuffer(dynBuf, gpu::BufferState::CBuffer)
            .NeverCull();
        graph.AddCopyPass(longCopy, uint32_t{3}, +[](const uint32_t&, RDGCopyPassContext&) noexcept { return true; })
            .NeverCull();
        auto compiled = graph.Compile();
        ASSERT_TRUE(compiled);
        ASSERT_TRUE(compiled->Prepare(slot, pool, uploads));
        ASSERT_TRUE(scheduler->RunFrame());
    };

    // Warmup settles 128 KiB slot buffer, scratch pool chunks, transient pool, and upload arena.
    for (uint32_t w = 0; w < 4; ++w) runFrame();

    const auto steady = allocation_probe::Measure([&] { runFrame(); });
    EXPECT_EQ(steady.Allocations, 0u);
    EXPECT_EQ(steady.Deallocations, 0u);
    scheduler->WaitIdle();
}

TEST(RuntimeAllocationsTest, M2SortVisibleCommandsAlreadySortedZeroAllocations) {
    MeshPassFrameDrawList list;
    constexpr uint32_t kCount = 256;
    list.VisibleCommands.reserve(kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        list.VisibleCommands.push_back({.SortKey = uint64_t{i / 4} << 16, .FirstInstance = i});
    }
    const auto counts = allocation_probe::Measure([&] { list.SortVisibleCommands(); });
    EXPECT_EQ(counts.Allocations, 0u);
    EXPECT_EQ(counts.Deallocations, 0u);
    ASSERT_EQ(list.VisibleCommands.size(), kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        EXPECT_EQ(list.VisibleCommands[i].FirstInstance, i);
    }
}

}  // namespace
}  // namespace radray
