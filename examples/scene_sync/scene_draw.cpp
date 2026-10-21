#include "scene_draw.h"

#include <cstring>
#include <radray/profiler.h>

namespace radray::example {

unique_ptr<StaticMesh> CreateCube(GpuSystem& gpu) {
    const array<float, 24> vertices{-0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f,
                                    -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f};
    const array<uint32_t, 36> indices{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    MeshResource cpu;
    cpu.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    cpu.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 8;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 36, 0, 4};
    cpu.Primitives.push_back(std::move(primitive));
    auto* device = gpu.GetDevice();
    auto commandsStorage = device->CreateCommandAllocator(gpu.GetMainQueue());
    if (!commandsStorage) return nullptr;
    auto commands = device->CreateCommandBuffer(commandsStorage.Get());
    auto fence = device->CreateFence();
    if (!commands || !fence) return nullptr;
    StagingBufferPool staging{device};
    ResourceUploader uploader{device, staging};
    HostWriteBatch writes;
    staging.BeginEpoch(writes);
    commands->Begin();
    auto geometry = uploader.UploadMeshResource(commands.Get(), cpu);
    commands->End();
    staging.SealEpoch();
    if (!geometry) return nullptr;
    writes.Flush(*device);
    auto* commandPtr = commands.Get();
    auto* fencePtr = fence.Get();
    uint64_t signal = 1;
    gpu.GetMainQueue()->Submit({.CmdBuffers = std::span{&commandPtr, 1}, .SignalFences = std::span{&fencePtr, 1}, .SignalValues = std::span{&signal, 1}});
    fence->Wait(signal);
    staging.CompleteEpoch();
    return make_unique<StaticMesh>(std::move(cpu), vector<StaticMeshSection>{}, Eigen::Vector3f{-0.5f, -0.5f, -0.5f}, Eigen::Vector3f{0.5f, 0.5f, 0.5f}, std::move(*geometry));
}

}  // namespace radray::example
