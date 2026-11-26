#include "gpu_test_fixture.h"

#include <string>
#include <magic_enum/magic_enum.hpp>

#include <array>
#include <latch>
#include <thread>
#include <radray/gpu/external_resource.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <algorithm>
#include <radray/gpu_impl/backend/d3d12_impl.h>
#endif

namespace radray::gpu {
namespace {

#if defined(RADRAY_ENABLE_D3D12)
TEST(D3D12Samplers, ConcurrentDirectCreationAllocatesDistinctIdentities) {
    auto factory = d3d12::CreateDXGIFactory({});
    ASSERT_TRUE(factory);
    auto device = d3d12::CreateDevice({.Factory = factory.Get(), .QueueCounts = {}});
    if (!device) GTEST_SKIP() << "D3D12 device unavailable";
    const SamplerDescriptor desc{.AddressS = AddressMode::ClampToEdge, .AddressT = AddressMode::ClampToEdge, .AddressR = AddressMode::ClampToEdge,
                                 .MinFilter = FilterMode::Nearest, .MagFilter = FilterMode::Nearest, .MipmapFilter = FilterMode::Nearest, .AnisotropyClamp = 1};
    array<uint64_t, 256> ids{};
    array<std::jthread, 4> workers;
    std::latch start{static_cast<std::ptrdiff_t>(workers.size())};
    for (size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::jthread{[&, index] {
            start.arrive_and_wait();
            for (size_t i = index; i < ids.size(); i += workers.size()) {
                auto sampler = device->CreateSampler(desc).Unwrap();
                ids[i] = static_cast<d3d12::SamplerD3D12*>(sampler.get())->_samplerId;
            }
        }};
    }
    for (auto& worker : workers) worker.join();
    std::ranges::sort(ids);
    EXPECT_NE(ids.front(), 0u);
    EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end());
}

TEST(D3D12Fence, DescendingTargetsBoundRegistrationsAndRecoverFromEventAllocationFailures) {
    auto factory = d3d12::CreateDXGIFactory({});
    ASSERT_TRUE(factory);
    auto device = d3d12::CreateDevice({.Factory = factory.Get(), .QueueCounts = {}});
    ASSERT_TRUE(device);
    auto fence = device->CreateFenceD3D12(0);
    ASSERT_TRUE(fence);
    constexpr uint64_t timeoutNs = 100'000;
    const auto registerEvent = +[](ID3D12Fence* native, uint64_t value, HANDLE event) noexcept {
        return native->SetEventOnCompletion(value, event);
    };
    const auto failRegistration = +[](ID3D12Fence*, uint64_t, HANDLE) noexcept { return E_OUTOFMEMORY; };
    const auto failCreation = +[](bool) noexcept -> std::optional<d3d12::Win32Event> { return std::nullopt; };
    EXPECT_EQ(fence->WaitWithEvents(65, timeoutNs, d3d12::Win32Event::Create, failRegistration), FenceStatus::Timeout);
    ASSERT_EQ(fence->_eventPool.size(), 1u);
    EXPECT_EQ(fence->_eventPool.front().ArmedValue, 0u);
    EXPECT_EQ(fence->WaitForStatus(64, timeoutNs), FenceStatus::Timeout);
    EXPECT_EQ(fence->WaitWithEvents(63, timeoutNs, failCreation, registerEvent), FenceStatus::Timeout);
    EXPECT_EQ(fence->_eventPool.size(), 1u);
    for (uint64_t value = 63; value != 0; --value)
        EXPECT_EQ(fence->WaitForStatus(value, timeoutNs), FenceStatus::Timeout);
    ASSERT_EQ(fence->_eventPool.size(), 16u);
    const auto lower = std::ranges::find(fence->_eventPool, 63u, &d3d12::FenceD3D12::WaitEvent::ArmedValue);
    ASSERT_NE(lower, fence->_eventPool.end());
    ASSERT_EQ(fence->_fence->Signal(63), S_OK);
    EXPECT_EQ(::WaitForSingleObject(lower->Handle.Get(), 1000), WAIT_OBJECT_0);
    EXPECT_EQ(fence->WaitForStatus(63, 0), FenceStatus::Completed);
    ASSERT_EQ(fence->_fence->Signal(65), S_OK);
}

TEST(D3D12DeviceLoss, WaitRejectsCachedValueOnFirstAndSubsequentLoss) {
    auto factory = d3d12::CreateDXGIFactory({});
    ASSERT_TRUE(factory);
    const auto adapters = factory->GetAdapters();
    const auto software = std::ranges::find_if(adapters, [](const DXGIAdapterInfo& info) {
        return info.IsSoftware && info.IsD3D12Supported;
    });
    if (software == adapters.end()) GTEST_SKIP() << "D3D12 software adapter unavailable";
    auto device = d3d12::CreateDevice({.Factory = factory.Get(), .AdapterIndex = software->Index, .QueueCounts = {}});
    ASSERT_TRUE(device);
    auto fence = device->CreateFenceD3D12(7);
    ASSERT_TRUE(fence);
    ASSERT_EQ(fence->WaitForStatus(7, 0), FenceStatus::Completed);
    EXPECT_EQ(fence->WaitForStatus(8, 0), FenceStatus::NotReady);

    d3d12::ComPtr<ID3D12Device5> native;
    if (FAILED(device->_device.As(&native))) GTEST_SKIP() << "ID3D12Device5 unavailable";
    native->RemoveDevice();
    ASSERT_FALSE(device->IsDeviceLost());
    EXPECT_EQ(fence->WaitForStatus(7, 0), FenceStatus::DeviceLost);
    ASSERT_TRUE(device->IsDeviceLost());
    EXPECT_FALSE(device->CreateBuffer({.Size = 16, .Memory = MemoryType::HostUpload, .Usage = BufferUse::MapWrite}));
    EXPECT_EQ(fence->GetCompletedValue(), 7u);
    EXPECT_EQ(fence->WaitForStatus(7, 0), FenceStatus::DeviceLost);
}
#endif

class DeviceTest : public test::BackendTest {};

TEST_P(DeviceTest, TimelineSupportsIndependentWaitersAndFiniteTimeouts) {
    auto& device = GetDevice();
    auto fence = device.CreateFence().Release();
    ASSERT_TRUE(fence);
    EXPECT_EQ(fence->WaitForStatus(0, 0), FenceStatus::Completed);
    EXPECT_EQ(fence->WaitForStatus(1, 0), FenceStatus::NotReady);
    for (const uint64_t target : {8u, 8u, 1u})
        EXPECT_EQ(fence->WaitForStatus(target, 100'000), FenceStatus::Timeout);

    array<FenceStatus, 4> results{};
    std::latch started{4};
    array<std::jthread, 4> workers;
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::jthread{[&, i] {
            started.count_down();
            results[i] = fence->WaitForStatus(i + 1, 5'000'000'000);
        }};
    }
    started.wait();
    Fence* signal = fence.get();
    uint64_t value = workers.size();
    ASSERT_TRUE(test::DirectQueue(device).Submit({.SignalFences = {&signal, 1}, .SignalValues = {&value, 1}}));
    for (auto& worker : workers) worker.join();
    for (auto status : results) EXPECT_EQ(status, FenceStatus::Completed);
    EXPECT_EQ(fence->GetCompletedValue(), value);
}

TEST_P(DeviceTest, SubmissionChecksRecordedTexturesAndQueryPools) {
    auto& device = GetDevice();
    for (bool destroyQueries : {false, true}) {
        auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 4, .Height = 4,
            .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM,
            .Usage = TextureUse::CopyDestination}).Unwrap();
        auto queries = device.CreateQueryPool({.Type = QueryType::Timestamp, .Count = 2}).Unwrap();
        auto recording = test::BeginRecording(device);
        test::Barrier(*recording.Commands, {BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::Undefined,
                                                                    .After = TextureState::CopyDestination}});
        recording.Commands->ResetQueryPool(queries.get(), 0, 2);
        recording.Commands->End();
        if (destroyQueries) queries.reset();
        else texture.reset();
        CommandBuffer* commands[]{recording.Commands.get()};
        EXPECT_FALSE(test::DirectQueue(device).Submit({.CmdBuffers = commands}));
        EXPECT_EQ(TakeRhiValidationErrors(), 1u);
    }
}

TEST_P(DeviceTest, CreatesDirectQueueAndReportsAdapter) {
    auto& device = GetDevice();
    const auto& capabilities = device.GetCapabilities();

    EXPECT_EQ(device.GetBackend(), GetParam());
    EXPECT_EQ(capabilities.Queues[static_cast<size_t>(QueueType::Direct)].CreatedCount, 1u);
    EXPECT_EQ(test::DirectQueue(device).GetQueueType(), QueueType::Direct);
    EXPECT_FALSE(capabilities.Detail.GpuName.empty());
    EXPECT_FALSE(capabilities.Detail.DriverVersion.empty());
    EXPECT_NE(capabilities.Detail.VendorId, 0u);
    EXPECT_GT(capabilities.Limits.MaxVertexInputBindings, 0u);
    EXPECT_GE(capabilities.Limits.BufferCopyOffsetAlignment, 1u);
    EXPECT_GE(capabilities.Limits.TextureDataPlacementAlignment, 1u);
    EXPECT_GE(capabilities.Limits.TextureDataPitchAlignment, 1u);
    EXPECT_GE(capabilities.Limits.CBufferOffsetAlignment, 1u);
    RecordProperty("adapter", capabilities.Detail.GpuName.c_str());
}

TEST_P(DeviceTest, BudgetViewsFollowAdapterTopology) {
    const auto& device = GetDevice();
    const auto& memory = device.GetCapabilities().Memory;
    const auto budget = device.QueryMemoryBudget();
    EXPECT_GT(memory.DeviceLocalBytes, 0u);
    EXPECT_LE(memory.HostVisibleDeviceLocalBytes, memory.DeviceLocalBytes);
    RecordProperty("adapter", device.GetCapabilities().Detail.GpuName.c_str());
    RecordProperty("architecture", std::string{magic_enum::enum_name(memory.Architecture)});
    RecordProperty("host_visible_device_bytes", std::to_string(memory.HostVisibleDeviceLocalBytes));
    const bool uma = memory.Architecture == MemoryArchitecture::CacheCoherentUMA || memory.Architecture == MemoryArchitecture::IntegratedUMA;
    if (uma) {
        EXPECT_EQ(memory.HostSharedBytes, 0u);
        EXPECT_EQ(budget.HostShared.BudgetBytes, 0u);
        EXPECT_EQ(budget.HostShared.UsageBytes, 0u);
    }
    if (uma || (GetParam() == GpuBackend::D3D12 && memory.HostVisibleDeviceLocalBytes)) {
        EXPECT_EQ(budget.DeviceUpload.BudgetBytes, budget.DeviceLocal.BudgetBytes);
        EXPECT_EQ(budget.DeviceUpload.UsageBytes, budget.DeviceLocal.UsageBytes);
    }
    if (!memory.HostVisibleDeviceLocalBytes) {
        EXPECT_EQ(budget.DeviceUpload.BudgetBytes, 0u);
        EXPECT_EQ(budget.DeviceUpload.UsageBytes, 0u);
    }
}

TEST_P(DeviceTest, ExternalExtensionsHaveIndependentLifetimes) {
    auto& device = GetDevice();
    auto first = device.CreateExternalResourceExtension().Release();
#if defined(RADRAY_PLATFORM_WINDOWS)
    ASSERT_TRUE(first);
    auto second = device.CreateExternalResourceExtension().Release();
    ASSERT_TRUE(second);
    ASSERT_NE(first.get(), second.get());
    first.reset();

    device.Destroy();
    EXPECT_TRUE(device.IsValid());
    EXPECT_GT(TakeRhiValidationErrors(), 0u);
    const auto identity = second->GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) {
        EXPECT_FALSE(error->Message.empty());
    } else {
        EXPECT_NE(std::get<ExternalDeviceIdentity>(identity).NodeMask, 0u);
    }
#else
    EXPECT_FALSE(first);
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, DeviceTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::gpu
