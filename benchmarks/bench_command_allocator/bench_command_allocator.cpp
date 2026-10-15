#include "bench_command_allocator.h"
#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <radray/profiler.h>
#include <radray/logger.h>
#include <cstring>
#include <radray/runtime/gpu_system.h>
#include <radray/scope_guard.h>
#include <radray/file.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/window/native_window.h>
#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace {
bool SignalBenchmarkGate(radray::render::Fence* fence, radray::render::RenderBackend backend, uint64_t value) {
#if defined(RADRAY_ENABLE_D3D12)
    if (backend == radray::render::RenderBackend::D3D12)
        return SUCCEEDED(static_cast<radray::render::d3d12::FenceD3D12*>(fence)->_fence->Signal(value));
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (backend == radray::render::RenderBackend::Vulkan) {
        auto* native = static_cast<radray::render::vulkan::FenceVulkan*>(fence);
        const VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, native->_fence->_semaphore, value};
        return native->_device->_ftb.vkSignalSemaphore(native->_device->_device, &signal) == VK_SUCCESS;
    }
#endif
    return false;
}
}  // namespace

void radray::benchmarking::RegisterCommandAllocatorBenchmarks() {
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (bool validation : {false, true}) {
            const auto name = fmt::format("RhiBinding/{}/{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", validation ? "validation" : "raw");
            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                FrameTimeline timeline{1};
                RuntimeStartupResult startup;
                GpuSystemDescriptor descriptor{.RhiValidation = {.IsEnabled = validation}, .FlightDataCount = 1, .EnableFrameProfiler = false};
                const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
                if (backend == render::RenderBackend::Vulkan) descriptor.Device = render::VulkanDeviceDescriptor{.Queues = std::span{&queue, 1}};
                auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
                if (!gpu) {
                    state.SkipWithError(startup.Reason);
                    return;
                }
                auto* device = gpu->GetDevice();
                const auto target = render::GetShaderTargetForBackend(backend);
                const auto suffix = backend == render::RenderBackend::D3D12 ? "compute.dxil.bin" : "compute.spirv.bin";
                auto blob = ReadBinaryFile(std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_artifacts" / suffix);
                if (!blob || blob->size() < sizeof(shader::WireMetadataEnvelope)) {
                    state.SkipWithError("compute fixture is unavailable");
                    return;
                }
                shader::WireMetadataEnvelope envelope;
                std::memcpy(&envelope, blob->data(), sizeof(envelope));
                auto artifact = render::CreateBackendShaderArtifact(*device, *blob, {.Target = *target, .ExpectedGpuArtifact = envelope.GpuArtifact, .ExpectedToolchainIdentity = shader::kShaderToolchainIdentity});
                if (!artifact) {
                    state.SkipWithError("compute artifact creation failed");
                    return;
                }
                auto shader = device->CreateShader({artifact->Generic().Bytecode(), artifact->Category, render::ShaderStage::Compute});
                if (!shader) {
                    state.SkipWithError("compute shader creation failed");
                    return;
                }
                auto pipeline = device->CreateComputePipelineState({artifact->Layout.get(), {shader.Get(), "CSMain"}});
                auto output = device->CreateBuffer({256, render::MemoryType::Device, render::BufferUse::UnorderedAccess});
                auto set = device->CreateShaderParameterSet({artifact->Layout.get(), 0});
                if (!pipeline || !output || !set || !set->Set(artifact->Layout->FindBinding("Output"), 0, render::ShaderBufferBinding{output.Get(), {0, 256}, 4}) || !set->FlushWrites()) {
                    state.SkipWithError("binding fixture creation failed");
                    return;
                }
                auto allocator = device->CreateCommandAllocator(gpu->GetMainQueue()).Unwrap();
                auto command = device->CreateCommandBuffer(allocator.get()).Unwrap();
                const auto record = [&] {
                    command->Begin();
                    auto encoder = command->BeginComputePass().Unwrap();
                    encoder->BindComputePipelineState(pipeline.Get());
                    for (uint32_t i = 0; i < 128; ++i) encoder->BindShaderParameterSet(0, set.Get());
                    command->EndComputePass(std::move(encoder));
                    command->End();
                };
                record();
                for (auto _ : state) {
                    allocator->Reset();
                    record();
                }
                state.counters["bindings/iteration"] = 128;
                state.SetItemsProcessed(state.iterations() * 128);
            })->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
        }
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
                        descriptor.RhiValidation.IsEnabled = false;
                        auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
                        if (!gpu) {
                            state.SkipWithError(startup.Reason);
                            return;
                        }
                        auto* device = gpu->GetDevice();
                        vector<unique_ptr<render::CommandAllocator>> allocators;
                        vector<unique_ptr<render::CommandBuffer>> commands;
                        const uint32_t allocatorCount = shared ? (backend == render::RenderBackend::D3D12 ? overlap : 1u) : count;
                        for (uint32_t i = 0; i < allocatorCount; ++i) {
                            auto allocator = device->CreateCommandAllocator(gpu->GetMainQueue());
                            if (!allocator) {
                                state.SkipWithError("allocator creation failed");
                                return;
                            }
                            allocators.push_back(allocator.Release());
                        }
                        for (uint32_t i = 0; i < count; ++i) {
                            auto command = device->CreateCommandBuffer(allocators[i % allocatorCount].get());
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
                        const size_t nativeCount = allocators.size();
#if defined(RADRAY_ENABLE_D3D12)
                        if (backend == render::RenderBackend::D3D12) {
                            state.counters["sizeof_allocator"] = sizeof(render::d3d12::CommandAllocatorD3D12);
                            state.counters["sizeof_command"] = sizeof(render::d3d12::CmdListD3D12);
                            state.counters["sizeof_graphics_encoder"] = sizeof(render::d3d12::CmdRenderPassD3D12);
                            state.counters["sizeof_parameter_set"] = sizeof(render::d3d12::ShaderParameterSetD3D12);
                            state.counters["sizeof_graphics_pso"] = sizeof(render::d3d12::GraphicsPsoD3D12);
                        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
                        if (backend == render::RenderBackend::Vulkan) {
                            state.counters["sizeof_allocator"] = sizeof(render::vulkan::CommandAllocatorVulkan);
                            state.counters["sizeof_command"] = sizeof(render::vulkan::CommandBufferVulkan);
                            state.counters["sizeof_graphics_encoder"] = sizeof(render::vulkan::SimulateCommandEncoderVulkan);
                            state.counters["sizeof_parameter_set"] = sizeof(render::vulkan::ShaderParameterSetVulkan);
                            state.counters["sizeof_graphics_pso"] = sizeof(render::vulkan::GraphicsPipelineVulkan);
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
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (uint32_t variant = 0; variant < 3; ++variant) {
            const array<std::string_view, 3> variants{"single", "multi", "gate"};
            const auto name = fmt::format("FlightSubmission/{}/{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", variants[variant]);
            benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                FrameTimeline timeline{1};
                RuntimeStartupResult startup;
                GpuSystemDescriptor descriptor{.FlightDataCount = 1, .EnableFrameProfiler = true};
                const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
                if (backend == render::RenderBackend::Vulkan) descriptor.Device = render::VulkanDeviceDescriptor{.Queues = std::span{&queue, 1}};
                descriptor.RhiValidation.IsEnabled = false;
                auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
                if (!gpu) {
                    state.SkipWithError(startup.Reason);
                    return;
                }
                auto source = gpu->GetDevice()->CreateBuffer({128, render::MemoryType::Upload, render::BufferUse::CopySource | render::BufferUse::MapWrite}).Unwrap();
                auto target = gpu->GetDevice()->CreateBuffer({128, render::MemoryType::ReadBack, render::BufferUse::CopyDestination}).Unwrap();
                {
                    ScopedBufferMap map{source.get(), {0, 128}};
                    std::memset(map.Data(), 17, 128);
                }
                auto gate = gpu->GetDevice()->CreateFence().Unwrap();
                auto intermediate = gpu->GetDevice()->CreateFence().Unwrap();
                uint64_t serial = 0;
                double submitUs = 0, gpuMs = 0;
                const uint32_t batches = variant == 0 ? 1 : 8;
                const auto execute = [&] {
                    auto value = ++serial;
                    gpu->BeginUpdateForFlight(0);
                    auto frame = gpu->BeginFrameRecord(0, {}, {}, false);
                    for (uint32_t i = 0; i < batches; ++i) {
                        auto* command = frame.AllocateCommandBuffer();
                        command->CopyBufferToBuffer(target.get(), i * (128 / batches), source.get(), i * (128 / batches), 128 / batches);
                        render::Fence* signal = intermediate.get();
                        render::Fence* wait = gate.get();
                        frame.ReturnCommandBuffers({.CmdBuffers = std::span{&command, 1},
                                                    .SignalFences = variant == 2 && i == 0 ? std::span{&signal, 1} : std::span<render::Fence*>{},
                                                    .SignalValues = variant == 2 && i == 0 ? std::span{&value, 1} : std::span<uint64_t>{},
                                                    .WaitFences = variant == 2 && i == batches - 1 ? std::span{&wait, 1} : std::span<render::Fence*>{},
                                                    .WaitValues = variant == 2 && i == batches - 1 ? std::span{&value, 1} : std::span<uint64_t>{}});
                    }
                    const auto start = std::chrono::steady_clock::now();
                    frame.SubmitFrame();
                    submitUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                    if (variant == 2) {
                        intermediate->Wait(value);
                        if (gpu->CompleteFlightIfReady(0, false)) return false;
                        if (!SignalBenchmarkGate(gate.get(), backend, value)) return false;
                    }
                    if (!gpu->CompleteFlightIfReady(0, true)) return false;
                    FlightCompletion completion;
                    while (timeline.TryReadCompletion(completion)) gpu->ReleaseFrameResourcesGT(completion);
                    gpuMs += gpu->GetLastGpuTimeMs();
                    return true;
                };
                for (uint32_t warm = 0; warm < 8; ++warm)
                    if (!execute()) RADRAY_ABORT("flight benchmark warmup failed");
                submitUs = gpuMs = 0;
                for (auto _ : state)
                    if (!execute()) RADRAY_ABORT("flight benchmark execution failed");
                state.counters["submit_us/frame"] = submitUs / state.iterations();
                state.counters["gpu_ms/frame"] = gpuMs / state.iterations();
                state.counters["application_batches/frame"] = batches;
                // Gate GPU timestamps include the intentional host-signaled dependency gap.
            })->UseRealTime()
                ->Unit(benchmark::kMicrosecond);
        }
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (uint32_t variant = 0; variant < 6; ++variant)
            for (uint32_t views : {1u, 3u}) {
                const array<std::string_view, 6> variants{"stable", "clear", "load_store", "resize_variants", "recreate", "budget"};
                const auto name = fmt::format("RenderScope/{}/{}/V{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", variants[variant], views);
                benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
                    FrameTimeline timeline{1};
                    RuntimeStartupResult startup;
                    GpuSystemDescriptor descriptor{.FlightDataCount = 1, .EnableFrameProfiler = false};
                    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
                    if (backend == render::RenderBackend::Vulkan) descriptor.Device = render::VulkanDeviceDescriptor{.Queues = std::span{&queue, 1}};
                    descriptor.RhiValidation.IsEnabled = false;
                    auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
                    if (!gpu) {
                        state.SkipWithError(startup.Reason);
                        return;
                    }
                    vector<unique_ptr<render::Texture>> textures;
                    vector<unique_ptr<render::TextureView>> targets;
                    for (uint32_t i = 0; i < (variant == 3 || variant == 5 ? 3u : 1u); ++i) {
                        auto texture = gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 64u << i, 64u << i, 1, 1, 1,
                                                                        render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget})
                                           .Unwrap();
                        targets.push_back(gpu->GetDevice()->CreateTextureView({texture.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Unwrap());
                        textures.push_back(std::move(texture));
                    }
#if defined(RADRAY_ENABLE_VULKAN)
                    if (variant == 5 && backend == render::RenderBackend::Vulkan)
                        static_cast<render::vulkan::DeviceVulkan*>(gpu->GetDevice())->_renderPassCache->SetBudget({2, 2, 2, 2});
#endif
                    auto storage = gpu->GetDevice()->CreateCommandAllocator(gpu->GetMainQueue()).Unwrap();
                    auto command = gpu->GetDevice()->CreateCommandBuffer(storage.get()).Unwrap();
                    uint64_t frame = 0;
                    const auto record = [&] {
                        storage->Reset();
                        const auto image = frame++ % targets.size();
                        const auto size = variant == 4 ? 64u + uint32_t(frame % 16) * 8 : 64u << image;
                        if (variant == 4) {
                            targets[0].reset();
                            textures[0].reset();
                            textures[0] = gpu->GetDevice()->CreateTexture({render::TextureDimension::Dim2D, size, size, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget}).Unwrap();
                            targets[0] = gpu->GetDevice()->CreateTextureView({textures[0].get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Unwrap();
                        }
                        command->Begin();
                        render::RenderPassColorAttachment color{targets[image].get(), variant == 2 && frame % 2 ? render::LoadAction::Load : render::LoadAction::Clear, variant == 2 && frame % 2 ? render::StoreAction::Discard : render::StoreAction::Store, {{float(frame % 17) / 17, 0, 0, 1}}};
                        if (variant == 0) color.Clear = {{0, 0, 0, 1}};
                        for (uint32_t view = 0; view < views; ++view) {
                            auto encoder = command->BeginRenderPass({std::span{&color, 1}, {}, size, size, 1});
                            if (!encoder) {
                                command->End();
                                return false;
                            }
                            command->EndRenderPass(encoder.Release());
                        }
                        command->End();
                        return true;
                    };
                    for (uint32_t warm = 0; warm < 8; ++warm)
                        if (!record()) {
                            state.SkipWithError("render scope warmup failed");
                            return;
                        }
                    RADRAY_PROFILE_MESSAGE("RenderScope warmup complete");
                    for (auto _ : state)
                        if (!record()) {
                            state.SkipWithError("render scope preparation failed");
                            break;
                        }
                    storage->Reset();
                    state.counters["scopes/frame"] = views;
                    // CPU native preparation/encoding only: no GPU submission or completion time.
                })->UseRealTime()
                    ->Unit(benchmark::kMicrosecond);
            }
}
void radray::benchmarking::RegisterWindowSubmissionBenchmarks() {
    for (const auto backend : {render::RenderBackend::D3D12, render::RenderBackend::Vulkan})
        for (uint32_t flights : {1u, 2u, 3u, 4u})
            for (uint32_t count : {1u, 3u})
                for (uint32_t variant = 0; variant < 3; ++variant) {
                    const array<std::string_view, 3> variants{"stable", "resize", "output_skip"};
                    const auto name = fmt::format("WindowSubmission/{}/{}/F{}/W{}", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan", variants[variant], flights, count);
                    benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
#if defined(RADRAY_PLATFORM_WINDOWS)
                        NativeWindow::GlobalInit();
                        const auto shutdown = MakeScopeGuard([] { NativeWindow::GlobalShutdown(); });
                        FrameTimeline timeline{flights};
                        RuntimeStartupResult startup;
                        GpuSystemDescriptor descriptor{.FlightDataCount = flights, .EnableFrameProfiler = true};
                        const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
                        if (backend == render::RenderBackend::Vulkan) descriptor.Device = render::VulkanDeviceDescriptor{.Queues = std::span{&queue, 1}};
                        descriptor.RhiValidation.IsEnabled = false;
                        auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
                        if (!gpu) {
                            state.SkipWithError(startup.Reason);
                            return;
                        }
                        auto pump = NativeEventPump::Create(NativeWindowType::Win32HWND).Unwrap();
                        vector<unique_ptr<NativeWindow>> windows;
                        vector<unique_ptr<render::SwapChain>> chains;
                        struct Image {
                            unique_ptr<render::TextureView> View;
                            render::TextureStates State{render::TextureState::Undefined};
                        };
                        vector<unordered_map<render::Texture*, Image>> images(count);
                        for (uint32_t i = 0; i < count; ++i) {
                            windows.push_back(NativeWindow::Create(Win32WindowCreateDescriptor{.Title = "RadRay lifecycle benchmark", .Width = 240, .Height = 180, .X = int32_t(i * 260), .Y = 100, .ShowInTaskbar = false, .ActivateOnShow = false}).Unwrap());
                            pump->Register(windows.back().get());
                            chains.push_back(gpu->GetDevice()->CreateSwapChain({.PresentQueue = gpu->GetMainQueue(), .NativeHandler = windows.back()->GetNativeHandler(), .Width = 240, .Height = 180, .BackBufferCount = 3, .Format = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate}).Unwrap());
                        }
                        auto source = gpu->GetDevice()->CreateBuffer({64, render::MemoryType::Upload, render::BufferUse::CopySource | render::BufferUse::MapWrite}).Unwrap();
                        auto readback = gpu->GetDevice()->CreateBuffer({64, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
                        {
                            ScopedBufferMap map{source.get(), {0, 64}};
                            std::memset(map.Data(), 17, 64);
                            source->FlushMappedRange({0, 64});
                        }
                        vector<uint64_t> serials(flights, 0);
                        uint64_t frameIndex = 0, completed = 0, outputs = 0, skipped = 0;
                        double submitUs = 0, gpuMs = 0, latencyMs = 0;
                        const auto complete = [&](uint32_t flight) {
                            if (!serials[flight]) return;
                            const auto signal = gpu->GetFlightGpuSignal(flight);
                            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
                            while (!gpu->CompleteFlightIfReady(flight, false)) {
                                pump->DispatchEvents();
                                if (!signal.Fence->WaitFor(signal.Value, 1) && std::chrono::steady_clock::now() >= deadline)
                                    RADRAY_ABORT("window benchmark retirement failed");
                            }
                            gpuMs += gpu->GetLastGpuTimeMs();
                            latencyMs += double(timeline.GetLastFrameLatency().count()) * 1000;
                            ++completed;
                            RADRAY_PROFILE_PLOT("WindowSubmission/CompletedGpuMs", double(gpu->GetLastGpuTimeMs()));
                            RADRAY_PROFILE_PLOT("WindowSubmission/CompletedLatencyMs", double(timeline.GetLastFrameLatency().count()) * 1000);
                            FlightCompletion notification;
                            while (timeline.TryReadCompletion(notification)) {
                                gpu->ReleaseFrameResourcesGT(notification);
                                serials[notification.FlightIndex] = 0;
                            }
                        };
                        const auto drain = [&] {
                            for (;;) {
                                uint32_t oldest = flights;
                                for (uint32_t i = 0; i < flights; ++i)
                                    if (serials[i] && (oldest == flights || serials[i] < serials[oldest])) oldest = i;
                                if (oldest == flights) break;
                                complete(oldest);
                            }
                        };
                        const auto cleanup = MakeScopeGuard([&] {
                            drain();
                            for (auto& window : windows) pump->Unregister(window.get());
                        });
                        const auto execute = [&] {
                            pump->DispatchEvents();
                            const auto flight = uint32_t(frameIndex % flights);
                            complete(flight);
                            if (variant == 1) {
                                drain();
                                const uint32_t width = 240 + uint32_t(frameIndex % 3) * 32;
                                for (uint32_t i = 0; i < count; ++i) {
                                    images[i].clear();
                                    windows[i]->SetSize(int(width), 180);
                                    if (!chains[i]->Recreate(width, 180, render::TextureFormat::BGRA8_UNORM, render::PresentMode::Immediate)) RADRAY_ABORT("window benchmark recreation failed");
                                }
                            }
                            gpu->BeginUpdateForFlight(flight);
                            gpu->BeginFrameTiming(flight);
                            auto frame = gpu->BeginFrameRecord(flight, {}, {}, false);
                            auto* copy = frame.AllocateCommandBuffer();
                            if (backend == render::RenderBackend::Vulkan) {
                                vector<render::ResourceBarrierDescriptor> barriers;
                                if (!frameIndex) barriers.emplace_back(render::BarrierBufferDescriptor{source.get(), render::BufferState::HostWrite, render::BufferState::CopySource});
                                barriers.emplace_back(render::BarrierBufferDescriptor{readback.get(), frameIndex ? render::BufferState::HostRead : render::BufferState::Undefined, render::BufferState::CopyDestination});
                                copy->ResourceBarrier(barriers);
                            }
                            copy->CopyBufferToBuffer(readback.get(), 0, source.get(), 0, 64);
                            if (backend == render::RenderBackend::Vulkan) {
                                const render::ResourceBarrierDescriptor after = render::BarrierBufferDescriptor{readback.get(), render::BufferState::CopyDestination, render::BufferState::HostRead};
                                copy->ResourceBarrier(std::span{&after, 1});
                            }
                            frame.ReturnCommandBuffers({.CmdBuffers = std::span{&copy, 1}});
                            for (uint32_t i = 0; i < count; ++i) {
                                auto acquired = chains[i]->AcquireNext(2000);
                                if (!acquired.Frame) RADRAY_ABORT("window benchmark acquisition failed");
                                auto* texture = acquired.Frame->GetBackBuffer();
                                auto& image = images[i][texture];
                                if (!image.View) image.View = gpu->GetDevice()->CreateTextureView({texture, render::TextureDimension::Dim2D, render::TextureFormat::BGRA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Unwrap();
                                // Initialize every external image before exercising a skipped output.
                                const bool presentable = variant != 2 || frameIndex % 2 == 0 || image.State == render::TextureState::Undefined;
                                auto target = frame.AdoptSwapChainFrame(chains[i].get(), std::move(*acquired.Frame), [presentable] { return presentable; }, [](render::SwapChainPresentResult result) {
                                    if (result.Status != render::SwapChainStatus::Success && result.Status != render::SwapChainStatus::RetryLater) RADRAY_ABORT("window benchmark presentation failed"); });
                                target.OnOutput = [&](const AppFrameOutputResult& result) { ++outputs; if (!result.CommandsSubmitted) ++skipped; };
                                auto* commands = frame.AllocateCommandBuffer();
                                const auto desc = texture->GetDesc();
                                const render::RenderPassColorAttachment color{image.View.get(), render::LoadAction::Clear, render::StoreAction::Store};
                                const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{texture, image.State, render::TextureState::RenderTarget};
                                commands->ResourceBarrier(std::span{&before, 1});
                                auto encoder = commands->BeginRenderPass({std::span{&color, 1}, {}, desc.Width, desc.Height, 1}).Unwrap();
                                commands->EndRenderPass(std::move(encoder));
                                const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{texture, render::TextureState::RenderTarget, render::TextureState::Present};
                                commands->ResourceBarrier(std::span{&after, 1});
                                frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
                                image.State = render::TextureState::Present;
                            }
                            const auto start = std::chrono::steady_clock::now();
                            frame.SubmitFrame();
                            submitUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                            serials[flight] = frame.FrameSerial();
                            ++frameIndex;
                        };
                        for (uint32_t warm = 0; warm < 8 + flights * 2; ++warm) execute();
                        drain();
                        completed = outputs = skipped = 0;
                        submitUs = gpuMs = latencyMs = 0;
                        RADRAY_PROFILE_MESSAGE("WindowSubmission warmup complete");
                        for (auto _ : state) execute();
                        drain();
                        {
                            ScopedBufferMap map{readback.get(), {0, 64}};
                            readback->InvalidateMappedRange({0, 64});
                            const auto* bytes = static_cast<const uint8_t*>(map.Data());
                            for (uint32_t i = 0; i < 64; ++i)
                                if (bytes[i] != 17) RADRAY_ABORT("required copy was lost with a skipped output");
                        }
                        state.counters["submit_us/frame"] = submitUs / state.iterations();
                        state.counters["gpu_ms/frame"] = gpuMs / completed;
                        state.counters["latency_ms/frame"] = latencyMs / completed;
                        state.counters["outputs/frame"] = double(outputs) / state.iterations();
                        state.counters["skipped_outputs/frame"] = double(skipped) / state.iterations();
                        state.counters["required_copy_bytes/frame"] = 64;
#else
                        state.SkipWithError("native window benchmark requires Windows");
#endif
                    })->UseRealTime()
                        ->Unit(benchmark::kMicrosecond);
                }
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    radray::benchmarking::RegisterCommandAllocatorBenchmarks();
    radray::benchmarking::RegisterWindowSubmissionBenchmarks();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
