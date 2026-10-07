#include "bench_command_allocator.h"
#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <radray/runtime/gpu_system.h>
#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif

void radray::benchmarking::RegisterCommandAllocatorBenchmarks() {
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (uint32_t count : {1u, 8u, 32u, 128u})
            for (uint32_t overlap : {1u, 2u, 4u}) {
                if (overlap > count) continue;
                for (bool shared : {false, true}) {
                    const auto name = fmt::format("CommandAllocator/{}/{}/M{}/K{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", shared ? "shared" : "per_command", count, overlap);
                    benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                        FrameTimeline timeline{1};
                        RuntimeStartupResult startup;
                        GpuSystemDescriptor descriptor{.FlightDataCount = 1, .EnableFrameProfiler = false};
                        const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
                        if (backend == render::RenderBackend::Vulkan) {
                            render::VulkanDeviceDescriptor vk;
                            vk.Queues = std::span{&queue, 1};
                            descriptor.Device = vk;
                        }
                        auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
                        if (!gpu) {
                            state.SkipWithError(startup.Reason);
                            return;
                        }
                        auto* device = gpu->GetDevice();
                        vector<unique_ptr<render::CommandAllocator>> allocators;
                        vector<unique_ptr<render::CommandBuffer>> commands;
                        for (uint32_t i = 0; i < (shared ? 1u : count); ++i) {
                            auto allocator = device->CreateCommandAllocator(gpu->GetMainQueue());
                            if (!allocator) {
                                state.SkipWithError("allocator creation failed");
                                return;
                            }
                            allocators.push_back(allocator.Release());
                        }
                        for (uint32_t i = 0; i < count; ++i) {
                            auto command = device->CreateCommandBuffer(allocators[shared ? 0 : i].get());
                            if (!command) {
                                state.SkipWithError("command creation failed");
                                return;
                            }
                            commands.push_back(command.Release());
                        }
                        auto source = device->CreateBuffer({count * 4, render::MemoryType::Upload, render::BufferUse::CopySource}).Unwrap();
                        auto target = device->CreateBuffer({count * 4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination}).Unwrap();
                        const auto record = [&] {
                            for (uint32_t begin = 0; begin < count; begin += overlap) {
                                const auto end = std::min(count, begin + overlap);
                                for (uint32_t i = begin; i < end; ++i) {
                                    commands[i]->Begin();
                                    commands[i]->CopyBufferToBuffer(target.get(), i * 4, source.get(), i * 4, 4);
                                }
                                for (uint32_t i = end; i > begin; --i) commands[i - 1]->End();
                            }
                        };
                        record();  // Cold native growth excluded. These lists are never submitted.
                        double resets = 0, recordings = 0;
                        for (auto _ : state) {
                            const auto start = std::chrono::steady_clock::now();
                            for (auto& allocator : allocators) allocator->Reset();
                            const auto reset = std::chrono::steady_clock::now();
                            record();
                            const auto finish = std::chrono::steady_clock::now();
                            resets += std::chrono::duration<double, std::micro>(reset - start).count();
                            recordings += std::chrono::duration<double, std::micro>(finish - reset).count();
                            state.SetIterationTime(std::chrono::duration<double>(finish - start).count());
                        }
                        size_t nativeCount = allocators.size();
#if defined(RADRAY_ENABLE_D3D12)
                        if (backend == render::RenderBackend::D3D12) {
                            nativeCount = 0;
                            for (const auto& allocator : allocators) nativeCount += static_cast<render::d3d12::CommandAllocatorD3D12*>(allocator.get())->_native.size();
                        }
#endif
                        state.counters["native_capacity"] = double(nativeCount);
                        state.counters["rhi_allocators"] = double(allocators.size());
                        state.counters["reset_us"] = resets / state.iterations();
                        state.counters["record_us"] = recordings / state.iterations();
                        state.counters["commands"] = count;
                        state.counters["peak_open"] = overlap;
                    })->UseManualTime()
                        ->Unit(benchmark::kMicrosecond);
                }
            }
}
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    radray::benchmarking::RegisterCommandAllocatorBenchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
