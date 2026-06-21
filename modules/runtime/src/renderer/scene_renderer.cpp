#include <radray/runtime/renderer/scene_renderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/material.h>
#include <radray/runtime/renderer/light_scene_proxy.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>
#include <radray/runtime/renderer/scene.h>

namespace radray {

void MeshPassProcessor::BuildCommands(
    const VisiblePrimitiveList& visible,
    const SceneView& view,
    vector<MeshDrawCommand>& out,
    const PrimitiveFilter& filter) const {
    if (_config.Cache == nullptr) {
        RADRAY_ERR_LOG("MeshPassProcessor: PSOCache is null");
        return;
    }

    vector<MeshBatchElement> elements;
    for (const PrimitiveSceneProxy* proxy : visible.Primitives) {
        if (proxy == nullptr) {
            continue;
        }
        // filter 非空:只为通过过滤的图元产出命令(对应 Unity FilteringSettings)。
        if (filter && !filter(*proxy)) {
            continue;
        }
        Material* material = proxy->GetMaterial();
        if (material == nullptr || !material->IsValid()) {
            continue;
        }
        const render::VertexBufferLayout& layout = proxy->GetVertexLayout();

        render::GraphicsPipelineState* pso = _config.Cache->GetOrCreate(
            *material,
            layout,
            _config.RtFormats,
            _config.PipelineOverride);
        if (pso == nullptr) {
            continue;
        }
        auto paramOpt = material->FindParameterId(_config.ObjectConstantsParam);
        if (!paramOpt.has_value()) {
            RADRAY_ERR_LOG("MeshPassProcessor: material missing param '{}'", _config.ObjectConstantsParam);
            continue;
        }

        // 该 shader 的 gScene push-constant 实际尺寸可能小于 ObjectConstants(160)。
        // 例如 example_imgui 的 sphere.hlsl 仅声明 MVP+Model(128)。
        // 按 root signature 反射出的实际尺寸裁剪,只推送 shader 需要的前缀,
        // 避免 "push constant size mismatch expected:128 actual:160"。
        render::RootSignature* rootSig = material->GetRootSignature();
        uint32_t pushSize = static_cast<uint32_t>(sizeof(ObjectConstants));
        if (rootSig != nullptr) {
            auto rangeOpt = rootSig->FindPushConstantRange(paramOpt.value());
            if (rangeOpt.HasValue()) {
                pushSize = std::min(pushSize, rangeOpt.Get()->Size);
            }
        }

        // per-object 常量:MVP = ViewProj * Model,Model 为代理世界矩阵。
        const Eigen::Matrix4f& model = proxy->GetWorldMatrix();
        const Eigen::Matrix4f mvp = view.ViewProjMatrix * model;
        ObjectConstants constants{};
        std::memcpy(constants.MVP, mvp.data(), sizeof(constants.MVP));
        std::memcpy(constants.Model, model.data(), sizeof(constants.Model));
        constants.CameraPosition[0] = view.EyePosition.x();
        constants.CameraPosition[1] = view.EyePosition.y();
        constants.CameraPosition[2] = view.EyePosition.z();
        constants.CameraPosition[3] = 1.0f;
        constants.Debug[0] = 0;
        if (_config.ObjectConstantsOverride) {
            _config.ObjectConstantsOverride(constants, *proxy, view);
        }

        // PSO 指针高位作为分组键:同 PSO 命令相邻,减少状态切换。
        const uint64_t sortKey = reinterpret_cast<uintptr_t>(pso);

        // per-material descriptor set:需 GPU 系统首次懒构建;静态材质构建后缓存。
        // Gpu 为空时跳过 per-material 绑定(退回纯 push-constant 路径)。
        render::DescriptorSet* materialSet = nullptr;
        if (_config.Gpu != nullptr) {
            materialSet = proxy->GetMaterialDescriptorSet(_config.Gpu);
        }
        const render::DescriptorSetIndex materialSetIndex = proxy->GetMaterialSetIndex();

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
            cmd.PushConstantData.resize(pushSize);
            std::memcpy(cmd.PushConstantData.data(), &constants, pushSize);
            if (_config.ViewDescriptorSet != nullptr) {
                cmd.DescriptorSets[cmd.DescriptorSetCount++] = BoundDescriptorSet{
                    _config.ViewDescriptorSetIndex,
                    _config.ViewDescriptorSet};
            }
            if (materialSet != nullptr) {
                cmd.DescriptorSets[cmd.DescriptorSetCount++] = BoundDescriptorSet{materialSetIndex, materialSet};
            }
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

void SceneRenderer::Cull(const Scene& scene, const SceneView& view, VisiblePrimitiveList& out) {
    // 最小化:不做视锥裁剪,收集全部可渲染代理。view 预留给后续裁剪/relevance。
    (void)view;
    out.Clear();
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene.GetPrimitives()) {
        if (proxy == nullptr || !proxy->IsRenderable()) {
            continue;
        }
        out.Primitives.push_back(proxy.get());
    }
}

void SceneRenderer::DrawRenderers(
    render::GraphicsCommandEncoder* encoder,
    const VisiblePrimitiveList& visible,
    const SceneView& view,
    const MeshPassProcessor::Config& processorConfig,
    const PrimitiveFilter& filter) {
    if (encoder == nullptr) {
        return;
    }
    _drawCommands.clear();
    MeshPassProcessor processor{processorConfig};
    processor.BuildCommands(visible, view, _drawCommands, filter);
    if (_drawCommands.empty()) {
        return;
    }
    MeshPassProcessor::SortCommands(_drawCommands);

    // 顺序录制。跟踪上次绑定的 PSO/RootSig,排序后同状态命令相邻,跳过冗余绑定。
    render::GraphicsPipelineState* boundPso = nullptr;
    render::RootSignature* boundRootSig = nullptr;
    // 跟踪每个 set 槽上次绑定的 descriptor set,跳过冗余 BindDescriptorSet。
    std::array<render::DescriptorSet*, MeshDrawCommand::MaxBoundSets> boundSets{};
    for (const MeshDrawCommand& cmd : _drawCommands) {
        if (cmd.RootSig != boundRootSig) {
            encoder->BindRootSignature(cmd.RootSig);
            boundRootSig = cmd.RootSig;
            // RootSig 切换后 descriptor set 绑定会失效,重置跟踪。
            boundSets.fill(nullptr);
        }
        if (cmd.Pso != boundPso) {
            encoder->BindGraphicsPipelineState(cmd.Pso);
            boundPso = cmd.Pso;
        }
        for (uint32_t i = 0; i < cmd.DescriptorSetCount; ++i) {
            const BoundDescriptorSet& bound = cmd.DescriptorSets[i];
            if (bound.Handle == nullptr) {
                continue;
            }
            const uint32_t slot = bound.Set.Value;
            if (slot < boundSets.size() && boundSets[slot] == bound.Handle) {
                continue;  // 同一 set 已绑,跳过。
            }
            encoder->BindDescriptorSet(bound.Set, bound.Handle);
            if (slot < boundSets.size()) {
                boundSets[slot] = bound.Handle;
            }
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

namespace {

constexpr float kShadowNearPlane = 0.05f;
constexpr float kMaxSpotFov = 2.96706f;  // ~170 deg, keeps perspective projection well-conditioned.

SceneView MakePerspectiveSlice(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& forward,
    const Eigen::Vector3f& up,
    float fovY,
    float nearZ,
    float farZ,
    uint32_t resolution) noexcept {
    SceneView view{};
    view.ViewMatrix = LookAtFrontLH(position, forward, up);
    view.ProjMatrix = PerspectiveLH<float>(fovY, 1.0f, nearZ, farZ);
    view.ViewProjMatrix = view.ProjMatrix * view.ViewMatrix;
    view.EyePosition = position;
    view.ViewportWidth = resolution;
    view.ViewportHeight = resolution;
    return view;
}

}  // namespace

float AdditionalShadowKernelRadius(ShadowSoftMode mode) noexcept {
    switch (mode) {
        case ShadowSoftMode::Low:
            return 1.5f;
        case ShadowSoftMode::Medium:
            return 2.5f;
        default:
            return 1.0f;
    }
}

Eigen::Vector3f PointShadowFaceForward(uint32_t face) noexcept {
    // Order matches shader cube_face_id: +X,-X,+Y,-Y,+Z,-Z.
    switch (face) {
        case 0: return Eigen::Vector3f{1.0f, 0.0f, 0.0f};
        case 1: return Eigen::Vector3f{-1.0f, 0.0f, 0.0f};
        case 2: return Eigen::Vector3f{0.0f, 1.0f, 0.0f};
        case 3: return Eigen::Vector3f{0.0f, -1.0f, 0.0f};
        case 4: return Eigen::Vector3f{0.0f, 0.0f, 1.0f};
        default: return Eigen::Vector3f{0.0f, 0.0f, -1.0f};
    }
}

Eigen::Vector3f PointShadowFaceUp(uint32_t face) noexcept {
    // Up must not be parallel to forward; +Y/-Y use Z-axis up, the rest use +Y up.
    switch (face) {
        case 2: return Eigen::Vector3f{0.0f, 0.0f, -1.0f};
        case 3: return Eigen::Vector3f{0.0f, 0.0f, 1.0f};
        default: return Eigen::Vector3f{0.0f, 1.0f, 0.0f};
    }
}

AdditionalShadowSlice BuildSpotShadowSlice(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& direction,
    float outerAngle,
    float range,
    uint32_t resolution,
    float depthBiasTexels,
    float normalBiasTexels,
    ShadowSoftMode softMode) noexcept {
    Eigen::Vector3f forward = direction;
    const float len = forward.norm();
    forward = len > 1e-6f ? (forward / len).eval() : Eigen::Vector3f{0.0f, -1.0f, 0.0f};
    const Eigen::Vector3f up = std::abs(forward.y()) > 0.99f
        ? Eigen::Vector3f{0.0f, 0.0f, 1.0f}
        : Eigen::Vector3f{0.0f, 1.0f, 0.0f};

    const float safeRange = std::max(range, kShadowNearPlane + 0.1f);
    const float halfAngle = std::clamp(outerAngle, 0.01f, kMaxSpotFov * 0.5f);
    const float fovY = std::min(2.0f * halfAngle, kMaxSpotFov);
    const float nearZ = std::min(kShadowNearPlane, safeRange * 0.5f);

    AdditionalShadowSlice slice{};
    slice.View = MakePerspectiveSlice(position, forward, up, fovY, nearZ, safeRange, resolution);
    slice.LightDirectionForBias = -forward;

    // texel world size at the far plane: frustum width / resolution. Mirrors the directional path so the
    // same per-light bias values stay meaningful across light types.
    const float texelWorldSize = (2.0f * std::tan(halfAngle) * safeRange) / static_cast<float>(std::max(resolution, 1u));
    const float kernelRadius = AdditionalShadowKernelRadius(softMode);
    slice.DepthBias = -std::max(depthBiasTexels, 0.0f) * texelWorldSize * kernelRadius;
    slice.NormalBias = -std::max(normalBiasTexels, 0.0f) * texelWorldSize * kernelRadius;
    return slice;
}

AdditionalShadowSlice BuildPointShadowFaceSlice(
    const Eigen::Vector3f& position,
    uint32_t face,
    float range,
    uint32_t resolution,
    float depthBiasTexels,
    ShadowSoftMode softMode) noexcept {
    const Eigen::Vector3f forward = PointShadowFaceForward(face);
    const Eigen::Vector3f up = PointShadowFaceUp(face);
    const float safeRange = std::max(range, kShadowNearPlane + 0.1f);
    const float fovY = 1.5707964f;  // 90 deg, each face covers one sixth of the sphere.
    const float nearZ = std::min(kShadowNearPlane, safeRange * 0.5f);

    AdditionalShadowSlice slice{};
    slice.View = MakePerspectiveSlice(position, forward, up, fovY, nearZ, safeRange, resolution);
    slice.LightDirectionForBias = -forward;

    // 90 deg FOV → half angle 45 deg → tan = 1, frustum width at far = 2 * range.
    const float texelWorldSize = (2.0f * safeRange) / static_cast<float>(std::max(resolution, 1u));
    const float kernelRadius = AdditionalShadowKernelRadius(softMode);
    slice.DepthBias = -std::max(depthBiasTexels, 0.0f) * texelWorldSize * kernelRadius;
    // Point lights force normalBias=0 (matches URP/Built-in): the per-face forward is a poor normal proxy.
    slice.NormalBias = 0.0f;
    return slice;
}

bool BuildAdditionalShadows(
    const Scene& scene,
    uint32_t resolution,
    ShadowSoftMode softMode,
    AdditionalShadowData& out) {
    out.Clear();
    out.Resolution = std::max(resolution, 1u);
    out.SoftMode = softMode;

    uint32_t sliceCursor = 0;
    for (const unique_ptr<LightSceneProxy>& light : scene.GetLights()) {
        if (light == nullptr || !light->GetCastShadow()) {
            continue;
        }
        const LightType type = light->GetLightType();
        if (type != LightType::Spot && type != LightType::Point) {
            continue;
        }

        const uint32_t needed = (type == LightType::Point) ? AdditionalShadowData::PointFaceCount : 1u;
        if (sliceCursor + needed > AdditionalShadowData::MaxSlices) {
            // Atlas full: skip the remaining casters (URP scales resolution down instead; minimal path drops them).
            continue;
        }

        const Eigen::Vector3f position = light->GetPosition();
        const float range = std::max(light->GetRange(), kShadowNearPlane + 0.1f);
        const float depthBiasTexels = light->GetShadowDepthBias();
        const float normalBiasTexels = light->GetShadowNormalBias();

        AdditionalShadowLight record{};
        record.Light = light.get();
        record.FirstSlice = sliceCursor;
        record.SliceCount = needed;
        record.Position = position;
        record.Range = range;

        if (type == LightType::Spot) {
            record.Kind = AdditionalShadowKind::Spot;
            record.Direction = light->GetDirection();
            out.Slices[sliceCursor] = BuildSpotShadowSlice(
                position,
                light->GetDirection(),
                light->GetSpotOuterAngle(),
                range,
                out.Resolution,
                depthBiasTexels,
                normalBiasTexels,
                softMode);
        } else {
            record.Kind = AdditionalShadowKind::Point;
            record.Direction = Eigen::Vector3f{0.0f, -1.0f, 0.0f};
            for (uint32_t face = 0; face < AdditionalShadowData::PointFaceCount; ++face) {
                out.Slices[sliceCursor + face] = BuildPointShadowFaceSlice(
                    position,
                    face,
                    range,
                    out.Resolution,
                    depthBiasTexels,
                    softMode);
            }
        }

        out.Lights.push_back(record);
        sliceCursor += needed;
    }

    out.SliceCount = sliceCursor;
    out.Enabled = sliceCursor > 0;
    return out.Enabled;
}

}  // namespace radray
