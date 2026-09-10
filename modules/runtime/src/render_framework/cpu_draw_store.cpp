#include <radray/runtime/render_framework/cpu_draw_record.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/shader_program.h>

namespace radray {

uint32_t HashPassName(std::string_view name) noexcept {
    return static_cast<uint32_t>(HashData64(name.data(), name.size()));
}

bool IsMirroredAffine(const Eigen::Matrix4f& localToWorld) noexcept {
    const float* m = localToWorld.data();
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    const float m00 = m[0], m10 = m[1], m20 = m[2];
    const float m01 = m[4], m11 = m[5], m21 = m[6];
    const float m02 = m[8], m12 = m[9], m22 = m[10];
    const float det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    return std::isfinite(det) && det < 0.0f;
}

render::FrontFace OppositeFrontFace(render::FrontFace face) noexcept {
    return face == render::FrontFace::CCW ? render::FrontFace::CW : render::FrontFace::CCW;
}

size_t CpuDrawStore::KeyHash::operator()(const Key& key) const noexcept {
    HashCode hash;
    hash.Add(key.PrimitiveGeneration);
    hash.Add(key.SectionIndex);
    hash.Add(key.PassNameHash);
    hash.Add(key.PassIndex);
    return hash.ToHashCode();
}

bool CpuDrawStore::Sync(RenderSceneSnapshot& scene, RenderValidationMode validation) {
    _stats = {};
    scene.DrawRecords.clear();
    scene.PrimitiveDrawBegin.clear();
    scene.PrimitiveDrawBegin.resize(scene.Primitives.size() + 1);
    if (++_epoch == 0) {
        _cache.clear();
        ++_epoch;
    }
    constexpr size_t kMaxIndex = std::numeric_limits<uint32_t>::max();
    scene.DrawRecords.reserve(scene.MeshBatches.size() * 2);
    for (size_t primitiveIndex = 0; primitiveIndex < scene.Primitives.size(); ++primitiveIndex) {
        scene.PrimitiveDrawBegin[primitiveIndex] = static_cast<uint32_t>(scene.DrawRecords.size());
        const auto& primitive = scene.Primitives[primitiveIndex];
        const bool mirrored = IsMirroredAffine(primitive.LocalToWorld);
        for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
            const auto batchIndex = primitive.FirstMeshBatch + offset;
            if (batchIndex >= scene.MeshBatches.size()) return false;
            const auto& batch = scene.MeshBatches[batchIndex];
            if (batch.Material >= scene.Materials.size()) return false;
            const auto& material = scene.Materials[batch.Material];
            for (uint32_t passIndex = 0; passIndex < material.Passes.size(); ++passIndex) {
                if (scene.DrawRecords.size() >= kMaxIndex) return false;
                const auto& pass = material.Passes[passIndex];
                const Key key{primitive.Generation, batch.SectionIndex, HashPassName(pass.PassName), passIndex};
                auto [found, inserted] = _cache.try_emplace(key);
                auto& cached = found->second;
                const auto* geometry = batch.Geometry.Get();
                auto* program = pass.Program.Get();
                DrawRecordStatus status = DrawRecordStatus::Ready;
                if (!pass.Valid || !program) status = DrawRecordStatus::InvalidBindings;
                else if (!geometry) status = DrawRecordStatus::InvalidGeometry;
                else if (IsRenderValidationFull(validation) && !ValidateMeshGeometry(*geometry, batch.FirstIndex, batch.IndexCount))
                    status = DrawRecordStatus::InvalidGeometry;
                const bool same = !inserted && cached.Epoch != 0 &&
                                   cached.MaterialGeneration == material.Generation && cached.MaterialRevision == material.Revision &&
                                   cached.Geometry == geometry && cached.Program == program && cached.PipelineState == pass.PipelineState &&
                                   cached.FirstIndex == batch.FirstIndex && cached.IndexCount == batch.IndexCount &&
                                   cached.VertexOffset == batch.VertexOffset && cached.Queue == material.Queue &&
                                   cached.ProgramFrameId == pass.ProgramFrameId && cached.Status == status;
                if (!same) {
                    cached.MaterialGeneration = material.Generation;
                    cached.MaterialRevision = material.Revision;
                    cached.Geometry = geometry;
                    cached.Program = program;
                    cached.PipelineState = pass.PipelineState;
                    cached.FirstIndex = batch.FirstIndex;
                    cached.IndexCount = batch.IndexCount;
                    cached.VertexOffset = batch.VertexOffset;
                    cached.Queue = material.Queue;
                    cached.ProgramFrameId = pass.ProgramFrameId;
                    cached.Status = status;
                    if (cached.Record.Id == 0) {
                        if (_nextId == 0) return false;
                        cached.Record.Id = _nextId++;
                        cached.Record.Generation = 1;
                    } else {
                        ++cached.Record.Generation;
                    }
                    cached.Record.Description.Program = program;
                    cached.Record.Description.PipelineState = pass.PipelineState;
                    cached.Record.Description.Geometry = geometry;
                    cached.Record.Description.FirstIndex = batch.FirstIndex;
                    cached.Record.Description.IndexCount = batch.IndexCount;
                    cached.Record.Description.VertexOffset = batch.VertexOffset;
                    ++_stats.DrawRecordBuilds;
                } else {
                    ++_stats.DrawRecordsReused;
                }
                cached.Epoch = _epoch;
                DrawRecord record = cached.Record;
                record.PrimitiveId = primitive.Id;
                record.Primitive = static_cast<RenderPrimitiveIndex>(primitiveIndex);
                record.Batch = batchIndex;
                record.Material = batch.Material;
                record.SectionIndex = batch.SectionIndex;
                record.PassIndex = passIndex;
                record.PassNameHash = key.PassNameHash;
                record.ProgramFrameId = pass.ProgramFrameId;
                record.Queue = material.Queue;
                record.LayerMask = primitive.LayerMask;
                record.Status = status;
                if (record.Mirrored != mirrored) ++_stats.DrawRecordStateSelects;
                record.Mirrored = mirrored;
                scene.DrawRecords.push_back(record);
            }
        }
    }
    scene.PrimitiveDrawBegin.back() = static_cast<uint32_t>(scene.DrawRecords.size());
    std::erase_if(_cache, [&](const auto& entry) { return entry.second.Epoch != _epoch; });
    _stats.DrawRecordBytes = scene.DrawRecords.capacity() * sizeof(DrawRecord);
    scene.Stats.DrawRecordBuilds = _stats.DrawRecordBuilds;
    scene.Stats.DrawRecordsReused = _stats.DrawRecordsReused;
    scene.Stats.DrawRecordStateSelects = _stats.DrawRecordStateSelects;
    scene.Stats.DrawRecordBytes = _stats.DrawRecordBytes;
    scene.Stats.CpuSceneBytes = scene.Primitives.capacity() * sizeof(RenderPrimitiveData) +
                                 scene.MeshBatches.capacity() * sizeof(MeshBatch) +
                                 scene.Materials.capacity() * sizeof(MaterialRenderData) +
                                 scene.Lights.capacity() * sizeof(RenderLightData) +
                                 scene.Stats.DrawRecordBytes;
    return true;
}

}  // namespace radray
