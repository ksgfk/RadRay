#include <radray/runtime/render_framework/render_scene_snapshot.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <radray/logger.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_program.h>

namespace radray {

void RenderSceneSnapshot::ResetForReuse() noexcept {
    const auto previous = Stats;
    Primitives.clear();
    MeshBatches.clear();
    Materials.clear();
    Lights.clear();
    Stats = {};
    Stats.PrimitiveHighWatermark = previous.PrimitiveHighWatermark;
    Stats.BatchHighWatermark = previous.BatchHighWatermark;
    Stats.MaterialHighWatermark = previous.MaterialHighWatermark;
    Stats.LightHighWatermark = previous.LightHighWatermark;
}

bool BuildRenderSceneSnapshot(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets) {
    RenderSceneSnapshotBuilder builder;
    return builder.Build(scene, out, retainedAssets);
}

bool RenderSceneSnapshotBuilder::Build(const Scene& scene, RenderSceneSnapshot& out, vector<StreamingAssetRefAny>& retainedAssets) {
    const auto start = std::chrono::steady_clock::now();
    if (++_epoch == 0) { _materials.clear(); _programs.clear(); ++_epoch; }
    RenderSceneSnapshot next = std::move(out);
    out = {};
    auto materialStorage = std::move(next.Materials);
    next.ResetForReuse();
    next.Materials = std::move(materialStorage);
    size_t materialCount = 0;
    uint32_t programCount = 0;
    const auto ownerStart = retainedAssets.size();
    struct Rollback {
        vector<StreamingAssetRefAny>& Owners;
        size_t Start;
        bool Success{false};
        ~Rollback() { if (!Success) Owners.resize(Start); }
    } rollback{retainedAssets, ownerStart};
    constexpr size_t kMaxIndex = std::numeric_limits<uint32_t>::max();
    for (const auto& proxy : scene.Primitives()) {
        if (proxy) proxy->CollectAssetReferences(retainedAssets);
    }
    for (const auto& proxy : scene.Primitives()) {
        ++next.Stats.InputPrimitives;
        if (!proxy) continue;
        if (next.Primitives.size() >= kMaxIndex) return false;
        RenderPrimitiveData primitive;
        primitive.LocalToWorld = proxy->GetLocalToWorld();
        primitive.Generation = proxy->GetGeneration();
        primitive.MotionRevision = proxy->GetMotionRevision();
        primitive.WorldBounds = TransformBounds(proxy->GetLocalBounds(), primitive.LocalToWorld);
        primitive.LayerMask = proxy->GetLayerMask();
        primitive.DisableFrustumCulling = proxy->IsFrustumCullingDisabled();
        primitive.FirstMeshBatch = static_cast<uint32_t>(next.MeshBatches.size());
        if (!primitive.WorldBounds.IsFiniteValid()) ++next.Stats.InvalidBounds;
        const auto primitiveIndex = static_cast<uint32_t>(next.Primitives.size());
        for (uint32_t section = 0; section < proxy->GetSectionCount(); ++section) {
            ++next.Stats.InputSections;
            const auto args = proxy->GetDrawArgs(section);
            if (!args.Geometry) {
                ++next.Stats.MissingGeometry;
                continue;
            }
            if (!args.IndexCount) {
                ++next.Stats.EmptyDraw;
                continue;
            }
            const auto& ib = args.Geometry->Ibv;
            if (args.FirstIndex > std::numeric_limits<uint32_t>::max() - args.IndexCount ||
                (ib.Target && (ib.Stride == 0 || ib.Offset > ib.Target->GetDesc().Size ||
                               uint64_t{args.FirstIndex} + args.IndexCount > (ib.Target->GetDesc().Size - ib.Offset) / ib.Stride))) {
                ++next.Stats.InvalidDrawRange;
                continue;
            }
            const auto material = proxy->GetMaterial(section);
            if (!material) {
                ++next.Stats.MaterialUnavailable;
                continue;
            }
            auto [found, inserted] = _materials.try_emplace(material.Get());
            next.Stats.ScratchEntriesCreated += inserted ? 1 : 0;
            if (found->second.Epoch != _epoch) {
                found->second = {_epoch, {}};
                ++next.Stats.InputMaterials;
                if (materialCount >= kMaxIndex) return false;
                if (materialCount == next.Materials.size()) next.Materials.emplace_back();
                auto& data = next.Materials[materialCount];
                if (material->BuildRenderData(data, retainedAssets)) {
                    for (auto& pass : data.Passes) {
                        if (!pass.Program) continue;
                        auto [program, created] = _programs.try_emplace(pass.Program.Get());
                        next.Stats.ScratchEntriesCreated += created ? 1 : 0;
                        if (program->second.Epoch != _epoch) {
                            if (programCount == kMaxIndex) return false;
                            program->second = {_epoch, programCount++};
                        }
                        pass.ProgramFrameId = *program->second.Index;
                        const auto buffers = pass.Program->GetParameterLayout().Buffers();
                        for (uint32_t index = 0; index < buffers.size(); ++index)
                            next.Stats.MaterialBytesCopied += pass.Parameters.GetBufferData(index).size();
                    }
                    found->second.Index = static_cast<uint32_t>(materialCount++);
                }
            }
            if (!found->second.Index) {
                ++next.Stats.MaterialUnavailable;
                continue;
            }
            if (next.MeshBatches.size() >= kMaxIndex) return false;
            next.MeshBatches.push_back({primitiveIndex, *found->second.Index, args.Geometry, args.FirstIndex, args.IndexCount, args.VertexOffset, section});
            ++primitive.MeshBatchCount;
        }
        next.Primitives.push_back(std::move(primitive));
    }
    for (const auto& light : scene.Lights()) {
        ++next.Stats.InputLights;
        if (!light || !light->AffectsWorld()) continue;
        if (next.Lights.size() >= kMaxIndex) return false;
        RenderLightData data;
        data.Type = light->GetLightType();
        light->GetLightRenderParameters(data.Parameters);
        data.WorldBounds = {data.Parameters.WorldPosition, light->GetRadius()};
        data.LayerMask = light->GetLayerMask();
        data.CastShadow = light->CastShadow();
        next.Lights.push_back(std::move(data));
    }
    next.Materials.resize(materialCount);
    std::erase_if(_materials, [&](const auto& entry) { return entry.second.Epoch != _epoch; });
    std::erase_if(_programs, [&](const auto& entry) { return entry.second.Epoch != _epoch; });
    next.Stats.Primitives = next.Primitives.size();
    next.Stats.MeshBatches = next.MeshBatches.size();
    next.Stats.Materials = next.Materials.size();
    next.Stats.Lights = next.Lights.size();
    next.Stats.RetainedAssets = retainedAssets.size() - ownerStart;
    next.Stats.PrimitiveHighWatermark = std::max(next.Stats.PrimitiveHighWatermark, next.Primitives.capacity());
    next.Stats.BatchHighWatermark = std::max(next.Stats.BatchHighWatermark, next.MeshBatches.capacity());
    next.Stats.MaterialHighWatermark = std::max(next.Stats.MaterialHighWatermark, next.Materials.capacity());
    next.Stats.LightHighWatermark = std::max(next.Stats.LightHighWatermark, next.Lights.capacity());
    next.Stats.CpuNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    rollback.Success = true;
    out = std::move(next);
    return true;
}

}  // namespace radray
