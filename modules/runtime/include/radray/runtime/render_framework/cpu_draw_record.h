#pragma once

#include <radray/basic_math.h>
#include <radray/hash.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>
#include <radray/types.h>

namespace radray {

class ShaderProgram;
struct RenderSceneSnapshot;

/// Sparse identity for a live Scene slot. Slot may be reused; Generation must match.
struct SceneObjectId {
    uint32_t Slot{0};
    uint32_t Generation{0};
    bool IsValid() const noexcept { return Generation != 0; }
    friend bool operator==(const SceneObjectId&, const SceneObjectId&) = default;
};

enum class DrawRecordStatus : uint8_t { Ready,
                                         MissingPass,
                                         InvalidBindings,
                                         InvalidGeometry };

/// Long-lived CPU draw description. No frame CB offsets, view pointers, or graph handles.
struct DrawRecord {
    uint32_t Id{0};
    uint32_t Generation{0};
    SceneObjectId PrimitiveId{};
    RenderPrimitiveIndex Primitive{0};
    MeshBatchIndex Batch{0};
    RenderMaterialIndex Material{0};
    uint32_t SectionIndex{0};
    uint32_t PassIndex{0};
    uint32_t PassNameHash{0};
    uint32_t ProgramFrameId{0};
    RenderQueue Queue{RenderQueue::Geometry};
    uint32_t LayerMask{0xffffffffu};
    DrawRecordStatus Status{DrawRecordStatus::InvalidGeometry};
    bool Mirrored{false};
    MeshDrawDescription Description{};
};

struct CpuDrawStoreStats {
    uint64_t DrawRecordBuilds{0};
    uint64_t DrawRecordsReused{0};
    uint64_t DrawRecordStateSelects{0};
    uint64_t DrawRecordBytes{0};
};

uint32_t HashPassName(std::string_view name) noexcept;
bool IsMirroredAffine(const Eigen::Matrix4f& localToWorld) noexcept;
render::FrontFace OppositeFrontFace(render::FrontFace face) noexcept;

/// Game-thread cache of stable draw records. Sync writes snapshot-local copies; camera-only frames reuse.
class CpuDrawStore {
public:
    bool Sync(RenderSceneSnapshot& scene);
    const CpuDrawStoreStats& GetStats() const noexcept { return _stats; }
    void ResetCounters() noexcept { _stats = {}; }

private:
    struct Key {
        uint64_t PrimitiveGeneration{0};
        uint32_t SectionIndex{0};
        uint32_t PassNameHash{0};
        uint32_t PassIndex{0};
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept;
    };
    struct Cached {
        uint64_t Epoch{0};
        uint64_t MaterialGeneration{0}, MaterialRevision{0};
        const GpuMesh::DrawData* Geometry{nullptr};
        ShaderProgram* Program{nullptr};
        MaterialPipelineState PipelineState{};
        uint32_t FirstIndex{0}, IndexCount{0};
        int32_t VertexOffset{0};
        RenderQueue Queue{RenderQueue::Geometry};
        uint32_t ProgramFrameId{0};
        DrawRecordStatus Status{DrawRecordStatus::InvalidGeometry};
        DrawRecord Record{};
    };

    unordered_map<Key, Cached, KeyHash> _cache;
    uint64_t _epoch{0};
    uint32_t _nextId{1};
    CpuDrawStoreStats _stats;
};

}  // namespace radray
