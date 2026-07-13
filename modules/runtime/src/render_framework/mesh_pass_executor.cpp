#include <radray/runtime/render_framework/mesh_pass_executor.h>

#include <cstring>
#include <algorithm>
#include <tuple>
#include <utility>

#include <radray/logger.h>
#include <radray/hash.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/shader_default_resource_library.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/render_queue.h>

namespace radray {

namespace {

// 从 pass 的拥有式顶点布局构造 render::VertexBufferLayout (span 指向 pass 内 element)。
// element span 借用 passDesc.VertexLayouts[i].Elements 的存储, 故 out 只在 passDesc 存活期间有效。
void BuildVertexLayouts(
    const ShaderPassDesc& passDesc,
    vector<render::VertexBufferLayout>& out) noexcept {
    out.clear();
    out.reserve(passDesc.VertexLayouts.size());
    for (const OwningVertexBufferLayout& l : passDesc.VertexLayouts) {
        render::VertexBufferLayout vl{};
        vl.ArrayStride = l.ArrayStride;
        vl.StepMode = l.StepMode;
        vl.Elements = std::span<const render::VertexElement>{l.Elements.data(), l.Elements.size()};
        out.emplace_back(vl);
    }
}

struct RetiredMaterialConstants {
    shared_ptr<MaterialConstantPool> Pool;
    vector<MaterialConstantPool::Allocation> Allocations;

    RetiredMaterialConstants(
        shared_ptr<MaterialConstantPool> pool,
        vector<MaterialConstantPool::Allocation>&& allocations) noexcept
        : Pool(std::move(pool)), Allocations(std::move(allocations)) {}

    ~RetiredMaterialConstants() noexcept {
        if (Pool == nullptr) {
            return;
        }
        for (const auto& allocation : Allocations) {
            Pool->Release(allocation);
        }
    }
};

const render::VertexElement* FindPositionElement(
    const OwningVertexBufferLayout& layout) noexcept {
    auto element = std::ranges::find_if(
        layout.Elements,
        [](const render::VertexElement& value) noexcept {
            return value.Semantic == "POSITION" && value.SemanticIndex == 0;
        });
    return element != layout.Elements.end() ? &*element : nullptr;
}

bool HasCompatiblePositionLayout(
    const ShaderPassDesc& source,
    const ShaderPassDesc& fallback) noexcept {
    const size_t count = std::min(source.VertexLayouts.size(), fallback.VertexLayouts.size());
    for (size_t i = 0; i < count; ++i) {
        const OwningVertexBufferLayout& sourceLayout = source.VertexLayouts[i];
        const OwningVertexBufferLayout& fallbackLayout = fallback.VertexLayouts[i];
        const render::VertexElement* sourcePosition = FindPositionElement(sourceLayout);
        const render::VertexElement* fallbackPosition = FindPositionElement(fallbackLayout);
        if (sourcePosition != nullptr && fallbackPosition != nullptr &&
            sourceLayout.ArrayStride == fallbackLayout.ArrayStride &&
            sourceLayout.StepMode == fallbackLayout.StepMode &&
            sourcePosition->Offset == fallbackPosition->Offset &&
            sourcePosition->Format == fallbackPosition->Format &&
            sourcePosition->Location == fallbackPosition->Location) {
            return true;
        }
    }
    return false;
}

ShaderVariantKey BuildDiagnosticVariantKey(
    const DrawItem& item,
    std::span<const std::string_view> globalKeywords,
    render::Device* device) {
    ShaderVariantKey key{};
    ShaderAsset* shader = item.Material != nullptr ? item.Material->Shader.Get() : nullptr;
    if (shader == nullptr) {
        return key;
    }
    key.ProgramId = shader->GetProgramId();
    key.PassIndex = item.PassIndex;
    key.Backend = device != nullptr
                      ? static_cast<uint32_t>(device->GetBackend())
                      : 0u;
    key.ShaderModel = static_cast<uint32_t>(render::HlslShaderModel::SM60);

    vector<std::string_view> enabled;
    enabled.reserve(item.Material->EnabledKeywords.size() + globalKeywords.size());
    for (const string& keyword : item.Material->EnabledKeywords) {
        enabled.push_back(keyword);
    }
    enabled.insert(enabled.end(), globalKeywords.begin(), globalKeywords.end());
    key.KeywordBitmask = shader->GetKeywords().Project(enabled);
    if (item.PassIndex < shader->GetPasses().size()) {
        key.KeywordBitmask &= shader->GetPasses()[item.PassIndex].VariantKeywordMask;
    }
    return key;
}

}  // namespace

MeshPassExecutor::MeshPassExecutor(
    render::Device* device,
    ShaderVariantLibrary* variantCache,
    GraphicsPipelineStateLibrary* psoCache,
    SamplerCache* samplerCache,
    std::string perObjectCBufferName,
    Nullable<ShaderDefaultResourceLibrary*> defaultResources,
    shared_ptr<const MaterialRenderSnapshot> errorMaterial) noexcept
    : _device(device),
      _variantCache(variantCache),
      _psoCache(psoCache),
      _samplerCache(samplerCache),
      _defaultResources(defaultResources.Get()),
      _errorMaterial(std::move(errorMaterial)),
      _perObjectName(std::move(perObjectCBufferName)) {}

void MeshPassExecutor::SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept {
    if (!_viewName.empty() && _viewName != viewCBufferName) {
        _viewParameters.ClearConstant(_viewName);
    }
    _viewName.assign(viewCBufferName.begin(), viewCBufferName.end());
    _viewData.assign(data.begin(), data.end());
    if (!_viewName.empty() && !_viewData.empty()) {
        _viewParameters.SetConstant(_viewName, _viewData);
    } else if (!_viewName.empty()) {
        _viewParameters.ClearConstant(_viewName);
    }
    _viewBindingValid = false;
}

void MeshPassExecutor::SetViewParameter(
    std::string_view name,
    std::span<const byte> data) {
    _viewParameters.SetConstant(name, data);
    if (name == _viewName) {
        _viewData.assign(data.begin(), data.end());
        _viewBindingValid = false;
    }
}

void MeshPassExecutor::SetViewResource(
    std::string_view name,
    render::ResourceView* resource) noexcept {
    if (resource != nullptr) {
        _viewParameters.SetResource(name, resource);
    } else {
        _viewParameters.ClearResource(name);
    }
}

void MeshPassExecutor::SetViewSampler(
    std::string_view name,
    render::Sampler* sampler) noexcept {
    if (sampler != nullptr) {
        _viewParameters.SetSampler(name, sampler);
    } else {
        _viewParameters.ClearSampler(name);
    }
}

void MeshPassExecutor::SetPassConstant(
    std::string_view name,
    std::span<const byte> data) {
    _passParameters.SetConstant(name, data);
}

void MeshPassExecutor::SetPassResource(
    std::string_view name,
    render::ResourceView* resource) noexcept {
    if (resource != nullptr) {
        _passParameters.SetResource(name, resource);
    } else {
        _passParameters.ClearResource(name);
    }
}

void MeshPassExecutor::SetPassSampler(
    std::string_view name,
    render::Sampler* sampler) noexcept {
    if (sampler != nullptr) {
        _passParameters.SetSampler(name, sampler);
    } else {
        _passParameters.ClearSampler(name);
    }
}

void MeshPassExecutor::SetGlobalTexture(std::string_view name, render::TextureView* view) noexcept {
    SetPassResource(name, view);
}

void MeshPassExecutor::SetGlobalSampler(std::string_view name, render::Sampler* sampler) noexcept {
    SetPassSampler(name, sampler);
}

void MeshPassExecutor::EnableGlobalKeyword(std::string_view name) noexcept {
    for (std::string_view kw : _globalKeywords) {
        if (kw == name) {
            return;
        }
    }
    _globalKeywords.push_back(name);
}

void MeshPassExecutor::ClearGlobals() noexcept {
    _passParameters.Clear();
    _globalKeywords.clear();
}

Nullable<const DynamicCBufferArena::Allocation*> MeshPassExecutor::EnsureViewBinding(
    FrameResources& resources) noexcept {
    if (_viewBindingValid && _viewGeneration == resources.Generation) {
        return &_viewAllocation;
    }
    if (_viewData.empty()) {
        return nullptr;
    }

    auto alloc = resources.ViewArena.Allocate(_viewData.size());
    if (alloc.Target == nullptr || alloc.Mapped == nullptr) {
        RADRAY_ERR_LOG("MeshPassExecutor: per-view cbuffer allocation failed");
        return nullptr;
    }
    std::memcpy(alloc.Mapped, _viewData.data(), _viewData.size());
    _viewAllocation = alloc;
    _viewBindingValid = true;
    _viewGeneration = resources.Generation;
    return &_viewAllocation;
}

Nullable<const DynamicCBufferArena::Allocation*> MeshPassExecutor::EnsureObjectBinding(
    FrameResources& resources,
    PrimitiveSceneProxy* proxy) noexcept {
    for (const FrameObjectBinding& binding : resources.ObjectBindings) {
        if (binding.Proxy == proxy) {
            return &binding.Allocation;
        }
    }
    PerObjectConstants constants{};
    const Eigen::Matrix4f matrix = proxy->GetLocalToWorld();
    std::memcpy(constants.ObjectToWorld, matrix.data(), sizeof(constants.ObjectToWorld));
    auto allocation = resources.PerObjectArena.Allocate(sizeof(constants));
    if (allocation.Target == nullptr || allocation.Mapped == nullptr) {
        return nullptr;
    }
    std::memcpy(allocation.Mapped, &constants, sizeof(constants));
    FrameObjectBinding& binding = resources.ObjectBindings.emplace_back(FrameObjectBinding{
        .Proxy = proxy,
        .Allocation = allocation});
    return &binding.Allocation;
}


Nullable<const MeshPassExecutor::ResolvedDrawState*> MeshPassExecutor::ResolveDrawState(
    const DrawItem& item,
    FrameResources& resources) noexcept {
    for (ResolvedDrawState& state : _resolvedDrawStates) {
        if (!(state.MaterialKey == item.Material->BindingKey) || state.PassIndex != item.PassIndex ||
            state.RenderPass != _renderPass || state.GlobalKeywords.size() != _globalKeywords.size()) {
            continue;
        }
        bool sameKeywords = true;
        for (size_t i = 0; i < _globalKeywords.size(); ++i) {
            if (state.GlobalKeywords[i] != _globalKeywords[i]) {
                sameKeywords = false;
                break;
            }
        }
        if (sameKeywords) {
            state.LastUsedFrame = _frameSerial;
            ++resources.Counters.DrawStateCacheHits;
            return &state;
        }
    }

    ++resources.Counters.DrawStateCacheMisses;
    const ShaderVariantLibraryStats variantStatsBefore = _variantCache->GetStats();
    auto variantOpt = item.Material->ResolveVariant(
        *_variantCache, item.PassIndex, std::span<const std::string_view>{_globalKeywords});
    const ShaderVariantLibraryStats variantStatsAfter = _variantCache->GetStats();
    resources.Counters.ShaderVariantCacheHits +=
        variantStatsAfter.VariantHits - variantStatsBefore.VariantHits;
    resources.Counters.ShaderVariantCacheMisses +=
        variantStatsAfter.VariantMisses - variantStatsBefore.VariantMisses;
    if (!variantOpt.HasValue()) {
        return nullptr;
    }
    const CompiledShaderVariant* variant = variantOpt.Get();
    const uint64_t psoHitsBefore = _psoCache->GetHitCount();
    const uint64_t psoMissesBefore = _psoCache->GetMissCount();
    auto psoOpt = ResolvePso(item, *variant);
    resources.Counters.PipelineCacheHits += _psoCache->GetHitCount() - psoHitsBefore;
    resources.Counters.PipelineCacheMisses += _psoCache->GetMissCount() - psoMissesBefore;
    if (!psoOpt.HasValue()) {
        return nullptr;
    }

    vector<string> globalKeywords;
    globalKeywords.reserve(_globalKeywords.size());
    for (std::string_view keyword : _globalKeywords) {
        globalKeywords.emplace_back(keyword);
    }
    ResolvedDrawState& state = _resolvedDrawStates.emplace_back(ResolvedDrawState{
        .MaterialKey = item.Material->BindingKey,
        .PassIndex = item.PassIndex,
        .GlobalKeywords = std::move(globalKeywords),
        .RenderPass = _renderPass,
        .Variant = variant,
        .Pso = psoOpt.Get(),
        .LastUsedFrame = _frameSerial});
    return &state;
}

Nullable<render::GraphicsPipelineState*> MeshPassExecutor::ResolvePso(
    const DrawItem& item,
    const CompiledShaderVariant& variant) noexcept {
    const ShaderAsset* shader = item.Material->Shader.Get();
    if (shader == nullptr || _renderPass == nullptr) {
        return nullptr;
    }
    const auto& passes = shader->GetPasses();
    if (item.PassIndex >= passes.size()) {
        RADRAY_ERR_LOG("MeshPassExecutor: pass index {} out of range", item.PassIndex);
        return nullptr;
    }
    const ShaderPassDesc& passDesc = passes[item.PassIndex];

    vector<render::VertexBufferLayout> vertexLayouts;
    BuildVertexLayouts(passDesc, vertexLayouts);

    // 材质对 PSO 固定功能状态的覆盖 (blend / zwrite / cull, 对应 Unity 的 [_Prop] 渲染状态)。
    // 以 pass 的基线固定状态为底, 材质覆盖叠加其上; 不覆盖的字段沿用基线。
    const MaterialRenderState* rs = passDesc.AllowMaterialRenderStateOverrides
                                        ? &item.Material->RenderState
                                        : nullptr;

    render::GraphicsPipelineStateDescriptor desc{};
    desc.PipelineLayout = variant.Layout;
    desc.VS = render::ShaderEntry{.Target = variant.VS, .EntryPoint = passDesc.VertexEntry};
    desc.PS = render::ShaderEntry{.Target = variant.PS, .EntryPoint = passDesc.PixelEntry};
    desc.VertexLayouts = std::span<const render::VertexBufferLayout>{vertexLayouts.data(), vertexLayouts.size()};

    // Primitive: 覆盖 cull。
    desc.Primitive = passDesc.Primitive;
    if (rs != nullptr && rs->Cull.has_value()) {
        desc.Primitive.Cull = rs->Cull.value();
    }

    // DepthStencil: 覆盖 zwrite (仅当 pass 有 depth-stencil 段)。
    desc.DepthStencil = passDesc.DepthStencil;
    if (rs != nullptr && rs->DepthWrite.has_value() && desc.DepthStencil.has_value()) {
        desc.DepthStencil->DepthWriteEnable = rs->DepthWrite.value();
    }

    desc.MultiSample = passDesc.MultiSample;

    // ColorTargets: 覆盖每个 target 的 blend。passDesc.ColorTargets 为共享只读, 故本地拷贝再改,
    // 拷贝存活至 GetOrCreate 返回 (PSO 缓存据展平 key 去重, 不持有 desc)。
    if (rs != nullptr && rs->OverrideBlend) {
        _colorTargetScratch.assign(passDesc.ColorTargets.begin(), passDesc.ColorTargets.end());
        for (render::ColorTargetState& ct : _colorTargetScratch) {
            ct.Blend = rs->Blend;  // 有值=覆盖为开; nullopt=强制关闭混合
        }
        desc.ColorTargets = std::span<const render::ColorTargetState>{_colorTargetScratch.data(), _colorTargetScratch.size()};
    } else {
        desc.ColorTargets = std::span<const render::ColorTargetState>{passDesc.ColorTargets.data(), passDesc.ColorTargets.size()};
    }
    desc.CompatibleRenderPass = _renderPass;
    return _psoCache->GetOrCreate(desc);
}

Nullable<const MeshDrawCommandTemplate*> MeshPassExecutor::GetOrCreateCommandTemplate(
    const DrawItem& item,
    const ResolvedDrawState& state,
    const MeshDrawArgs& args,
    FrameResources& resources) noexcept {
    const auto& geometry = *args.Geometry;
    for (const auto& candidate : _commandTemplates) {
        if (candidate->Proxy == item.Proxy &&
            candidate->ProxyGeneration == item.Proxy->GetGeneration() &&
            candidate->SectionIndex == item.SectionIndex &&
            candidate->PassIndex == item.PassIndex &&
            candidate->VariantKey == state.Variant->Key &&
            candidate->MaterialKey == item.Material->BindingKey &&
            candidate->Pipeline == state.Pso &&
            candidate->Geometry.Vbv.Target == geometry.Vbv.Target &&
            candidate->Geometry.Vbv.Offset == geometry.Vbv.Offset &&
            candidate->Geometry.Ibv.Target == geometry.Ibv.Target &&
            candidate->Geometry.Ibv.Offset == geometry.Ibv.Offset &&
            candidate->FirstIndex == args.FirstIndex &&
            candidate->IndexCount == args.IndexCount &&
            candidate->VertexOffset == args.VertexOffset) {
            candidate->LastUsedFrame = _frameSerial;
            ++resources.Counters.DrawCommandTemplateHits;
            return candidate.get();
        }
    }

    ++resources.Counters.DrawCommandTemplateMisses;
    auto command = make_unique<MeshDrawCommandTemplate>();
    command->Proxy = item.Proxy;
    command->ProxyGeneration = item.Proxy->GetGeneration();
    command->SectionIndex = item.SectionIndex;
    command->PassIndex = item.PassIndex;
    command->VariantKey = state.Variant->Key;
    command->MaterialKey = item.Material->BindingKey;
    command->Pipeline = state.Pso;
    command->Geometry = geometry;
    command->FirstIndex = args.FirstIndex;
    command->IndexCount = args.IndexCount;
    command->VertexOffset = args.VertexOffset;
    command->PipelineId = _psoCache->GetId(state.Pso);
    const void* pages[] = {geometry.Vbv.Target, geometry.Ibv.Target};
    command->GeometryPageId = HashData64(pages, sizeof(pages));
    command->LastUsedFrame = _frameSerial;
    auto* result = command.get();
    _commandTemplates.push_back(std::move(command));
    return result;
}

void MeshPassExecutor::ReportDiagnostic(
    const DrawItem& item,
    const ShaderBindingDiagnostic& diagnostic) noexcept {
    ShaderBindingDiagnostic normalized = diagnostic;
    ShaderAsset* shader = item.Material != nullptr ? item.Material->Shader.Get() : nullptr;
    if (normalized.ProgramId.IsEmpty() && shader != nullptr) {
        normalized.ProgramId = shader->GetProgramId();
    }
    normalized.PassIndex = item.PassIndex;
    if (std::ranges::find(_reportedDiagnostics, normalized) != _reportedDiagnostics.end()) {
        return;
    }
    _reportedDiagnostics.push_back(normalized);
    const string group = normalized.Group.has_value()
                             ? fmt::format("{}", *normalized.Group)
                             : string{"-"};
    const string binding = normalized.Binding.has_value()
                               ? fmt::format("{}", *normalized.Binding)
                               : string{"-"};
    RADRAY_ERR_LOG(
        "MeshPassExecutor binding invalid: shader={} pass={} variant=0x{:x} group={} binding={} reason={}",
        normalized.ProgramId,
        normalized.PassIndex,
        normalized.VariantKey.KeywordBitmask,
        group,
        binding,
        normalized.Reason);
}

bool MeshPassExecutor::CompileErrorFallback(
    const DrawItem& item,
    FrameResources& resources,
    std::string_view reason) noexcept {
    ShaderBindingDiagnostic diagnostic{};
    diagnostic.PassIndex = item.PassIndex;
    diagnostic.VariantKey = BuildDiagnosticVariantKey(item, _globalKeywords, _device);
    diagnostic.Reason = string{reason};

    ShaderAsset* sourceShader = item.Material != nullptr ? item.Material->Shader.Get() : nullptr;
    if (sourceShader == nullptr || item.PassIndex >= sourceShader->GetPasses().size()) {
        diagnostic.Reason = fmt::format("{}; source shader pass is unavailable", reason);
        ReportDiagnostic(item, diagnostic);
        return false;
    }
    diagnostic.ProgramId = sourceShader->GetProgramId();
    const ShaderPassDesc& sourcePass = sourceShader->GetPasses()[item.PassIndex];
    if (sourcePass.ColorTargets.empty()) {
        diagnostic.Reason = fmt::format("{}; depth-only pass has no visible error fallback", reason);
        ReportDiagnostic(item, diagnostic);
        return false;
    }
    if (_errorMaterial == nullptr || _errorMaterial.get() == item.Material.get()) {
        diagnostic.Reason = fmt::format("{}; error material is unavailable", reason);
        ReportDiagnostic(item, diagnostic);
        return false;
    }
    ShaderAsset* errorShader = _errorMaterial->Shader.Get();
    const auto errorPassIndex = errorShader != nullptr
                                    ? errorShader->FindPassByTag(sourcePass.PassTag)
                                    : std::nullopt;
    if (!errorPassIndex.has_value()) {
        diagnostic.Reason = fmt::format(
            "{}; error material has no pass tag '{}'",
            reason,
            sourcePass.PassTag);
        ReportDiagnostic(item, diagnostic);
        return false;
    }
    const ShaderPassDesc& errorPass = errorShader->GetPasses()[*errorPassIndex];
    if (!HasCompatiblePositionLayout(sourcePass, errorPass)) {
        diagnostic.Reason = fmt::format(
            "{}; vertex layout has no POSITION compatible with the error material",
            reason);
        ReportDiagnostic(item, diagnostic);
        return false;
    }

    DrawItem fallback = item;
    fallback.Material = _errorMaterial;
    fallback.PassIndex = *errorPassIndex;
    if (!CompileCommandInternal(fallback, resources, false, true)) {
        diagnostic.Reason = fmt::format("{}; error material compilation also failed", reason);
        ReportDiagnostic(item, diagnostic);
        return false;
    }
    _commands.back().SortMaterialKey = item.Material->BindingKey;
    return true;
}

bool MeshPassExecutor::CompileCommand(const DrawItem& item, FrameResources& resources) noexcept {
    return CompileCommandInternal(item, resources, true, false);
}

bool MeshPassExecutor::CompileCommandInternal(
    const DrawItem& item,
    FrameResources& resources,
    bool allowErrorFallback,
    bool isErrorFallback) noexcept {
    if (item.Material == nullptr || item.Proxy == nullptr) {
        return false;
    }

    // 1-2. 同一 material/pass/global-keyword 状态跨帧复用，快照失效时由 BeginFrame 回收。
    auto stateOpt = ResolveDrawState(item, resources);
    if (!stateOpt.HasValue()) {
        ++resources.Counters.BindingResolutionFailures;
        ShaderBindingDiagnostic diagnostic{};
        if (ShaderAsset* shader = item.Material->Shader.Get(); shader != nullptr) {
            diagnostic.ProgramId = shader->GetProgramId();
        }
        diagnostic.PassIndex = item.PassIndex;
        diagnostic.VariantKey = BuildDiagnosticVariantKey(item, _globalKeywords, _device);
        diagnostic.Reason = "shader variant or pipeline creation failed";
        ReportDiagnostic(item, diagnostic);
        return allowErrorFallback
                   ? CompileErrorFallback(item, resources, "shader variant or pipeline creation failed")
                   : false;
    }
    const ResolvedDrawState& state = *stateOpt.Get();
    const CompiledShaderVariant& variant = *state.Variant;

    // 3. 取几何 (VB/IB + 索引范围)。
    MeshDrawArgs args = item.Proxy->GetDrawArgs(item.SectionIndex);
    if (args.Geometry == nullptr) {
        RADRAY_ERR_LOG("MeshPassExecutor: no geometry for section {}", item.SectionIndex);
        return false;
    }
    const GpuMesh::DrawData* draw = args.Geometry;
    if (draw->Ibv.Target == nullptr || draw->Ibv.Stride == 0) {
        RADRAY_ERR_LOG("MeshPassExecutor: section {} has no index buffer", item.SectionIndex);
        return false;
    }
    if (args.IndexCount == 0) {
        RADRAY_ERR_LOG("MeshPassExecutor: section {} has zero index count", item.SectionIndex);
        return false;
    }

    BindingResolveResult bindings = ResolveBindingGroups(item, variant, resources);
    if (bindings.Status != BindingResolveStatus::Ready) {
        if (bindings.Status == BindingResolveStatus::Invalid) {
            ReportDiagnostic(item, bindings.Diagnostic);
            if (allowErrorFallback) {
                return CompileErrorFallback(item, resources, bindings.Diagnostic.Reason);
            }
        }
        return false;
    }

    const auto retainedSnapshot = std::static_pointer_cast<const void>(item.Material);
    if (std::ranges::none_of(
            resources.RetainedObjects,
            [&](const shared_ptr<const void>& retained) noexcept {
                return retained.get() == retainedSnapshot.get();
            })) {
        resources.RetainedObjects.push_back(retainedSnapshot);
    }

    auto commandTemplate = GetOrCreateCommandTemplate(
        item,
        state,
        args,
        resources);
    if (!commandTemplate.HasValue()) {
        return false;
    }
    _commands.push_back(MeshDrawCommand{
        .Template = commandTemplate.Get(),
        .PerObjectBufferPage = bindings.ObjectBufferPage,
        .BindingGroups = std::move(bindings.Groups),
        .IsErrorFallback = isErrorFallback,
        .SortMaterialKey = item.Material->BindingKey,
        .RenderQueue = item.RenderQueue,
        .ViewDistance = item.ViewDistance});
    return true;
}

bool MeshPassExecutor::RecordCommand(
    render::GraphicsCommandEncoder* encoder,
    const MeshDrawCommand& command,
    FrameResources& resources,
    uint32_t instanceCount) noexcept {
    if (encoder == nullptr || command.Template == nullptr || instanceCount == 0) {
        return false;
    }
    const MeshDrawCommandTemplate& draw = *command.Template;
    if (_recorder.Pipeline != draw.Pipeline) {
        encoder->BindGraphicsPipelineState(draw.Pipeline);
        _recorder.Pipeline = draw.Pipeline;
        _recorder.GenericGroups.clear();
        ++resources.Counters.PipelineBinds;
    }
    if (draw.Geometry.Vbv.Target != nullptr) {
        const bool changed = !_recorder.HasVertex ||
                             _recorder.Vertex.Target != draw.Geometry.Vbv.Target ||
                             _recorder.Vertex.Offset != draw.Geometry.Vbv.Offset ||
                             _recorder.Vertex.Size != draw.Geometry.Vbv.Size;
        if (changed) {
            encoder->BindVertexBuffer(std::span{&draw.Geometry.Vbv, 1});
            _recorder.Vertex = draw.Geometry.Vbv;
            _recorder.HasVertex = true;
        }
    }
    const bool indexChanged = !_recorder.HasIndex ||
                              _recorder.Index.Target != draw.Geometry.Ibv.Target ||
                              _recorder.Index.Offset != draw.Geometry.Ibv.Offset ||
                              _recorder.Index.Stride != draw.Geometry.Ibv.Stride;
    if (indexChanged) {
        encoder->BindIndexBuffer(draw.Geometry.Ibv);
        _recorder.Index = draw.Geometry.Ibv;
        _recorder.HasIndex = true;
    }
    for (const MeshDrawCommand::BindingGroupState& binding : command.BindingGroups) {
        auto recorded = std::ranges::find(
            _recorder.GenericGroups,
            binding.GroupIndex,
            &RecorderState::GenericGroup::GroupIndex);
        if (recorded == _recorder.GenericGroups.end() ||
            recorded->Group != binding.Group ||
            recorded->DynamicOffsets != binding.DynamicOffsets) {
            encoder->BindBindingGroup(
                binding.GroupIndex,
                binding.Group,
                binding.DynamicOffsets);
            if (recorded == _recorder.GenericGroups.end()) {
                _recorder.GenericGroups.push_back(RecorderState::GenericGroup{
                    .GroupIndex = binding.GroupIndex,
                    .Group = binding.Group,
                    .DynamicOffsets = binding.DynamicOffsets});
            } else {
                recorded->Group = binding.Group;
                recorded->DynamicOffsets = binding.DynamicOffsets;
            }
            ++resources.Counters.DescriptorGroupBinds;
            resources.Counters.DynamicOffsetBinds += binding.DynamicOffsets.size();
        }
    }
    encoder->DrawIndexed(draw.IndexCount, instanceCount, draw.FirstIndex, draw.VertexOffset, 0);
    ++resources.Counters.Draws;
    if (command.IsErrorFallback) {
        ++resources.Counters.ErrorFallbackDraws;
    }
    resources.Counters.DrawInstances += instanceCount;
    return true;
}

bool MeshPassExecutor::SubmitItem(
    render::GraphicsCommandEncoder* encoder,
    const DrawItem& item,
    FrameResources& resources) noexcept {
    PrepareFrame(resources);
    _commands.clear();
    return CompileCommand(item, resources) && RecordCommand(encoder, _commands.front(), resources, 1);
}

void MeshPassExecutor::RetireStaleMaterialBindings(FrameResources& resources) noexcept {
    for (auto it = _genericMaterialBindings.begin(); it != _genericMaterialBindings.end();) {
        if (it->LastUsedFrame + 1 >= _frameSerial) {
            ++it;
            continue;
        }
        if (it->Snapshot != nullptr) {
            resources.RetainedObjects.push_back(
                std::static_pointer_cast<const void>(it->Snapshot));
        }
        resources.RetainedObjects.push_back(
            std::static_pointer_cast<const void>(_materialDescriptorPool));
        if (!it->ConstantAllocations.empty()) {
            auto retiredConstants = make_shared<RetiredMaterialConstants>(
                _materialConstantPool, std::move(it->ConstantAllocations));
            resources.RetainedObjects.push_back(
                std::static_pointer_cast<const void>(std::move(retiredConstants)));
        }
        resources.RetireList.emplace_back(std::move(it->Group));
        it = _genericMaterialBindings.erase(it);
    }
    std::erase_if(_resolvedDrawStates, [this](const ResolvedDrawState& state) noexcept {
        return state.LastUsedFrame + 1 < _frameSerial;
    });
    std::erase_if(_commandTemplates, [this](const auto& command) noexcept {
        return command->LastUsedFrame + 1 < _frameSerial;
    });
}

void MeshPassExecutor::PrepareFrame(FrameResources& resources) noexcept {
    if (_lastFrameResources == &resources && _lastFrameGeneration == resources.Generation) {
        return;
    }
    _lastFrameResources = &resources;
    _lastFrameGeneration = resources.Generation;
    ++_frameSerial;
    RetireStaleMaterialBindings(resources);
    _viewAllocation = {};
    _viewBindingValid = false;
    _recorder = {};
}

uint32_t MeshPassExecutor::Execute(
    render::GraphicsCommandEncoder* encoder,
    const DrawList& list,
    FrameResources& resources,
    uint32_t instanceCount) noexcept {
    if (instanceCount == 0) {
        return 0;
    }
    PrepareFrame(resources);
    if (_recorder.Encoder != encoder) {
        _recorder = {};
        _recorder.Encoder = encoder;
    }
    _commands.clear();
    _commands.reserve(list.Size());
    for (const DrawItem& item : list.Items()) {
        CompileCommand(item, resources);
    }
    resources.FlushHostWrites();
    if (_materialConstantPool != nullptr) {
        _materialConstantPool->FlushHostWrites();
    }
    if (list.GetSortMode() == DrawList::SortMode::Opaque) {
        std::stable_sort(_commands.begin(), _commands.end(), [](const MeshDrawCommand& a, const MeshDrawCommand& b) {
            return std::tie(
                       a.RenderQueue,
                       a.Template->PipelineId,
                       a.SortMaterialKey.Lo,
                       a.SortMaterialKey.Hi,
                       a.PerObjectBufferPage,
                       a.Template->GeometryPageId,
                       a.ViewDistance) <
                   std::tie(
                       b.RenderQueue,
                       b.Template->PipelineId,
                       b.SortMaterialKey.Lo,
                       b.SortMaterialKey.Hi,
                       b.PerObjectBufferPage,
                       b.Template->GeometryPageId,
                       b.ViewDistance);
        });
    } else if (list.GetSortMode() == DrawList::SortMode::Transparent) {
        std::stable_sort(_commands.begin(), _commands.end(), [](const MeshDrawCommand& a, const MeshDrawCommand& b) {
            if (a.RenderQueue != b.RenderQueue) {
                return a.RenderQueue < b.RenderQueue;
            }
            if (a.ViewDistance != b.ViewDistance) {
                return a.ViewDistance > b.ViewDistance;
            }
            return std::tie(
                       a.Template->PipelineId,
                       a.SortMaterialKey.Lo,
                       a.SortMaterialKey.Hi,
                       a.PerObjectBufferPage) <
                   std::tie(
                       b.Template->PipelineId,
                       b.SortMaterialKey.Lo,
                       b.SortMaterialKey.Hi,
                       b.PerObjectBufferPage);
        });
    }
    uint32_t submitted = 0;
    for (const MeshDrawCommand& command : _commands) {
        if (RecordCommand(encoder, command, resources, instanceCount)) {
            ++submitted;
        }
    }
    resources.Counters.ObjectArenaHighWatermark = resources.PerObjectArena.GetHighWatermark();
    resources.Counters.ViewArenaHighWatermark = resources.ViewArena.GetHighWatermark();
    return submitted;
}

}  // namespace radray
