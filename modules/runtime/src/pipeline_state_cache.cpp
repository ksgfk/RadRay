#include <radray/runtime/pipeline_state_cache.h>

#include <algorithm>

#include <radray/render/rhi.h>

namespace radray {

namespace {

/// 把 program 变体的 stage 列表压成参与 key 比较的 (stage, 字节码 hash) 序列。
///
/// 用 ShaderHash 而非变体身份: 两个不同 variant 若投影到同一份字节码, 本就该命中同一个
/// PSO (见 pipeline_state_cache.h 的 key 说明)。
vector<std::pair<render::ShaderStage, ShaderHash>> MakeStageKeys(
    const ShaderProgramVariant& variant) noexcept {
    vector<std::pair<render::ShaderStage, ShaderHash>> keys;
    for (const ShaderProgramVariant::StageBlob& blob : variant.Stages()) {
        if (blob.Bytecode == nullptr) {
            continue;
        }
        keys.emplace_back(blob.Stage, blob.Bytecode->Key);
    }
    return keys;
}

}  // namespace

PipelineStateCache::PipelineStateCache(render::Device* device) noexcept
    : _device(device) {}

PipelineStateCache::~PipelineStateCache() noexcept {
    Clear();
}

Nullable<render::GraphicsPipelineState*> PipelineStateCache::GetOrCreateGraphics(
    const StreamingAssetRef<ShaderAsset>& asset,
    const GraphicsPipelineStateKey& key,
    const ShaderVariantKey& variant,
    render::ShaderBlobCategory category,
    ShaderAssetDiagnostic& outDiag) noexcept {
    if (key.Program == nullptr) {
        outDiag.Message = "GraphicsPipelineStateKey::Program is null";
        return nullptr;
    }
    if (key.CompatibleRenderPass == nullptr) {
        outDiag.Message = "GraphicsPipelineStateKey::CompatibleRenderPass is null";
        outDiag.PassName = key.Program->GetName();
        return nullptr;
    }

    // 【必须先解析变体再查缓存】: key 的字节码部分是 ShaderHash, 只有解析过才知道。
    // 这不额外花钱 —— program 自己缓存字节码, 命中时 GetOrCreateVariant 只是一次查表。
    Nullable<const ShaderProgramVariant*> resolved =
        key.Program->GetOrCreateVariant(variant, category, outDiag);
    if (!resolved.HasValue()) {
        return nullptr;
    }
    const vector<std::pair<render::ShaderStage, ShaderHash>> stageKeys = MakeStageKeys(*resolved);

    for (GraphicsEntry& entry : _graphics) {
        if (entry.Program == key.Program &&
            entry.CompatibleRenderPass == key.CompatibleRenderPass &&
            entry.Primitive == key.Primitive &&
            entry.DepthStencil == key.DepthStencil &&
            entry.MultiSample == key.MultiSample &&
            entry.StageKeys == stageKeys &&
            entry.ColorTargets.size() == key.ColorTargets.size() &&
            std::equal(entry.ColorTargets.begin(), entry.ColorTargets.end(), key.ColorTargets.begin())) {
            ++_graphicsHits;
            return entry.Object.get();
        }
    }

    ++_graphicsMisses;
    if (_device == nullptr) {
        outDiag.Message = "PipelineStateCache has no device";
        outDiag.PassName = key.Program->GetName();
        return nullptr;
    }

    render::PipelineLayout* layout = key.Program->GetPipelineLayout().Get();
    if (layout == nullptr) {
        outDiag.Message = "pass has no pipeline layout";
        outDiag.PassName = key.Program->GetName();
        return nullptr;
    }
    std::optional<render::VertexInputState> vertexInput = key.Program->GetVertexInputState();
    if (!vertexInput.has_value()) {
        outDiag.Message = "pass has no vertex input state, cannot build a graphics pipeline";
        outDiag.PassName = key.Program->GetName();
        return nullptr;
    }

    // 【Shader 是局部量】: 见 pipeline_state_cache.h 的文件头。作用域到本函数返回为止,
    // 只需活过 CreateGraphicsPipelineState 那一次调用。
    vector<unique_ptr<render::Shader>> shaders;
    shaders.reserve(resolved->Stages().size());
    std::optional<render::ShaderEntry> vsEntry;
    std::optional<render::ShaderEntry> psEntry;
    for (const ShaderProgramVariant::StageBlob& blob : resolved->Stages()) {
        if (blob.Bytecode == nullptr) {
            continue;
        }
        auto shaderResult = _device->CreateShader(MakeShaderDescriptor(*blob.Bytecode));
        if (!shaderResult.HasValue()) {
            outDiag.Message = "CreateShader failed";
            outDiag.PassName = key.Program->GetName();
            outDiag.Stage = blob.Stage;
            return nullptr;
        }
        shaders.push_back(shaderResult.Release());
        const render::ShaderEntry entry{shaders.back().get(), blob.EntryPoint};
        switch (blob.Stage) {
            case render::ShaderStage::Vertex: vsEntry = entry; break;
            case render::ShaderStage::Pixel: psEntry = entry; break;
            default:
                outDiag.Message = "graphics pipeline only supports vertex and pixel stages";
                outDiag.PassName = key.Program->GetName();
                outDiag.Stage = blob.Stage;
                return nullptr;
        }
    }
    if (!vsEntry.has_value()) {
        outDiag.Message = "graphics pipeline requires a vertex stage";
        outDiag.PassName = key.Program->GetName();
        return nullptr;
    }

    const render::GraphicsPipelineStateDescriptor psoDesc{
        .PipelineLayout = layout,
        .VS = vsEntry,
        .PS = psEntry,
        .VertexInput = vertexInput.value(),
        .Primitive = key.Primitive,
        .DepthStencil = key.DepthStencil,
        .MultiSample = key.MultiSample,
        .ColorTargets = key.ColorTargets,
        .CompatibleRenderPass = key.CompatibleRenderPass};
    auto psoResult = _device->CreateGraphicsPipelineState(psoDesc);
    if (!psoResult.HasValue()) {
        outDiag.Message = "CreateGraphicsPipelineState failed";
        outDiag.PassName = key.Program->GetName();
        return nullptr;
    }
    auto pso = psoResult.Release();
    render::GraphicsPipelineState* result = pso.get();
    _graphics.push_back(GraphicsEntry{
        .Program = key.Program,
        .CompatibleRenderPass = key.CompatibleRenderPass,
        .StageKeys = stageKeys,
        .Primitive = key.Primitive,
        .DepthStencil = key.DepthStencil,
        .MultiSample = key.MultiSample,
        .ColorTargets = vector<render::ColorTargetState>{key.ColorTargets.begin(), key.ColorTargets.end()},
        .Owner = asset.Get(),
        .Ref = asset,
        .Object = std::move(pso)});
    return result;
}

uint32_t PipelineStateCache::RemovePipelineStatesUsing(const ShaderAsset* asset) noexcept {
    if (asset == nullptr) {
        return 0;
    }
    const size_t before = _graphics.size();
    auto removed = std::remove_if(
        _graphics.begin(),
        _graphics.end(),
        [asset](const GraphicsEntry& entry) noexcept { return entry.Owner == asset; });
    _graphics.erase(removed, _graphics.end());
    return static_cast<uint32_t>(before - _graphics.size());
}

void PipelineStateCache::Clear() noexcept {
    _graphics.clear();
}

}  // namespace radray
