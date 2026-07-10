#include <radray/runtime/render_framework/mesh_pass_executor.h>

#include <cstring>
#include <utility>

#include <radray/logger.h>
#include <radray/render/sampler_cache.h>
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

}  // namespace

MeshPassExecutor::MeshPassExecutor(
    render::Device* device,
    render::ShaderVariantCache* variantCache,
    render::GraphicsPipelineStateCache* psoCache,
    render::SamplerCache* samplerCache,
    std::string perObjectCBufferName,
    uint32_t flightCount) noexcept
    : _device(device),
      _variantCache(variantCache),
      _psoCache(psoCache),
      _samplerCache(samplerCache),
      _perObjectName(std::move(perObjectCBufferName)) {
    const uint32_t count = flightCount == 0 ? 1 : flightCount;
    _flights.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        _flights.emplace_back(device);
    }
}

void MeshPassExecutor::BeginFrame(uint32_t flightIndex) noexcept {
    if (_flights.empty()) {
        _currentFlight = 0;
        return;
    }
    _currentFlight = flightIndex < _flights.size() ? flightIndex : 0;
    FlightResources& fr = _flights[_currentFlight];
    for (ParameterTableCache& cache : fr.TableCaches) {
        if (cache.Used < cache.Tables.size()) {
            cache.Tables.resize(cache.Used);
        }
        cache.Used = 0;
    }
    std::erase_if(fr.TableCaches, [](const ParameterTableCache& cache) noexcept {
        return cache.Tables.empty();
    });
    std::erase_if(_resolvedDrawStates, [](const ResolvedDrawState& state) noexcept {
        return state.Material.expired();
    });
    fr.Arena.Reset();
    _viewBindingValid = false;
}

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

Nullable<render::ShaderParameterTable*> MeshPassExecutor::AcquireParameterTable(
    FlightResources& resources,
    render::ShaderBindingLayout* layout) noexcept {
    ParameterTableCache* cache = nullptr;
    for (ParameterTableCache& candidate : resources.TableCaches) {
        if (candidate.Layout == layout) {
            cache = &candidate;
            break;
        }
    }
    if (cache == nullptr) {
        cache = &resources.TableCaches.emplace_back(ParameterTableCache{.Layout = layout});
    }
    if (cache->Used == cache->Tables.size()) {
        auto tableOpt = _device->CreateShaderParameterTable(layout);
        if (!tableOpt.HasValue()) {
            return nullptr;
        }
        cache->Tables.emplace_back(tableOpt.Release());
    }
    render::ShaderParameterTable* table = cache->Tables[cache->Used++].get();
    table->Reset();
    return table;
}

bool MeshPassExecutor::EnsureViewBinding(FlightResources& resources) noexcept {
    if (_viewBindingValid) {
        return true;
    }
    if (_viewData.empty()) {
        return false;
    }

    auto alloc = resources.Arena.Allocate(_viewData.size());
    if (alloc.Target == nullptr || alloc.Mapped == nullptr) {
        RADRAY_ERR_LOG("MeshPassExecutor: per-view cbuffer allocation failed");
        return false;
    }
    std::memcpy(alloc.Mapped, _viewData.data(), _viewData.size());
    _viewBinding.Target = alloc.Target;
    _viewBinding.Range = render::BufferRange{.Offset = alloc.Offset, .Size = _viewData.size()};
    _viewBinding.Usage = render::BufferViewUsage::CBuffer;
    _viewBindingValid = true;
    return true;
}

Nullable<const MeshPassExecutor::ResolvedDrawState*> MeshPassExecutor::ResolveDrawState(const DrawItem& item) noexcept {
    for (const ResolvedDrawState& state : _resolvedDrawStates) {
        const bool sameOwner = !state.Material.owner_before(item.Material) &&
                               !item.Material.owner_before(state.Material);
        if (!sameOwner || state.PassIndex != item.PassIndex ||
            state.GlobalKeywords.size() != _globalKeywords.size()) {
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
            return &state;
        }
    }

    auto variantOpt = item.Material->ResolveVariant(
        *_variantCache, item.PassIndex, std::span<const std::string_view>{_globalKeywords});
    if (!variantOpt.HasValue()) {
        RADRAY_ERR_LOG("MeshPassExecutor: failed to resolve shader variant for pass {}", item.PassIndex);
        return nullptr;
    }
    const render::CompiledShaderVariant* variant = variantOpt.Get();
    auto psoOpt = ResolvePso(item, *variant);
    if (!psoOpt.HasValue()) {
        return nullptr;
    }

    vector<string> globalKeywords;
    globalKeywords.reserve(_globalKeywords.size());
    for (std::string_view keyword : _globalKeywords) {
        globalKeywords.emplace_back(keyword);
    }
    ResolvedDrawState& state = _resolvedDrawStates.emplace_back(ResolvedDrawState{
        .Material = item.Material,
        .PassIndex = item.PassIndex,
        .GlobalKeywords = std::move(globalKeywords),
        .Variant = variant,
        .Pso = psoOpt.Get()});
    return &state;
}

Nullable<render::GraphicsPipelineState*> MeshPassExecutor::ResolvePso(
    const DrawItem& item,
    const render::CompiledShaderVariant& variant) noexcept {
    const ShaderAsset* shader = item.Material->Shader.Get();
    if (shader == nullptr) {
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
    const MaterialRenderState& rs = item.Material->RenderState;

    render::GraphicsPipelineStateDescriptor desc{};
    desc.BindingLayout = variant.Layout;
    desc.VS = render::ShaderEntry{.Target = variant.VS, .EntryPoint = passDesc.VertexEntry};
    desc.PS = render::ShaderEntry{.Target = variant.PS, .EntryPoint = passDesc.PixelEntry};
    desc.VertexLayouts = std::span<const render::VertexBufferLayout>{vertexLayouts.data(), vertexLayouts.size()};

    // Primitive: 覆盖 cull。
    desc.Primitive = passDesc.Primitive;
    if (rs.Cull.has_value()) {
        desc.Primitive.Cull = rs.Cull.value();
    }

    // DepthStencil: 覆盖 zwrite (仅当 pass 有 depth-stencil 段)。
    desc.DepthStencil = passDesc.DepthStencil;
    if (rs.DepthWrite.has_value() && desc.DepthStencil.has_value()) {
        desc.DepthStencil->DepthWriteEnable = rs.DepthWrite.value();
    }

    desc.MultiSample = passDesc.MultiSample;

    // ColorTargets: 覆盖每个 target 的 blend。passDesc.ColorTargets 为共享只读, 故本地拷贝再改,
    // 拷贝存活至 GetOrCreate 返回 (PSO 缓存据展平 key 去重, 不持有 desc)。
    if (rs.OverrideBlend) {
        _colorTargetScratch.assign(passDesc.ColorTargets.begin(), passDesc.ColorTargets.end());
        for (render::ColorTargetState& ct : _colorTargetScratch) {
            ct.Blend = rs.Blend;  // 有值=覆盖为开; nullopt=强制关闭混合
        }
        desc.ColorTargets = std::span<const render::ColorTargetState>{_colorTargetScratch.data(), _colorTargetScratch.size()};
    } else {
        desc.ColorTargets = std::span<const render::ColorTargetState>{passDesc.ColorTargets.data(), passDesc.ColorTargets.size()};
    }
    return _psoCache->GetOrCreate(desc);
}

bool MeshPassExecutor::SubmitItem(render::GraphicsCommandEncoder* encoder, const DrawItem& item) noexcept {
    if (encoder == nullptr || item.Material == nullptr || item.Proxy == nullptr) {
        return false;
    }

    if (_flights.empty()) {
        RADRAY_ERR_LOG("MeshPassExecutor: no flight resources (BeginFrame not called?)");
        return false;
    }
    FlightResources& fr = _flights[_currentFlight];

    // 1-2. 同一 material/pass/global-keyword 状态跨帧复用，快照失效时由 BeginFrame 回收。
    auto stateOpt = ResolveDrawState(item);
    if (!stateOpt.HasValue()) {
        return false;
    }
    const ResolvedDrawState& state = *stateOpt.Get();
    const render::CompiledShaderVariant& variant = *state.Variant;
    render::GraphicsPipelineState* pso = state.Pso;

    // 3. 取几何 (VB/IB + 索引范围)。
    MeshDrawArgs args = item.Proxy->GetDrawArgs(item.SectionIndex);
    if (args.Geometry == nullptr) {
        RADRAY_ERR_LOG("MeshPassExecutor: no geometry for section {}", item.SectionIndex);
        return false;
    }
    const render::RenderMesh::DrawData* draw = args.Geometry;
    if (draw->Ibv.Target == nullptr || draw->Ibv.Stride == 0) {
        RADRAY_ERR_LOG("MeshPassExecutor: section {} has no index buffer", item.SectionIndex);
        return false;
    }
    if (args.IndexCount == 0) {
        RADRAY_ERR_LOG("MeshPassExecutor: section {} has zero index count", item.SectionIndex);
        return false;
    }

    // 4. 参数表 (每条 draw 独立)。
    auto tableOpt = AcquireParameterTable(fr, variant.Layout);
    if (!tableOpt.HasValue()) {
        RADRAY_ERR_LOG("MeshPassExecutor: failed to create shader parameter table");
        return false;
    }
    render::ShaderParameterTable* table = tableOpt.Get();

    // 4a. per-object 常量 (LocalToWorld)。cbuffer 名字未在 shader 声明时静默跳过。
    if (!_perObjectName.empty()) {
        auto pid = table->GetShaderBindingLayout()->FindParameterId(_perObjectName);
        if (pid.has_value()) {
            PerObjectConstants c{};
            Eigen::Matrix4f m = item.Proxy->GetLocalToWorld();
            // Eigen 默认列优先存储; HLSL cbuffer 中 float4x4 默认也是 column_major,
            // 两者内存布局一致 (首 float4 = 第 0 列), 故直接拷贝, 无需转置。
            std::memcpy(c.ObjectToWorld, m.data(), sizeof(c.ObjectToWorld));

            auto alloc = fr.Arena.Allocate(sizeof(PerObjectConstants));
            if (alloc.Target == nullptr || alloc.Mapped == nullptr) {
                RADRAY_ERR_LOG("MeshPassExecutor: per-object cbuffer allocation failed");
                return false;
            }
            std::memcpy(alloc.Mapped, &c, sizeof(PerObjectConstants));
            render::BufferBindingDescriptor bbd{};
            bbd.Target = alloc.Target;
            bbd.Range = render::BufferRange{.Offset = alloc.Offset, .Size = sizeof(PerObjectConstants)};
            bbd.Usage = render::BufferViewUsage::CBuffer;
            table->SetResource(pid.value(), bbd);
        }
    }

    // 4b. per-view 常量 (可选)。
    if (!_viewName.empty() && !_viewData.empty()) {
        auto pid = table->GetShaderBindingLayout()->FindParameterId(_viewName);
        if (pid.has_value()) {
            if (!EnsureViewBinding(fr)) {
                return false;
            }
            table->SetResource(pid.value(), _viewBinding);
        }
    }

    // 4c. material property。
    //   - 常量: 交给 MaterialConstantBinder, 用变体反射把散字段打进所属 cbuffer 块 (整块提交)。
    //     per-object / per-view 系统块名被跳过 (上面 4a/4b 已单独填充)。
    //   - 纹理 / 采样器: 直接按名绑定。
    if (item.Material != nullptr) {
        item.Material->CollectConstants(_constantScratch);
        if (!_constantScratch.empty()) {
            std::string_view reserved[2] = {_perObjectName, _viewName};
            _constantBinder.Bind(variant, *table, fr.Arena, _constantScratch, reserved);
        }
        if (_samplerCache != nullptr) {
            item.Material->ApplyResources(*table, *_samplerCache);
        }
    }

    // 4d. 管线级全局纹理 / 采样器 (per-pass, 非 per-material)。名字未命中静默跳过。
    for (const GlobalTexture& g : _globalTextures) {
        if (g.View == nullptr) {
            continue;
        }
        auto pid = table->GetShaderBindingLayout()->FindParameterId(g.Name);
        if (pid.has_value()) {
            table->SetResource(pid.value(), static_cast<render::ResourceView*>(g.View));
        }
    }
    for (const GlobalSampler& g : _globalSamplers) {
        if (g.Sampler == nullptr) {
            continue;
        }
        auto pid = table->GetShaderBindingLayout()->FindParameterId(g.Name);
        if (pid.has_value()) {
            table->SetSampler(pid.value(), g.Sampler);
        }
    }

    // 5. 录制 draw。
    encoder->BindGraphicsPipelineState(pso);
    if (draw->Vbv.Target != nullptr) {
        encoder->BindVertexBuffer(std::span{&draw->Vbv, 1});
    }
    encoder->BindIndexBuffer(draw->Ibv);
    encoder->BindShaderParameters(table);
    encoder->DrawIndexed(args.IndexCount, 1, args.FirstIndex, args.VertexOffset, 0);
    return true;
}

uint32_t MeshPassExecutor::Execute(render::GraphicsCommandEncoder* encoder, const DrawList& list) noexcept {
    uint32_t submitted = 0;
    for (const DrawItem& item : list.Items()) {
        if (SubmitItem(encoder, item)) {
            ++submitted;
        }
    }
    return submitted;
}

}  // namespace radray
