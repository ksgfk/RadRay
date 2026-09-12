#pragma once

#include <optional>

#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>
#include <radray/types.h>
#include <radray/vertex_data.h>
#include <radray/runtime/render_framework/render_memory_stats.h>

namespace radray {

struct PrimitiveVertexAttribute {
    string Semantic;
    uint32_t SemanticIndex{0};
    uint32_t BufferBinding{0};
    uint32_t Offset{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};

    friend bool operator==(const PrimitiveVertexAttribute&, const PrimitiveVertexAttribute&) noexcept = default;
};

/// Geometry-owned physical vertex layout. Shader locations are resolved only when a
/// concrete artifact is used to create a PSO.
class PrimitiveVertexLayout {
public:
    vector<render::VertexBufferLayout> Buffers;
    vector<PrimitiveVertexAttribute> Attributes;

    static std::optional<PrimitiveVertexLayout> FromMeshPrimitive(
        const MeshPrimitive& primitive) noexcept;

    friend bool operator==(const PrimitiveVertexLayout&, const PrimitiveVertexLayout&) noexcept = default;
};

struct PrimitiveVertexLayoutId {
    uint64_t Value{0};
    bool IsValid() const noexcept { return Value != 0; }
    friend bool operator==(const PrimitiveVertexLayoutId&, const PrimitiveVertexLayoutId&) = default;
};

struct PrimitiveVertexLayoutHash {
    size_t operator()(const PrimitiveVertexLayout& layout) const noexcept;
};

/// Cold-path layout identities. Intern pins a normalized layout until Clear; Acquire/Release retain
/// it only while recipes use it. Released IDs are never reused, including IDs kept in old snapshots.
class PrimitiveVertexLayoutRegistry {
public:
    PrimitiveVertexLayoutRegistry() = default;
    PrimitiveVertexLayoutRegistry(const PrimitiveVertexLayoutRegistry& other);
    PrimitiveVertexLayoutRegistry& operator=(const PrimitiveVertexLayoutRegistry& other);
    PrimitiveVertexLayoutRegistry(PrimitiveVertexLayoutRegistry&&) noexcept = default;
    PrimitiveVertexLayoutRegistry& operator=(PrimitiveVertexLayoutRegistry&&) noexcept = default;
    PrimitiveVertexLayoutId Intern(const PrimitiveVertexLayout& layout);
    PrimitiveVertexLayoutId Acquire(const PrimitiveVertexLayout& layout);
    bool Release(PrimitiveVertexLayoutId id) noexcept;
    void Clear() noexcept {
        _byId.clear();
        _layouts.clear();
    }
    size_t Size() const noexcept { return _layouts.size(); }
    RenderMemoryStats GetMemoryStats() const noexcept;

private:
    struct Entry {
        PrimitiveVertexLayoutId Id;
        size_t References{0};
        bool Pinned{false};
    };
    Entry& FindOrAdd(const PrimitiveVertexLayout& layout, bool retain);
    void RebuildIndex();
    unordered_map<PrimitiveVertexLayout, Entry, PrimitiveVertexLayoutHash> _layouts;
    unordered_map<uint64_t, const PrimitiveVertexLayout*> _byId;
};

class ResolvedPrimitiveVertexLayout {
public:
    ResolvedPrimitiveVertexLayout() noexcept = default;
    ResolvedPrimitiveVertexLayout(const ResolvedPrimitiveVertexLayout& other);
    ResolvedPrimitiveVertexLayout(ResolvedPrimitiveVertexLayout&& other) noexcept;
    ResolvedPrimitiveVertexLayout& operator=(const ResolvedPrimitiveVertexLayout& other);
    ResolvedPrimitiveVertexLayout& operator=(ResolvedPrimitiveVertexLayout&& other) noexcept;
    ~ResolvedPrimitiveVertexLayout() noexcept = default;

    render::VertexInputState GetState() const noexcept;

private:
    friend std::optional<ResolvedPrimitiveVertexLayout> ResolvePrimitiveVertexLayout(
        const PrimitiveVertexLayout&,
        const shader::ShaderArtifactView&) noexcept;

    void RebindSemantics() noexcept;

    vector<render::VertexBufferLayout> _buffers;
    vector<string> _semantics;
    vector<render::VertexAttribute> _attributes;
};

std::optional<ResolvedPrimitiveVertexLayout> ResolvePrimitiveVertexLayout(
    const PrimitiveVertexLayout& layout,
    const shader::ShaderArtifactView& artifact) noexcept;

}  // namespace radray
