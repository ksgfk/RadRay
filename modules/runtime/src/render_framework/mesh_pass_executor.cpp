#include <radray/runtime/render_framework/mesh_pass_executor.h>

#include <cstring>
#include <utility>

#include <radray/logger.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>
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
    std::string perObjectCBufferName,
    uint32_t flightCount) noexcept
    : _device(device),
      _variantCache(variantCache),
      _psoCache(psoCache),
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
    fr.Tables.clear();
    fr.Arena.Reset();
}

void MeshPassExecutor::SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept {
    _viewName.assign(viewCBufferName.begin(), viewCBufferName.end());
    _viewData.assign(data.begin(), data.end());
}

Nullable<render::GraphicsPipelineState*> MeshPassExecutor::ResolvePso(
    const DrawItem& item,
    const render::CompiledShaderVariant& variant) noexcept {
    const ShaderAsset* shader = item.Material->GetShader().Get();
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

    render::GraphicsPipelineStateDescriptor desc{};
    desc.BindingLayout = variant.Layout;
    desc.VS = render::ShaderEntry{.Target = variant.VS, .EntryPoint = passDesc.VertexEntry};
    desc.PS = render::ShaderEntry{.Target = variant.PS, .EntryPoint = passDesc.PixelEntry};
    desc.VertexLayouts = std::span<const render::VertexBufferLayout>{vertexLayouts.data(), vertexLayouts.size()};
    desc.Primitive = passDesc.Primitive;
    desc.DepthStencil = passDesc.DepthStencil;
    desc.MultiSample = passDesc.MultiSample;
    desc.ColorTargets = std::span<const render::ColorTargetState>{passDesc.ColorTargets.data(), passDesc.ColorTargets.size()};
    return _psoCache->GetOrCreate(desc);
}

bool MeshPassExecutor::SubmitItem(render::GraphicsCommandEncoder* encoder, const DrawItem& item) noexcept {
    if (encoder == nullptr || item.Material == nullptr || item.Proxy == nullptr) {
        return false;
    }

    // 1. 解析变体。
    auto variantOpt = item.Material->ResolveVariant(*_variantCache, item.PassIndex);
    if (!variantOpt.HasValue()) {
        RADRAY_ERR_LOG("MeshPassExecutor: failed to resolve shader variant for pass {}", item.PassIndex);
        return false;
    }
    const render::CompiledShaderVariant& variant = *variantOpt.Get();

    // 2. PSO。
    auto psoOpt = ResolvePso(item, variant);
    if (!psoOpt.HasValue()) {
        return false;
    }
    render::GraphicsPipelineState* pso = psoOpt.Get();

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

    if (_flights.empty()) {
        RADRAY_ERR_LOG("MeshPassExecutor: no flight resources (BeginFrame not called?)");
        return false;
    }
    FlightResources& fr = _flights[_currentFlight];

    // 4. 参数表 (每条 draw 独立)。
    auto tableOpt = _device->CreateShaderParameterTable(variant.Layout);
    if (!tableOpt.HasValue()) {
        RADRAY_ERR_LOG("MeshPassExecutor: failed to create shader parameter table");
        return false;
    }
    render::ShaderParameterTable* table = fr.Tables.emplace_back(tableOpt.Release()).get();

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
            auto alloc = fr.Arena.Allocate(_viewData.size());
            if (alloc.Target != nullptr && alloc.Mapped != nullptr) {
                std::memcpy(alloc.Mapped, _viewData.data(), _viewData.size());
                render::BufferBindingDescriptor bbd{};
                bbd.Target = alloc.Target;
                bbd.Range = render::BufferRange{.Offset = alloc.Offset, .Size = _viewData.size()};
                bbd.Usage = render::BufferViewUsage::CBuffer;
                table->SetResource(pid.value(), bbd);
            }
        }
    }

    // 4c. material property (push/root constant + 纹理/采样器)。
    item.Material->ApplyProperties(*table);

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
