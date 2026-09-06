#include <radray/runtime/render_framework/render_scene_snapshot.h>

#include <algorithm>
#include <limits>
#include <radray/logger.h>
#include <radray/runtime/render_framework/scene.h>

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
    RenderSceneSnapshot next = std::move(out);
    out = {};
    next.ResetForReuse();
    vector<StreamingAssetRefAny> owners;
    unordered_map<Material*, std::optional<RenderMaterialIndex>> materials;
    unordered_map<ShaderProgram*, uint32_t> programs;
    constexpr size_t kMaxIndex = std::numeric_limits<uint32_t>::max();
    for (const auto& proxy : scene.Primitives()) {
        if (proxy) proxy->CollectAssetReferences(owners);
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
            auto [found, inserted] = materials.try_emplace(material.Get(), std::nullopt);
            if (inserted) {
                ++next.Stats.InputMaterials;
                MaterialRenderData data;
                if (material->BuildRenderData(data, owners)) {
                    if (next.Materials.size() >= kMaxIndex) return false;
                    for (auto& pass : data.Passes) {
                        if (!pass.Program) continue;
                        if (!programs.contains(pass.Program.Get()) && programs.size() >= kMaxIndex) return false;
                        auto [program, unused] = programs.try_emplace(pass.Program.Get(), static_cast<uint32_t>(programs.size()));
                        pass.ProgramFrameId = program->second;
                    }
                    found->second = static_cast<uint32_t>(next.Materials.size());
                    next.Materials.push_back(std::move(data));
                }
            }
            if (!found->second) {
                ++next.Stats.MaterialUnavailable;
                continue;
            }
            if (next.MeshBatches.size() >= kMaxIndex) return false;
            next.MeshBatches.push_back({primitiveIndex, *found->second, args.Geometry, args.FirstIndex, args.IndexCount, args.VertexOffset, section});
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
    next.Stats.Primitives = next.Primitives.size();
    next.Stats.MeshBatches = next.MeshBatches.size();
    next.Stats.Materials = next.Materials.size();
    next.Stats.Lights = next.Lights.size();
    next.Stats.RetainedAssets = owners.size();
    next.Stats.PrimitiveHighWatermark = std::max(next.Stats.PrimitiveHighWatermark, next.Primitives.capacity());
    next.Stats.BatchHighWatermark = std::max(next.Stats.BatchHighWatermark, next.MeshBatches.capacity());
    next.Stats.MaterialHighWatermark = std::max(next.Stats.MaterialHighWatermark, next.Materials.capacity());
    next.Stats.LightHighWatermark = std::max(next.Stats.LightHighWatermark, next.Lights.capacity());
    retainedAssets.insert(retainedAssets.end(), std::make_move_iterator(owners.begin()), std::make_move_iterator(owners.end()));
    out = std::move(next);
    return true;
}

}  // namespace radray
