#include "workload.h"

#include "descriptor_probe.h"
#include "framework_bridge.h"
#include <cmath>
#include <cstring>
#include <radray/runtime/gpu_system.h>

namespace radray::example {
array<float, 4> DescriptorMaterialColor(const DescriptorReuseOptions& options, uint32_t material, uint32_t frame) {
    array<float, 4> result{.4f + .5f * float(material + 1) / float(options.Materials + 1), .8f, .6f, 1};
    if (options.Scene == "changed-override" && material == 0 && frame % 2) result[0] *= .5f;
    return result;
}
array<float, 4> DescriptorInstanceColor(uint32_t slot) { return {slot % 2 ? .75f : 1.0f, 1, slot % 3 ? .8f : 1.0f, 1}; }
Rect DescriptorObjectRect(const DescriptorReuseOptions& options, uint32_t object) {
    const auto columns = uint32_t(std::ceil(std::sqrt(double(options.ObjectCount()))));
    const auto x = object % columns, y = object / columns;
    const auto left = x * 256 / columns, top = y * 256 / columns;
    return {int32_t(left), int32_t(top), (x + 1) * 256 / columns - left, (y + 1) * 256 / columns - top};
}
array<float, 4> DescriptorViewValues(uint32_t view) { return {.2f + float(view) * .005f, view % 2 ? .6f : .4f, 0, 0}; }
uint32_t DescriptorVisibleSlot(const DescriptorReuseOptions& options, uint32_t pass, uint32_t object) {
    return options.Scene == "visible-order" && pass == 1 ? options.ObjectCount() - 1 - object : object;
}

bool RunDescriptorReuse(const DescriptorReuseOptions& options) {
    JsonWriter report;
    auto root = report.RootObject();
    root.AddUint("schema_version", 1);
    root.AddString("framework", DescriptorFrameworkBridge::ImplementationName());
    root.AddString("backend", options.Backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan");
    root.AddString("scene", options.Scene);
    root.AddString("abi", options.Scene == "mixed" ? "grouped-v1-migrated-from-mixed-v1" : "grouped-v1");
    root.AddUint("objects_requested", options.Objects);
    root.AddUint("objects", options.ObjectCount());
    root.AddUint("materials", options.Materials);
    root.AddUint("views", options.Views);
    root.AddUint("flights", options.Flights);
    root.AddUint("warmup_frames", 2 * options.Flights);
    root.AddUint("steady_frames", options.SteadyFrames);
    root.AddUint("width", 256);
    root.AddUint("height", 256);
    root.AddBool("rhi_validation", false);
    auto snapshots = root.AddArray("snapshots");
    string error;
    DescriptorProbe probe;
    const auto snapshot = [&](std::string_view phase, uint32_t frame) {
        auto item = snapshots.AppendObject();
        item.AddString("phase", phase);
        item.AddUint("frame", frame);
        probe.Snapshot(item);
    };
    const auto save = [&] {
        root.AddBool("passed", error.empty());
        root.AddString("error", error);
        if (!report.WriteFile(options.Output)) return false;
        if (!error.empty()) fmt::print(stderr, "{}\n", error);
        return error.empty();
    };
    FrameTimeline nativeTimeline{1}, timeline{options.Flights};
    GpuSystemDescriptor descriptor;
    descriptor.RhiValidation.IsEnabled = false;
    descriptor.FlightDataCount = 1;
    descriptor.EnableFrameProfiler = false;
    descriptor.DXGIFactory.IsEnableDebugLayer = false;
    descriptor.VulkanInstance.IsEnableDebugLayer = false;
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    if (options.Backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor device;
        device.Queues = std::span{&queue, 1};
        descriptor.Device = device;
    }
    RuntimeStartupResult startup;
    auto native = GpuSystem::TryCreate(descriptor, nativeTimeline, startup);
    if (!native) {
        error = startup.Reason;
        return save();
    }
    auto device = make_shared<DescriptorProbeDevice>(native->GetDevice(), probe);
    auto gpu = GpuSystem::TryAttachDevice({device, 0, 3, options.Flights, false}, timeline, startup);
    if (!gpu) {
        error = startup.Reason;
        return save();
    }
    root.AddString("gpu", device->GetDetail().GpuName);
    root.AddUint("cbuffer_alignment", device->GetCapabilities().Limits.CBufferOffsetAlignment);
    const uint32_t alignment = std::max(1u, device->GetDetail().TextureDataPitchAlignment);
    const uint32_t pitch = (256 * 4 + alignment - 1) / alignment * alignment;
    const uint64_t plane = uint64_t(pitch) * 256;
    struct Output {
        unique_ptr<render::Texture> Color, Depth;
        unique_ptr<render::TextureView> ColorView, DepthView;
        unique_ptr<render::Buffer> Readback;
        uint32_t Sequence{0}, Views{0};
        bool Pending{false}, Initialized{false};
    };
    vector<Output> outputs(options.Flights);
    auto texture = device->CreateTexture({render::TextureDimension::Dim2D, 2, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                          render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination});
    if (!texture) {
        error = "Procedural texture allocation failed";
        return save();
    }
    auto image = device->CreateTextureView({texture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource});
    if (!image) {
        error = "Procedural texture view failed";
        return save();
    }
    for (auto& output : outputs) {
        auto color = device->CreateTexture({render::TextureDimension::Dim2D, 256, 256, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                            render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource});
        auto depth = device->CreateTexture({render::TextureDimension::Dim2D, 256, 256, 1, 1, 1, render::TextureFormat::D32_FLOAT,
                                            render::MemoryType::Device, render::TextureUse::DepthStencilWrite | render::TextureUse::CopySource});
        auto readback = device->CreateBuffer({2 * plane, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead});
        if (!color || !depth || !readback) {
            error = "Output allocation failed";
            return save();
        }
        output.Color = color.Release();
        output.Depth = depth.Release();
        output.Readback = readback.Release();
        auto cv = device->CreateTextureView({output.Color.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
        auto dv = device->CreateTextureView({output.Depth.get(), render::TextureDimension::Dim2D, render::TextureFormat::D32_FLOAT, {0, 1, 0, 1}, render::TextureViewUsage::DepthWrite});
        if (!cv || !dv) {
            error = "Output view allocation failed";
            return save();
        }
        output.ColorView = cv.Release();
        output.DepthView = dv.Release();
    }
    auto bridge = make_unique<DescriptorFrameworkBridge>(native->GetDevice(), device.get(), probe, options);
    if (!bridge->Initialize(image.Get())) {
        error = bridge->Error().empty() ? "Framework initialization failed" : bridge->Error();
        return save();
    }
    uint64_t readFrames = 0, checkedPixels = 0, checkedDepth = 0;
    const auto read = [&](Output& output) {
        const auto* bytes = static_cast<const uint8_t*>(output.Readback->Map(0, 2 * plane));
        if (!bytes) {
            error = "GPU readback mapping failed";
            return;
        }
        output.Readback->InvalidateMappedRange({0, 2 * plane});
        for (uint32_t object = 0; object < options.ObjectCount(); ++object) {
            const auto rect = DescriptorObjectRect(options, object);
            const auto offset = size_t(rect.Y + int32_t(rect.Height / 2)) * pitch + size_t(rect.X + int32_t(rect.Width / 2)) * 4;
            const auto material = object % options.Materials;
            const auto slot = DescriptorVisibleSlot(options, options.PassCount() - 1, object);
            const auto tint = DescriptorMaterialColor(options, material, output.Sequence);
            const auto instance = DescriptorInstanceColor(slot);
            const auto view = DescriptorViewValues(output.Views - 1);
            const float blend = material % 2 ? view[1] * 2 - .5f : view[1] < .5f ? 0.0f
                                                                                 : 1.0f;
            const array<float, 4> left{1, 128.0f / 255, 64.0f / 255, 1}, right{64.0f / 255, 1, 128.0f / 255, 1};
            for (uint32_t channel = 0; channel < 4; ++channel) {
                const float expected = tint[channel] * instance[channel] * (left[channel] * (1 - blend) + right[channel] * blend) * 255;
                if (std::abs(float(bytes[offset + channel]) - expected) > 1.1f && error.empty())
                    error = fmt::format("Color mismatch frame={} object={} channel={} got={} expected={}", output.Sequence, object, channel, bytes[offset + channel], expected);
            }
            ++checkedPixels;
            if (options.HasDepth()) {
                float depth;
                std::memcpy(&depth, bytes + plane + offset, sizeof(depth));
                const auto expected = view[0] + .01f * instance[0] + float(slot % 5) * .001f;
                if (std::abs(depth - expected) > .00001f && error.empty())
                    error = fmt::format("Depth mismatch frame={} object={} got={} expected={}", output.Sequence, object, depth, expected);
                ++checkedDepth;
            }
        }
        output.Readback->Unmap();
        ++readFrames;
    };
    const auto complete = [&](uint32_t flight) {
        if (!outputs[flight].Pending) return;
        gpu->CompleteFlightIfReady(flight, true);
        FlightCompletion completion;
        while (timeline.TryReadCompletion(completion)) {
            read(outputs[completion.FlightIndex]);
            outputs[completion.FlightIndex].Pending = false;
            gpu->ReleaseFrameResourcesGT(completion);
        }
        timeline.CleanupCompletedFlights();
    };
    const auto warmup = 2 * options.Flights;
    const auto steadyEnd = warmup + options.SteadyFrames;
    const auto totalFrames = steadyEnd + (options.Scene == "peak-drop" ? 61 * options.Flights : 0);
    DescriptorProbe::Counters initialization;
    DescriptorProbe::Counters steady;
    for (uint32_t sequence = 0; sequence < totalFrames && error.empty(); ++sequence) {
        const auto flight = sequence % options.Flights;
        complete(flight);
        if (!error.empty()) break;
        gpu->BeginUpdateForFlight(flight);
        auto frame = gpu->BeginFrameRecord(flight, {}, {}, false);
        probe.BeginFrame();
        const auto views = options.Scene == "peak-drop" ? (sequence < steadyEnd ? std::max(6u, options.Views) : 1u) : options.Views;
        if (!bridge->Prepare(frame, views, sequence)) {
            error = bridge->Error().empty() ? "Frame preparation failed" : bridge->Error();
            frame.SubmitFrame();
            gpu->WaitAndRetireFlights();
            FlightCompletion completion;
            while (timeline.TryReadCompletion(completion)) gpu->ReleaseFrameResourcesGT(completion);
            break;
        }
        auto* commands = frame.AllocateCommandBuffer();
        if (!sequence) {
            const array<uint8_t, 8> pixels{255, 128, 64, 255, 64, 255, 128, 255};
            const TextureRegionUploadRequest upload{std::as_bytes(std::span{pixels}), texture.Get(), {.Width = 2, .Height = 1}};
            if (!frame.GetUploader().TryUploadTextureRegions(commands, std::span{&upload, 1})) error = "Texture upload failed";
        }
        auto& output = outputs[flight];
        vector<render::ResourceBarrierDescriptor> enter{
            render::BarrierTextureDescriptor{.Target = output.Color.get(), .Before = output.Initialized ? render::TextureState::CopySource : render::TextureState::Undefined, .After = render::TextureState::RenderTarget}};
        if (options.HasDepth()) enter.push_back(render::BarrierTextureDescriptor{.Target = output.Depth.get(), .Before = output.Initialized ? render::TextureState::CopySource : render::TextureState::Undefined, .After = render::TextureState::DepthWrite});
        if (options.Backend == render::RenderBackend::Vulkan) enter.push_back(render::BarrierBufferDescriptor{.Target = output.Readback.get(), .Before = output.Initialized ? render::BufferState::HostRead : render::BufferState::Undefined, .After = render::BufferState::CopyDestination});
        commands->ResourceBarrier(enter);
        for (uint32_t view = 0; view < views; ++view) {
            for (uint32_t pass = 0; pass < options.PassCount(); ++pass) {
                const bool depthOnly = options.HasDepth() && pass == 0;
                vector<render::RenderPassColorAttachment> colors;
                if (!depthOnly) colors.push_back({output.ColorView.get(), render::LoadAction::Clear, render::StoreAction::Store, {{0, 0, 0, 1}}});
                std::optional<render::RenderPassDepthStencilAttachment> depth;
                if (options.HasDepth()) depth = {output.DepthView.get(), pass == 0 ? render::LoadAction::Clear : render::LoadAction::Load, render::StoreAction::Store, render::LoadAction::DontCare, render::StoreAction::Discard, false, {1, 0}};
                auto encoder = commands->BeginRenderPass({colors, depth, 256, 256, 1});
                if (!encoder) {
                    error = "BeginRenderPass failed";
                    break;
                }
                DescriptorProbeEncoder wrapped{encoder.Get()};
                bridge->Record(&wrapped, view, pass);
                commands->EndRenderPass(encoder.Release());
            }
            if (!error.empty()) break;
        }
        vector<render::ResourceBarrierDescriptor> exit{
            render::BarrierTextureDescriptor{.Target = output.Color.get(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::CopySource}};
        if (options.HasDepth()) exit.push_back(render::BarrierTextureDescriptor{.Target = output.Depth.get(), .Before = render::TextureState::DepthWrite, .After = render::TextureState::CopySource});
        commands->ResourceBarrier(exit);
        commands->CopyTextureToBuffer(output.Readback.get(), 0, output.Color.get(), {0, 1, 0, 1});
        if (options.HasDepth()) commands->CopyTextureToBuffer(output.Readback.get(), plane, output.Depth.get(), {0, 1, 0, 1});
        if (options.Backend == render::RenderBackend::Vulkan) {
            const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = output.Readback.get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
            commands->ResourceBarrier(std::span{&barrier, 1});
        }
        frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        frame.SubmitFrame();
        output.Pending = output.Initialized = true;
        output.Sequence = sequence;
        output.Views = views;
        if (sequence + 1 == warmup) {
            snapshot("warmup", sequence + 1);
            initialization = probe.Total;
        }
        if (sequence + 1 == steadyEnd) {
            snapshot("steady", sequence + 1);
            steady = probe.Total;
        }
        if (sequence >= steadyEnd) {
            const auto lower = sequence + 1 - steadyEnd;
            if (lower == 1 || lower == options.Flights || lower == 61 * options.Flights)
                snapshot(fmt::format("drop-{}", lower), sequence + 1);
        }
    }
    for (uint32_t flight = 0; flight < options.Flights; ++flight) complete(flight);
    snapshot("drain", totalFrames);
    bridge->Unregister();
    snapshot("unregister", totalFrames);
    gpu->AbandonUnpublishedResourcesTerminalGT();
    gpu.reset();
    bridge.reset();
    snapshot("cleanup", totalFrames);
    auto phases = root.AddObject("preparation");
    auto init = phases.AddObject("initialization");
    init.AddUint("created", initialization.Created);
    init.AddUint("set", initialization.Set);
    init.AddUint("flush", initialization.Flush);
    auto stable = phases.AddObject("steady");
    stable.AddUint("created", steady.Created >= initialization.Created ? steady.Created - initialization.Created : 0);
    stable.AddUint("set", steady.Set >= initialization.Set ? steady.Set - initialization.Set : 0);
    stable.AddUint("flush", steady.Flush >= initialization.Flush ? steady.Flush - initialization.Flush : 0);
    root.AddUint("readback_frames", readFrames);
    root.AddUint("checked_color_pixels", checkedPixels);
    root.AddUint("checked_depth_pixels", checkedDepth);
    if (probe.Total.LiveSets && error.empty()) error = "Descriptor storage remains after final cleanup";
    return save();
}
}  // namespace radray::example
