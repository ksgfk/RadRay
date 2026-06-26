#include <radray/runtime/render/render_context.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/render/render_pass.h>
#include <radray/runtime/render/renderer.h>
#include <radray/runtime/render/material.h>
#include <radray/runtime/render/shader.h>

namespace radray::srp {

RendererList RenderContext::CreateRendererList(
    const CullingResults& cull,
    const DrawingSettings& draw,
    const FilteringSettings& filter,
    const GpuRenderTargetFormats& rtFormats,
    const RenderPass& pass) {
    RendererList list;
    if (_gpu == nullptr || _variantCache == nullptr) {
        RADRAY_ERR_LOG("RenderContext: gpu or variant cache is null");
        return list;
    }
    PSOCache& psoCache = _gpu->GetPSOCache();
    const uint32_t perObjectSize = pass.PerObjectByteSize();

    for (const Renderer* r : cull.Renderers) {
        if (r == nullptr) {
            continue;
        }
        // 第一层:意图(queue / layer / visible)。
        if (!filter.Test(*r)) {
            continue;
        }
        Material* mat = r->GetMaterial();
        Shader* sh = mat != nullptr ? mat->GetShader() : nullptr;
        if (sh == nullptr) {
            continue;
        }
        // 第二层:relevance —— 按 LightMode 优先级找本 shader 命中的 pass;找不到丢弃。
        std::string_view lightMode;
        if (!sh->ResolveTag(draw.ShaderTags, &lightMode)) {
            continue;
        }
        KeywordSet kw = draw.PassKeywords;
        kw.Merge(mat->MaterialKeywords());
        const ShaderVariant* variant = _variantCache->Get(*sh, lightMode, kw);
        if (variant == nullptr) {
            continue;
        }

        // —— 组合渲染状态:pass 权威 + material 语义(twoSided→cull)——
        MeshPassRenderState state = pass.RenderState(*mat);
        render::DepthStencilState depthState = state.DepthStencil;
        depthState.Format = rtFormats.DepthFormat;

        vector<render::ColorTargetState> colorTargets;
        colorTargets.reserve(rtFormats.ColorFormats.size());
        for (render::TextureFormat fmt : rtFormats.ColorFormats) {
            render::ColorTargetState ct = render::ColorTargetState::Default(fmt);
            ct.Blend = state.Blend;
            ct.WriteMask = state.ColorWriteMask;
            colorTargets.push_back(ct);
        }

        render::PrimitiveState primitive = render::PrimitiveState::Default();
        if (mat->IsTwoSided()) {
            primitive.Cull = render::CullMode::None;
        }

        std::optional<CompiledShaderEntry> psEntry = variant->PS;
        const render::VertexBufferLayout& layout = r->GetVertexLayout();
        render::VertexBufferLayout vertexLayouts[] = {layout};
        render::GraphicsPipelineState* pso = psoCache.GetOrCreate(PSOCache::GraphicsPsoDesc{
            .RootSig = variant->RootSig,
            .RootLayout = variant->RootLayout,
            .VS = variant->VS,
            .PS = psEntry,
            .VertexLayouts = std::span<const render::VertexBufferLayout>{vertexLayouts, 1},
            .Primitive = primitive,
            .DepthStencil = depthState,
            .MultiSample = render::MultiSampleState::Default(),
            .ColorTargets = std::span<const render::ColorTargetState>{colorTargets.data(), colorTargets.size()}});
        if (pso == nullptr) {
            continue;
        }
        render::RootSignature* rootSig = variant->RootSig;

        // —— per-object push-constant:pass 决定布局与内容,反射给字节数 ——
        render::BindingParameterId pushId{};
        uint32_t pushSize = 0;
        const std::string_view paramName = pass.PerObjectParamName();
        if (!paramName.empty() && perObjectSize > 0) {
            auto idOpt = rootSig->FindParameterId(paramName);
            if (idOpt.has_value()) {
                pushId = idOpt.value();
                pushSize = perObjectSize;
                auto rangeOpt = rootSig->FindPushConstantRange(pushId);
                if (rangeOpt.HasValue()) {
                    const uint32_t reflected = rangeOpt.Get()->Size;
                    if (reflected > perObjectSize) {
                        RADRAY_ERR_LOG(
                            "RenderContext: push-constant '{}' reflected size {} exceeds pass PerObjectByteSize {}; skipping draw",
                            paramName, reflected, perObjectSize);
                        continue;
                    }
                    pushSize = reflected;
                }
            }
        }

        // per-material descriptor set(space1),懒建 + 缓存。
        render::DescriptorSet* materialSet = mat->GetDescriptorSet(rootSig);
        const render::DescriptorSetIndex materialSetIndex = mat->MaterialSetIndex();

        // PSO 指针高位作为分组键:同 PSO 命令相邻,减少状态切换。
        const uint64_t sortKey = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pso));

        const MeshBatchElement e = r->BatchElement();
        if (e.Vbv.Target == nullptr || e.Ibv.Target == nullptr || e.IndexCount == 0) {
            continue;
        }
        MeshDrawCommand cmd{};
        cmd.Vbv = e.Vbv;
        cmd.Ibv = e.Ibv;
        cmd.IndexCount = e.IndexCount;
        cmd.FirstIndex = e.FirstIndex;
        cmd.VertexOffset = e.VertexOffset;
        cmd.Pso = pso;
        cmd.RootSig = rootSig;
        if (pushSize > 0) {
            cmd.PushConstantId = pushId;
            cmd.PushConstantData.resize(pushSize);
            pass.WritePerObject(std::span<byte>{cmd.PushConstantData.data(), cmd.PushConstantData.size()}, *r, cull.View);
        }
        if (draw.ViewConstants != nullptr) {
            cmd.DescriptorSets[cmd.DescriptorSetCount++] = BoundDescriptorSet{draw.ViewConstantsIndex, draw.ViewConstants};
        }
        if (materialSet != nullptr) {
            cmd.DescriptorSets[cmd.DescriptorSetCount++] = BoundDescriptorSet{materialSetIndex, materialSet};
        }
        cmd.SortKey = sortKey;
        list.Commands.push_back(std::move(cmd));
    }

    // 批 = 排序副产物。FrontToBack/BackToFront 暂用 PSO 分组(深度排序留待后续接入 bounds)。
    std::stable_sort(
        list.Commands.begin(), list.Commands.end(),
        [](const MeshDrawCommand& a, const MeshDrawCommand& b) noexcept { return a.SortKey < b.SortKey; });
    return list;
}

void RenderContext::DrawRendererList(const DrawingSettings& draw, const RendererList& list) {
    (void)draw;  // space0 已在每条命令的 DescriptorSets 里;此处统一走逐命令绑定。
    if (_encoder == nullptr || list.Empty()) {
        return;
    }
    render::GraphicsPipelineState* boundPso = nullptr;
    render::RootSignature* boundRootSig = nullptr;
    std::array<render::DescriptorSet*, MeshDrawCommand::MaxBoundSets> boundSets{};
    for (const MeshDrawCommand& cmd : list.Commands) {
        if (cmd.RootSig != boundRootSig) {
            _encoder->BindRootSignature(cmd.RootSig);
            boundRootSig = cmd.RootSig;
            boundSets.fill(nullptr);
        }
        if (cmd.Pso != boundPso) {
            _encoder->BindGraphicsPipelineState(cmd.Pso);
            boundPso = cmd.Pso;
        }
        for (uint32_t i = 0; i < cmd.DescriptorSetCount; ++i) {
            const BoundDescriptorSet& bound = cmd.DescriptorSets[i];
            if (bound.Handle == nullptr) {
                continue;
            }
            const uint32_t slot = bound.Set.Value;
            if (slot < boundSets.size() && boundSets[slot] == bound.Handle) {
                continue;
            }
            _encoder->BindDescriptorSet(bound.Set, bound.Handle);
            if (slot < boundSets.size()) {
                boundSets[slot] = bound.Handle;
            }
        }
        if (!cmd.PushConstantData.empty()) {
            _encoder->PushConstants(cmd.PushConstantId, cmd.PushConstantData.data(),
                                    static_cast<uint32_t>(cmd.PushConstantData.size()));
        }
        _encoder->BindVertexBuffer(std::span{&cmd.Vbv, 1});
        _encoder->BindIndexBuffer(cmd.Ibv);
        _encoder->DrawIndexed(cmd.IndexCount, 1, cmd.FirstIndex, cmd.VertexOffset, 0);
    }
}

}  // namespace radray::srp
