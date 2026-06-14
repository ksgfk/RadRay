#include <radray/runtime/renderer/scene_renderer.h>

#include <algorithm>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/material.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>
#include <radray/runtime/renderer/scene.h>

namespace radray {

void MeshPassProcessor::BuildCommands(
    const VisiblePrimitiveList& visible,
    const SceneView& view,
    vector<MeshDrawCommand>& out) const {
    if (_config.Cache == nullptr) {
        RADRAY_ERR_LOG("MeshPassProcessor: PSOCache is null");
        return;
    }

    vector<MeshBatchElement> elements;
    for (const PrimitiveSceneProxy* proxy : visible.Primitives) {
        if (proxy == nullptr) {
            continue;
        }
        Material* material = proxy->GetMaterial();
        if (material == nullptr || !material->IsValid()) {
            continue;
        }
        const render::VertexBufferLayout& layout = proxy->GetVertexLayout();

        render::GraphicsPipelineState* pso = _config.Cache->GetOrCreate(*material, layout, _config.RtFormats);
        if (pso == nullptr) {
            continue;
        }
        auto paramOpt = material->FindParameterId(_config.ObjectConstantsParam);
        if (!paramOpt.has_value()) {
            RADRAY_ERR_LOG("MeshPassProcessor: material missing param '{}'", _config.ObjectConstantsParam);
            continue;
        }

        // per-object 常量:MVP = ViewProj * Model,Model 为代理世界矩阵。
        const Eigen::Matrix4f& model = proxy->GetWorldMatrix();
        const Eigen::Matrix4f mvp = view.ViewProjMatrix * model;
        ObjectConstants constants{};
        std::memcpy(constants.MVP, mvp.data(), sizeof(constants.MVP));
        std::memcpy(constants.Model, model.data(), sizeof(constants.Model));

        // PSO 指针高位作为分组键:同 PSO 命令相邻,减少状态切换。
        const uint64_t sortKey = reinterpret_cast<uintptr_t>(pso);

        elements.clear();
        proxy->CollectBatchElements(elements);
        for (const MeshBatchElement& e : elements) {
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
            cmd.RootSig = material->GetRootSignature();
            cmd.PushConstantId = paramOpt.value();
            cmd.PushConstantData.resize(sizeof(ObjectConstants));
            std::memcpy(cmd.PushConstantData.data(), &constants, sizeof(ObjectConstants));
            cmd.SortKey = sortKey;
            out.push_back(std::move(cmd));
        }
    }
}

void MeshPassProcessor::SortCommands(vector<MeshDrawCommand>& commands) {
    std::stable_sort(
        commands.begin(),
        commands.end(),
        [](const MeshDrawCommand& a, const MeshDrawCommand& b) noexcept {
            return a.SortKey < b.SortKey;
        });
}

void SceneRenderer::InitViews(const Scene& scene, const SceneView& view) {
    // 最小化:不做视锥裁剪,收集全部可渲染代理。view 预留给后续裁剪/relevance。
    (void)view;
    _visible.Clear();
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene.GetPrimitives()) {
        if (proxy == nullptr || !proxy->IsRenderable()) {
            continue;
        }
        _visible.Primitives.push_back(proxy.get());
    }
}

void SceneRenderer::Render(
    render::GraphicsCommandEncoder* encoder,
    const Scene& scene,
    const SceneView& view,
    const MeshPassProcessor::Config& processorConfig) {
    if (encoder == nullptr) {
        return;
    }
    // 1. InitViews:收集本 View 可见图元。
    InitViews(scene, view);
    if (_visible.Primitives.empty()) {
        return;
    }
    // 2. BasePass:构建/排序/录制。
    RenderBasePass(encoder, view, processorConfig);
}

void SceneRenderer::RenderBasePass(
    render::GraphicsCommandEncoder* encoder,
    const SceneView& view,
    const MeshPassProcessor::Config& processorConfig) {
    _drawCommands.clear();
    MeshPassProcessor processor{processorConfig};
    processor.BuildCommands(_visible, view, _drawCommands);
    if (_drawCommands.empty()) {
        return;
    }
    MeshPassProcessor::SortCommands(_drawCommands);

    // 顺序录制。跟踪上次绑定的 PSO/RootSig,排序后同状态命令相邻,跳过冗余绑定。
    render::GraphicsPipelineState* boundPso = nullptr;
    render::RootSignature* boundRootSig = nullptr;
    for (const MeshDrawCommand& cmd : _drawCommands) {
        if (cmd.RootSig != boundRootSig) {
            encoder->BindRootSignature(cmd.RootSig);
            boundRootSig = cmd.RootSig;
        }
        if (cmd.Pso != boundPso) {
            encoder->BindGraphicsPipelineState(cmd.Pso);
            boundPso = cmd.Pso;
        }
        if (!cmd.PushConstantData.empty()) {
            encoder->PushConstants(
                cmd.PushConstantId,
                cmd.PushConstantData.data(),
                static_cast<uint32_t>(cmd.PushConstantData.size()));
        }
        encoder->BindVertexBuffer(std::span{&cmd.Vbv, 1});
        encoder->BindIndexBuffer(cmd.Ibv);
        encoder->DrawIndexed(cmd.IndexCount, 1, cmd.FirstIndex, cmd.VertexOffset, 0);
    }
}

}  // namespace radray
