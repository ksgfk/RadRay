#include <radray/runtime/render_framework/mesh_pass_executor.h>

#include <cstring>
#include <algorithm>
#include <tuple>
#include <utility>

#include <radray/logger.h>
#include <radray/hash.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_asset.h>
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

}  // namespace

MeshPassExecutor::MeshPassExecutor(
    render::Device* device,
    ShaderVariantLibrary* variantCache,
    GraphicsPipelineStateLibrary* psoCache,
    SamplerCache* samplerCache,
    std::string perObjectCBufferName) noexcept
    : _device(device),
      _variantCache(variantCache),
      _psoCache(psoCache),
      _samplerCache(samplerCache),
      _perObjectName(std::move(perObjectCBufferName)) {}

void MeshPassExecutor::SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept {
    _viewName.assign(viewCBufferName.begin(), viewCBufferName.end());
    _viewData.assign(data.begin(), data.end());
    _viewBindingValid = false;
}

void MeshPassExecutor::SetGlobalTexture(std::string_view name, render::TextureView* view) noexcept {
    for (GlobalTexture& g : _globalTextures) {
        if (g.Name == name) {
            g.View = view;
            return;
        }
    }
    _globalTextures.push_back(GlobalTexture{std::string{name}, view});
}

void MeshPassExecutor::SetGlobalSampler(std::string_view name, render::Sampler* sampler) noexcept {
    for (GlobalSampler& g : _globalSamplers) {
        if (g.Name == name) {
            g.Sampler = sampler;
            return;
        }
    }
    _globalSamplers.push_back(GlobalSampler{std::string{name}, sampler});
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
    _globalTextures.clear();
    _globalSamplers.clear();
    _globalKeywords.clear();
}

Nullable<render::BindingGroup*> MeshPassExecutor::AcquireSystemGroup(
    FrameResources& resources,
    const CompiledShaderVariant& variant,
    uint32_t groupIndex,
    render::Buffer* dynamicBuffer) noexcept {
    render::PipelineLayout* layout = variant.Layout;
    if (layout == nullptr) {
        return nullptr;
    }
    vector<render::ResourceView*> resourceKey{};
    vector<render::Sampler*> samplerKey{};
    if (groupIndex == 1) {
        resourceKey.reserve(_globalTextures.size());
        samplerKey.reserve(_globalSamplers.size());
        for (const GlobalTexture& global : _globalTextures) {
            resourceKey.push_back(global.View);
        }
        for (const GlobalSampler& global : _globalSamplers) {
            samplerKey.push_back(global.Sampler);
        }
    }

    for (FrameBindingGroupCacheEntry& cache : resources.SystemGroups) {
        if (cache.Layout == layout && cache.GroupIndex == groupIndex &&
            cache.DynamicBuffer == dynamicBuffer && cache.Resources == resourceKey &&
            cache.Samplers == samplerKey) {
            ++resources.Counters.SystemGroupCacheHits;
            return cache.Group.get();
        }
    }

    ++resources.Counters.SystemGroupCacheMisses;
    auto groupOpt = _device->CreateBindingGroup(
        resources.SystemDescriptorPool.get(), layout, groupIndex);
    if (!groupOpt.HasValue()) {
        return nullptr;
    }
    auto group = groupOpt.Release();
    render::BufferBindingDescriptor dynamicBinding{};
    dynamicBinding.Target = dynamicBuffer;
    dynamicBinding.Range = render::BufferRange{
        .Offset = 0,
        .Size = groupIndex == 0 ? sizeof(PerObjectConstants) : _viewData.size()};
    dynamicBinding.Usage = render::BufferViewUsage::CBuffer;
    const std::string_view dynamicName = groupIndex == 0 ? std::string_view{_perObjectName}
                                                         : std::string_view{_viewName};
    auto dynamicLocation = FindShaderBindingLocation(variant, dynamicName);
    if (!dynamicLocation.has_value() || dynamicLocation->Group != groupIndex ||
        !group->SetResource(dynamicLocation->Binding, dynamicBinding)) {
        return nullptr;
    }
    uint64_t descriptorUpdates = 1;

    if (groupIndex == 1) {
        for (const GlobalTexture& global : _globalTextures) {
            auto location = FindShaderBindingLocation(variant, global.Name);
            if (global.View == nullptr || !location.has_value() || location->Group != groupIndex) {
                RADRAY_ERR_LOG(
                    "MeshPassExecutor: global texture '{}' has no binding in group {}",
                    global.Name,
                    groupIndex);
                continue;
            }
            if (!group->SetResource(
                    location->Binding,
                    static_cast<render::ResourceView*>(global.View))) {
                RADRAY_ERR_LOG(
                    "MeshPassExecutor: failed to bind global texture '{}' at group {} binding {}",
                    global.Name,
                    groupIndex,
                    location->Binding);
                continue;
            }
            ++descriptorUpdates;
        }
        for (const GlobalSampler& global : _globalSamplers) {
            auto location = FindShaderBindingLocation(variant, global.Name);
            if (global.Sampler == nullptr || !location.has_value() || location->Group != groupIndex) {
                RADRAY_ERR_LOG(
                    "MeshPassExecutor: global sampler '{}' has no binding in group {}",
                    global.Name,
                    groupIndex);
                continue;
            }
            if (!group->SetSampler(location->Binding, global.Sampler)) {
                RADRAY_ERR_LOG(
                    "MeshPassExecutor: failed to bind global sampler '{}' at group {} binding {}",
                    global.Name,
                    groupIndex,
                    location->Binding);
                continue;
            }
            ++descriptorUpdates;
        }
    }

    ++resources.Counters.DescriptorGroupCreates;
    resources.Counters.DescriptorGroupUpdates += descriptorUpdates;
    FrameBindingGroupCacheEntry& cache = resources.SystemGroups.emplace_back(FrameBindingGroupCacheEntry{
        .Layout = layout,
        .GroupIndex = groupIndex,
        .DynamicBuffer = dynamicBuffer,
        .Resources = std::move(resourceKey),
        .Samplers = std::move(samplerKey),
        .Group = std::move(group)});
    return cache.Group.get();
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

Nullable<render::BindingGroup*> MeshPassExecutor::AcquireMaterialGroup(
    const DrawItem& item,
    const CompiledShaderVariant& variant,
    FrameResources& resources) noexcept {
    if (item.Material == nullptr || variant.Layout == nullptr) {
        return nullptr;
    }
    bool hasMaterialGroup = false;
    for (const render::BindingGroupLayout& groupLayout : variant.Layout->GetBindingGroupLayouts()) {
        if (groupLayout.GroupIndex == 2 && !groupLayout.Entries.empty()) {
            hasMaterialGroup = true;
            break;
        }
    }
    if (!hasMaterialGroup) {
        return nullptr;
    }

    if (_materialDescriptorPool == nullptr) {
        auto poolOpt = _device->CreateDescriptorPool(render::DescriptorPoolDescriptor{
            .MaxBindingGroups = 512,
            .MaxSampledTextures = 4096,
            .MaxStorageTextures = 128,
            .MaxUniformBuffers = 512,
            .MaxDynamicUniformBuffers = 0,
            .MaxStorageBuffers = 128,
            .MaxReadOnlyTexelBuffers = 64,
            .MaxReadWriteTexelBuffers = 64,
            .MaxSamplers = 4096,
            .MaxAccelerationStructures = 0,
            .Lifetime = render::DescriptorPoolLifetime::Persistent});
        if (!poolOpt.HasValue()) {
            return nullptr;
        }
        _materialDescriptorPool = shared_ptr<render::DescriptorPool>{poolOpt.Release()};
        _materialDescriptorPool->SetDebugName("material_descriptors");
    }
    if (_materialConstantPool == nullptr) {
        _materialConstantPool = make_shared<MaterialConstantPool>(
            _device,
            256 * 1024,
            std::max<uint64_t>(256, _device->GetDetail().CBufferAlignment));
    }

    for (MaterialBinding& binding : _materialBindings) {
        if (binding.Key == item.Material->BindingKey && binding.Layout == variant.Layout) {
            binding.LastUsedFrame = _frameSerial;
            ++resources.Counters.MaterialGroupCacheHits;
            return binding.Group.get();
        }
    }

    ++resources.Counters.MaterialGroupCacheMisses;
    auto groupOpt = _device->CreateBindingGroup(
        _materialDescriptorPool.get(), variant.Layout, 2);
    if (!groupOpt.HasValue()) {
        return nullptr;
    }
    auto group = groupOpt.Release();
    ++resources.Counters.DescriptorGroupCreates;
    item.Material->CollectConstants(_constantScratch);
    vector<MaterialConstantPool::Allocation> constantAllocations;
    const auto releaseConstants = [this, &constantAllocations]() noexcept {
        for (const auto& allocation : constantAllocations) {
            _materialConstantPool->Release(allocation);
        }
        constantAllocations.clear();
    };
    uint32_t constantBindings = 0;
    if (!_constantScratch.empty()) {
        const std::string_view reserved[] = {_perObjectName, _viewName};
        constantBindings = _constantBinder.Bind(
            variant,
            *group,
            *_materialConstantPool,
            _constantScratch,
            reserved,
            &constantAllocations);
        if (constantBindings == 0) {
            releaseConstants();
            return nullptr;
        }
    }
    if (_samplerCache != nullptr) {
        uint32_t expectedResources = 0;
        const auto countExpected = [&](std::string_view name) {
            auto location = FindShaderBindingLocation(variant, name);
            if (location.has_value() && location->Group == group->GetGroupIndex()) {
                ++expectedResources;
            }
        };
        for (const auto& texture : item.Material->Textures) {
            countExpected(texture.Name);
        }
        for (const auto& sampler : item.Material->Samplers) {
            countExpected(sampler.Name);
        }
        const uint32_t appliedResources =
            item.Material->ApplyResources(*group, variant, *_samplerCache);
        if (appliedResources != expectedResources) {
            releaseConstants();
            return nullptr;
        }
        resources.Counters.DescriptorGroupUpdates += appliedResources;
    }
    resources.Counters.DescriptorGroupUpdates += constantBindings;

    MaterialBinding& binding = _materialBindings.emplace_back(MaterialBinding{
        .Key = item.Material->BindingKey,
        .Snapshot = item.Material,
        .Layout = variant.Layout,
        .Group = std::move(group),
        .ConstantAllocations = std::move(constantAllocations),
        .LastUsedFrame = _frameSerial});
    return binding.Group.get();
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
        RADRAY_ERR_LOG("MeshPassExecutor: failed to resolve shader variant for pass {}", item.PassIndex);
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
    render::BindingGroup* materialGroup,
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
            candidate->MaterialGroup == materialGroup &&
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
    command->Layout = state.Variant->Layout;
    command->MaterialGroup = materialGroup;
    command->Geometry = geometry;
    command->FirstIndex = args.FirstIndex;
    command->IndexCount = args.IndexCount;
    command->VertexOffset = args.VertexOffset;
    command->PipelineId = _psoCache->GetId(state.Pso);
    command->MaterialBindingId = item.Material->BindingKey.Lo ^ item.Material->BindingKey.Hi;
    const void* pages[] = {geometry.Vbv.Target, geometry.Ibv.Target};
    command->GeometryPageId = HashData64(pages, sizeof(pages));
    command->LastUsedFrame = _frameSerial;
    auto* result = command.get();
    _commandTemplates.push_back(std::move(command));
    return result;
}

bool MeshPassExecutor::CompileCommand(const DrawItem& item, FrameResources& resources) noexcept {
    if (item.Material == nullptr || item.Proxy == nullptr) {
        return false;
    }

    // 1-2. 同一 material/pass/global-keyword 状态跨帧复用，快照失效时由 BeginFrame 回收。
    auto stateOpt = ResolveDrawState(item, resources);
    if (!stateOpt.HasValue()) {
        return false;
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

    // 4. 编译本条 draw 的频率分组绑定。PerObject / PerView 只改变 dynamic offset；
    // descriptor set 本身由 flight/layout 缓存复用。
    auto objectAllocation = EnsureObjectBinding(resources, item.Proxy);
    if (!objectAllocation.HasValue() || objectAllocation.Get()->Offset > std::numeric_limits<uint32_t>::max()) {
        RADRAY_ERR_LOG("MeshPassExecutor: per-object cbuffer allocation failed");
        return false;
    }
    auto objectGroup = AcquireSystemGroup(resources, variant, 0, objectAllocation.Get()->Target);
    if (!objectGroup.HasValue()) {
        RADRAY_ERR_LOG("MeshPassExecutor: failed to acquire per-object binding group");
        return false;
    }

    Nullable<const DynamicCBufferArena::Allocation*> viewAllocation{};
    Nullable<render::BindingGroup*> viewGroup{};
    if (!_viewName.empty() && !_viewData.empty()) {
        viewAllocation = EnsureViewBinding(resources);
        if (!viewAllocation.HasValue() || viewAllocation.Get()->Offset > std::numeric_limits<uint32_t>::max()) {
            RADRAY_ERR_LOG("MeshPassExecutor: per-view cbuffer allocation failed");
            return false;
        }
        viewGroup = AcquireSystemGroup(resources, variant, 1, viewAllocation.Get()->Target);
        if (!viewGroup.HasValue()) {
            RADRAY_ERR_LOG("MeshPassExecutor: failed to acquire per-view binding group");
            return false;
        }
    }
    auto materialGroup = AcquireMaterialGroup(item, variant, resources);
    bool needsMaterialGroup = false;
    for (const render::BindingGroupLayout& groupLayout : variant.Layout->GetBindingGroupLayouts()) {
        if (groupLayout.GroupIndex == 2 && !groupLayout.Entries.empty()) {
            needsMaterialGroup = true;
            break;
        }
    }
    if (needsMaterialGroup && !materialGroup.HasValue()) {
        return false;
    }

    auto commandTemplate = GetOrCreateCommandTemplate(
        item,
        state,
        args,
        materialGroup.HasValue() ? materialGroup.Get() : nullptr,
        resources);
    if (!commandTemplate.HasValue()) {
        return false;
    }
    _commands.push_back(MeshDrawCommand{
        .Template = commandTemplate.Get(),
        .PerObjectGroup = objectGroup.Get(),
        .ViewGroup = viewGroup.HasValue() ? viewGroup.Get() : nullptr,
        .PerObjectDynamicOffset = static_cast<uint32_t>(objectAllocation.Get()->Offset),
        .ViewDynamicOffset = viewAllocation.HasValue() ? static_cast<uint32_t>(viewAllocation.Get()->Offset) : 0u,
        .PerObjectBufferPage = reinterpret_cast<uint64_t>(objectAllocation.Get()->Target),
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
    if (_recorder.Groups[0] != command.PerObjectGroup ||
        _recorder.DynamicOffsets[0] != command.PerObjectDynamicOffset) {
        encoder->BindBindingGroup(
            0,
            command.PerObjectGroup,
            std::span{&command.PerObjectDynamicOffset, 1});
        _recorder.Groups[0] = command.PerObjectGroup;
        _recorder.DynamicOffsets[0] = command.PerObjectDynamicOffset;
        ++resources.Counters.DescriptorGroupBinds;
        ++resources.Counters.DynamicOffsetBinds;
    }
    if (command.ViewGroup != nullptr) {
        if (_recorder.Groups[1] != command.ViewGroup ||
            _recorder.DynamicOffsets[1] != command.ViewDynamicOffset) {
            encoder->BindBindingGroup(1, command.ViewGroup, std::span{&command.ViewDynamicOffset, 1});
            _recorder.Groups[1] = command.ViewGroup;
            _recorder.DynamicOffsets[1] = command.ViewDynamicOffset;
            ++resources.Counters.DescriptorGroupBinds;
            ++resources.Counters.DynamicOffsetBinds;
        }
    } else {
        _recorder.Groups[1] = nullptr;
    }
    if (draw.MaterialGroup != nullptr) {
        if (_recorder.Groups[2] != draw.MaterialGroup) {
            encoder->BindBindingGroup(2, draw.MaterialGroup);
            _recorder.Groups[2] = draw.MaterialGroup;
            ++resources.Counters.DescriptorGroupBinds;
        }
    } else {
        _recorder.Groups[2] = nullptr;
    }
    encoder->DrawIndexed(draw.IndexCount, instanceCount, draw.FirstIndex, draw.VertexOffset, 0);
    ++resources.Counters.Draws;
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
    for (auto it = _materialBindings.begin(); it != _materialBindings.end();) {
        if (it->LastUsedFrame + 1 >= _frameSerial) {
            ++it;
            continue;
        }
        render::BindingGroup* group = it->Group.get();
        std::erase_if(_commandTemplates, [group](const auto& command) noexcept {
            return command->MaterialGroup == group;
        });
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
        it = _materialBindings.erase(it);
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
    if (list.GetSortMode() == DrawList::SortMode::Opaque) {
        std::stable_sort(_commands.begin(), _commands.end(), [](const MeshDrawCommand& a, const MeshDrawCommand& b) {
            return std::tie(
                       a.RenderQueue,
                       a.Template->PipelineId,
                       a.Template->MaterialKey.Lo,
                       a.Template->MaterialKey.Hi,
                       a.PerObjectBufferPage,
                       a.Template->GeometryPageId,
                       a.ViewDistance) <
                   std::tie(
                       b.RenderQueue,
                       b.Template->PipelineId,
                       b.Template->MaterialKey.Lo,
                       b.Template->MaterialKey.Hi,
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
                       a.Template->MaterialKey.Lo,
                       a.Template->MaterialKey.Hi,
                       a.PerObjectBufferPage) <
                   std::tie(
                       b.Template->PipelineId,
                       b.Template->MaterialKey.Lo,
                       b.Template->MaterialKey.Hi,
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
