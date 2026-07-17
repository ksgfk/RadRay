#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include <radray/guid.h>
#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/types.h>

namespace radray::render {
class Dxc;
}  // namespace radray::render

namespace radray {

class AssetManager;

struct PipelineCacheHash {
    uint64_t Low{0};
    uint64_t High{0};

    friend bool operator==(const PipelineCacheHash&, const PipelineCacheHash&) noexcept = default;
    friend auto operator<=>(const PipelineCacheHash&, const PipelineCacheHash&) noexcept = default;
};

/// A logical request for one compiled shader stage. Defines are normalized by ShaderModuleCache.
struct ShaderModuleKey {
    Guid Shader{};
    vector<string> Defines{};
    uint32_t PassIndex{0};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};

    friend bool operator==(const ShaderModuleKey&, const ShaderModuleKey&) noexcept = default;
};

struct PipelineLayoutCacheKey {
    struct ShaderIdentity {
        ShaderModuleKey Module{};
        PipelineCacheHash InterfaceHash{};

        friend bool operator==(const ShaderIdentity&, const ShaderIdentity&) noexcept = default;
    };

    struct BindingGroupLayoutReuseIdentity {
        uint32_t Group{0};
        PipelineCacheHash SourceLayoutHash{};
        uint32_t SourceGroup{0};

        friend bool operator==(
            const BindingGroupLayoutReuseIdentity&,
            const BindingGroupLayoutReuseIdentity&) noexcept = default;
    };

    vector<ShaderIdentity> Shaders{};
    vector<render::StaticSamplerDescriptor> StaticSamplers{};
    vector<render::BindingGroupLayout> BindingGroupLayouts{};
    vector<BindingGroupLayoutReuseIdentity> BindingGroupLayoutReuses{};
    vector<render::DynamicBufferBinding> DynamicBufferBindings{};
    vector<render::PushConstantBinding> PushConstantBindings{};

    friend bool operator==(const PipelineLayoutCacheKey&, const PipelineLayoutCacheKey&) noexcept;
};

struct GraphicsPsoCacheKey {
    struct ShaderEntryIdentity {
        ShaderModuleKey Module{};
        PipelineCacheHash BinaryHash{};
        string EntryPoint{};

        friend bool operator==(const ShaderEntryIdentity&, const ShaderEntryIdentity&) noexcept = default;
    };

    struct VertexElement {
        uint64_t Offset{0};
        string Semantic{};
        uint32_t SemanticIndex{0};
        render::VertexFormat Format{render::VertexFormat::UNKNOWN};
        uint32_t Location{0};

        friend bool operator==(const VertexElement&, const VertexElement&) noexcept = default;
    };

    struct VertexBufferLayout {
        uint64_t ArrayStride{0};
        render::VertexStepMode StepMode{};
        vector<VertexElement> Elements{};

        friend bool operator==(const VertexBufferLayout&, const VertexBufferLayout&) noexcept = default;
    };

    PipelineCacheHash PipelineLayoutHash{};
    std::optional<ShaderEntryIdentity> VS{};
    std::optional<ShaderEntryIdentity> PS{};
    vector<VertexBufferLayout> VertexLayouts{};
    render::PrimitiveState Primitive{};
    std::optional<render::DepthStencilState> DepthStencil{};
    render::MultiSampleState MultiSample{};
    vector<render::ColorTargetState> ColorTargets{};

    friend bool operator==(const GraphicsPsoCacheKey&, const GraphicsPsoCacheKey&) noexcept;
};

PipelineCacheHash GetPipelineCacheHash(const ShaderModuleKey& key) noexcept;
PipelineCacheHash GetPipelineCacheHash(const PipelineLayoutCacheKey& key) noexcept;
PipelineCacheHash GetPipelineCacheHash(const GraphicsPsoCacheKey& key) noexcept;

}  // namespace radray

namespace std {

template <>
struct hash<radray::PipelineCacheHash> {
    size_t operator()(const radray::PipelineCacheHash& value) const noexcept;
};

template <>
struct hash<radray::ShaderModuleKey> {
    size_t operator()(const radray::ShaderModuleKey& key) const noexcept;
};

template <>
struct hash<radray::PipelineLayoutCacheKey> {
    size_t operator()(const radray::PipelineLayoutCacheKey& key) const noexcept;
};

template <>
struct hash<radray::GraphicsPsoCacheKey> {
    size_t operator()(const radray::GraphicsPsoCacheKey& key) const noexcept;
};

}  // namespace std

namespace radray {

/// Compiles and caches one GPU shader module per normalized ShaderModuleKey. When a cache
/// directory is supplied, bytecode and reflection are loaded and saved across process runs.
class ShaderModuleCache {
public:
    ShaderModuleCache(
        render::Device* device,
        render::Dxc* dxc,
        AssetManager* assetManager,
        string shaderSourceRoot,
        std::filesystem::path cacheDirectory = {}) noexcept;
    ShaderModuleCache(const ShaderModuleCache&) = delete;
    ShaderModuleCache(ShaderModuleCache&&) = delete;
    ShaderModuleCache& operator=(const ShaderModuleCache&) = delete;
    ShaderModuleCache& operator=(ShaderModuleCache&&) = delete;
    ~ShaderModuleCache() noexcept;

    Nullable<render::Shader*> GetOrCreate(const ShaderModuleKey& key) noexcept;

    bool FlushToDisk() noexcept;
    void Clear() noexcept;
    size_t GetCount() const noexcept { return _entries.size(); }

private:
    struct Entry {
        unique_ptr<render::Shader> Object{};
        vector<byte> Bytecode{};
        string ReflectionPayload{};
        std::optional<render::ShaderReflectionDesc> Reflection{};
        render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
        PipelineCacheHash BinaryHash{};
        PipelineCacheHash InterfaceHash{};
    };

    bool LoadFromDisk() noexcept;
    Nullable<render::Shader*> Materialize(const ShaderModuleKey& key, Entry& entry) noexcept;

    render::Device* _device{nullptr};
    render::Dxc* _dxc{nullptr};
    AssetManager* _assetManager{nullptr};
    string _shaderSourceRoot{};
    std::filesystem::path _cacheDirectory{};
    unordered_map<ShaderModuleKey, Entry> _entries{};
    unordered_map<const render::Shader*, ShaderModuleKey> _shaderKeys{};

    friend class GraphicsPipelineCache;
};

/// Owns both PipelineLayout and graphics PSO caches. Pipeline keys contain only stable shader
/// identities and owned descriptor values; native objects are materialized lazily after disk load.
class GraphicsPipelineCache {
public:
    GraphicsPipelineCache(
        render::Device* device,
        ShaderModuleCache* shaderModuleCache,
        std::filesystem::path cacheDirectory = {}) noexcept;
    GraphicsPipelineCache(const GraphicsPipelineCache&) = delete;
    GraphicsPipelineCache(GraphicsPipelineCache&&) = delete;
    GraphicsPipelineCache& operator=(const GraphicsPipelineCache&) = delete;
    GraphicsPipelineCache& operator=(GraphicsPipelineCache&&) = delete;
    ~GraphicsPipelineCache() noexcept;

    Nullable<render::PipelineLayout*> GetOrCreatePipelineLayout(
        const render::PipelineLayoutDescriptor& desc) noexcept;
    Nullable<render::GraphicsPipelineState*> GetOrCreateGraphicsPso(
        const render::GraphicsPipelineStateDescriptor& desc) noexcept;

    bool FlushToDisk() noexcept;
    void Clear() noexcept;

    size_t GetPipelineLayoutCount() const noexcept { return _pipelineLayouts.size(); }
    size_t GetGraphicsPsoCount() const noexcept { return _graphicsPsos.size(); }
    uint64_t GetPipelineLayoutHitCount() const noexcept { return _pipelineLayoutHits; }
    uint64_t GetPipelineLayoutMissCount() const noexcept { return _pipelineLayoutMisses; }
    uint64_t GetGraphicsPsoHitCount() const noexcept { return _graphicsPsoHits; }
    uint64_t GetGraphicsPsoMissCount() const noexcept { return _graphicsPsoMisses; }

private:
    struct PipelineLayoutEntry {
        unique_ptr<render::PipelineLayout> Object{};
    };

    struct GraphicsPsoEntry {
        unique_ptr<render::GraphicsPipelineState> Object{};
    };

    bool LoadFromDisk() noexcept;
    std::optional<PipelineLayoutCacheKey> BuildPipelineLayoutKey(
        const render::PipelineLayoutDescriptor& desc) const noexcept;
    std::optional<GraphicsPsoCacheKey> BuildGraphicsPsoKey(
        const render::GraphicsPipelineStateDescriptor& desc) const noexcept;
    void RegisterPipelineLayoutKey(const PipelineLayoutCacheKey& key) noexcept;
    void RegisterGraphicsPsoKey(const GraphicsPsoCacheKey& key) noexcept;

    render::Device* _device{nullptr};
    ShaderModuleCache* _shaderModuleCache{nullptr};
    std::filesystem::path _cacheDirectory{};
    unordered_map<PipelineLayoutCacheKey, PipelineLayoutEntry> _pipelineLayouts{};
    unordered_map<const render::PipelineLayout*, PipelineCacheHash> _pipelineLayoutKeys{};
    unordered_map<PipelineCacheHash, PipelineLayoutCacheKey> _pipelineLayoutHashes{};
    unordered_set<PipelineCacheHash> _pipelineLayoutHashCollisions{};
    unordered_map<GraphicsPsoCacheKey, GraphicsPsoEntry> _graphicsPsos{};
    unordered_map<PipelineCacheHash, GraphicsPsoCacheKey> _graphicsPsoHashes{};
    unordered_set<PipelineCacheHash> _graphicsPsoHashCollisions{};
    vector<unique_ptr<render::GraphicsPipelineState>> _uncachedGraphicsPsos{};
    vector<unique_ptr<render::PipelineLayout>> _uncachedPipelineLayouts{};
    uint64_t _pipelineLayoutHits{0};
    uint64_t _pipelineLayoutMisses{0};
    uint64_t _graphicsPsoHits{0};
    uint64_t _graphicsPsoMisses{0};
};

}  // namespace radray
