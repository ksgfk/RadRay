#include <cstring>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/render/pipeline_state_cache.h>

namespace radray::render {

std::optional<GraphicsPsoKey> BuildGraphicsPsoKey(const GraphicsPipelineStateDescriptor& desc) noexcept {
    GraphicsPsoKey key{};

    if (desc.BindingLayout == nullptr) {
        RADRAY_ERR_LOG("PSO cache: BindingLayout is null");
        return std::nullopt;
    }
    const Guid layoutGuid = desc.BindingLayout->GetGuid();
    if (layoutGuid.IsEmpty()) {
        RADRAY_ERR_LOG("PSO cache: BindingLayout has no cache-assigned identity (create it via ShaderBindingLayoutCache)");
        return std::nullopt;
    }
    key.LayoutId = layoutGuid;

    auto fillShaderGuid = [](const std::optional<ShaderEntry>& entry, Guid& out, const char* name) noexcept -> bool {
        if (!entry.has_value() || entry->Target == nullptr) {
            out = Guid::Empty();
            return true;
        }
        const Guid g = entry->Target->GetGuid();
        if (g.IsEmpty()) {
            RADRAY_ERR_LOG("PSO cache: {} shader has no cache-assigned identity (create it via ShaderVariantCache)", name);
            return false;
        }
        out = g;
        return true;
    };
    if (!fillShaderGuid(desc.VS, key.VSId, "VS")) {
        return std::nullopt;
    }
    if (!fillShaderGuid(desc.PS, key.PSId, "PS")) {
        return std::nullopt;
    }

    const auto& prim = desc.Primitive;
    key.Topology = static_cast<int32_t>(prim.Topology);
    key.FaceClockwise = static_cast<int32_t>(prim.FaceClockwise);
    key.Cull = static_cast<int32_t>(prim.Cull);
    key.Poly = static_cast<int32_t>(prim.Poly);
    key.HasStripIndexFormat = prim.StripIndexFormat.has_value() ? 1u : 0u;
    key.StripIndexFormat = prim.StripIndexFormat.has_value() ? static_cast<int32_t>(prim.StripIndexFormat.value()) : 0;
    key.UnclippedDepth = prim.UnclippedDepth ? 1u : 0u;
    key.Conservative = prim.Conservative ? 1u : 0u;

    if (desc.DepthStencil.has_value()) {
        const auto& ds = desc.DepthStencil.value();
        key.HasDepthStencil = 1u;
        key.DSFormat = static_cast<int32_t>(ds.Format);
        key.DepthCompare = static_cast<int32_t>(ds.DepthCompare);
        key.DepthWriteEnable = ds.DepthWriteEnable ? 1u : 0u;
        key.DepthBiasConstant = ds.DepthBias.Constant;
        key.DepthBiasSlopScale = ds.DepthBias.SlopScale;
        key.DepthBiasClamp = ds.DepthBias.Clamp;
        if (ds.Stencil.has_value()) {
            const auto& st = ds.Stencil.value();
            key.HasStencil = 1u;
            key.StencilFrontCompare = static_cast<int32_t>(st.Front.Compare);
            key.StencilFrontFailOp = static_cast<int32_t>(st.Front.FailOp);
            key.StencilFrontDepthFailOp = static_cast<int32_t>(st.Front.DepthFailOp);
            key.StencilFrontPassOp = static_cast<int32_t>(st.Front.PassOp);
            key.StencilBackCompare = static_cast<int32_t>(st.Back.Compare);
            key.StencilBackFailOp = static_cast<int32_t>(st.Back.FailOp);
            key.StencilBackDepthFailOp = static_cast<int32_t>(st.Back.DepthFailOp);
            key.StencilBackPassOp = static_cast<int32_t>(st.Back.PassOp);
            key.StencilReadMask = st.ReadMask;
            key.StencilWriteMask = st.WriteMask;
        }
    }

    const auto& ms = desc.MultiSample;
    key.MsCount = ms.Count;
    key.MsMask = ms.Mask;
    key.MsAlphaToCoverage = ms.AlphaToCoverageEnable ? 1u : 0u;

    if (desc.ColorTargets.size() > kMaxColorTargets) {
        RADRAY_ERR_LOG("PSO cache: color target count {} exceeds limit {}", desc.ColorTargets.size(), kMaxColorTargets);
        return std::nullopt;
    }
    key.ColorTargetCount = static_cast<uint32_t>(desc.ColorTargets.size());
    for (uint32_t i = 0; i < key.ColorTargetCount; ++i) {
        const auto& ct = desc.ColorTargets[i];
        auto& out = key.ColorTargets[i];
        out.Format = static_cast<int32_t>(ct.Format);
        if (ct.Blend.has_value()) {
            const auto& b = ct.Blend.value();
            out.HasBlend = 1u;
            out.ColorSrc = static_cast<int32_t>(b.Color.Src);
            out.ColorDst = static_cast<int32_t>(b.Color.Dst);
            out.ColorOp = static_cast<int32_t>(b.Color.Op);
            out.AlphaSrc = static_cast<int32_t>(b.Alpha.Src);
            out.AlphaDst = static_cast<int32_t>(b.Alpha.Dst);
            out.AlphaOp = static_cast<int32_t>(b.Alpha.Op);
        }
        out.WriteMask = static_cast<uint32_t>(ct.WriteMask.value());
    }

    if (desc.VertexLayouts.size() > kMaxVertexBufferLayouts) {
        RADRAY_ERR_LOG("PSO cache: vertex layout count {} exceeds limit {}", desc.VertexLayouts.size(), kMaxVertexBufferLayouts);
        return std::nullopt;
    }
    key.VertexLayoutCount = static_cast<uint32_t>(desc.VertexLayouts.size());
    for (uint32_t i = 0; i < key.VertexLayoutCount; ++i) {
        const auto& vl = desc.VertexLayouts[i];
        auto& outVl = key.VertexLayouts[i];
        outVl.ArrayStride = vl.ArrayStride;
        outVl.StepMode = static_cast<int32_t>(vl.StepMode);
        if (vl.Elements.size() > kMaxVertexElementsPerLayout) {
            RADRAY_ERR_LOG("PSO cache: vertex element count {} in layout {} exceeds limit {}", vl.Elements.size(), i, kMaxVertexElementsPerLayout);
            return std::nullopt;
        }
        outVl.ElemCount = static_cast<uint32_t>(vl.Elements.size());
        for (uint32_t j = 0; j < outVl.ElemCount; ++j) {
            const auto& el = vl.Elements[j];
            auto& outEl = outVl.Elems[j];
            outEl.Offset = el.Offset;
            if (el.Semantic.size() >= kMaxSemanticLength) {
                RADRAY_ERR_LOG("PSO cache: semantic '{}' length {} exceeds limit {}", el.Semantic, el.Semantic.size(), kMaxSemanticLength - 1);
                return std::nullopt;
            }
            std::memcpy(outEl.Semantic, el.Semantic.data(), el.Semantic.size());
            outEl.SemanticIndex = el.SemanticIndex;
            outEl.Format = static_cast<int32_t>(el.Format);
            outEl.Location = el.Location;
        }
    }

    return key;
}

std::optional<ComputePsoKey> BuildComputePsoKey(const ComputePipelineStateDescriptor& desc) noexcept {
    ComputePsoKey key{};
    if (desc.BindingLayout == nullptr) {
        RADRAY_ERR_LOG("PSO cache: BindingLayout is null");
        return std::nullopt;
    }
    const Guid layoutGuid = desc.BindingLayout->GetGuid();
    if (layoutGuid.IsEmpty()) {
        RADRAY_ERR_LOG("PSO cache: BindingLayout has no cache-assigned identity (create it via ShaderBindingLayoutCache)");
        return std::nullopt;
    }
    key.LayoutId = layoutGuid;
    if (desc.CS.Target == nullptr) {
        RADRAY_ERR_LOG("PSO cache: compute shader is null");
        return std::nullopt;
    }
    const Guid csGuid = desc.CS.Target->GetGuid();
    if (csGuid.IsEmpty()) {
        RADRAY_ERR_LOG("PSO cache: CS shader has no cache-assigned identity (create it via ShaderVariantCache)");
        return std::nullopt;
    }
    key.CSId = csGuid;
    return key;
}

}  // namespace radray::render
