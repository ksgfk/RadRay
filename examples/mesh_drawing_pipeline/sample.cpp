#include "sample.h"

#include <numbers>
#include <charconv>
#include <cstring>

#include <radray/logger.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/mesh_batch.h>
#include <radray/runtime/scene.h>

#include "gpu_support.h"
#include "pipeline.h"

// 阅读入口：RunSample 准备场景并运行多槽帧循环；Processor 与 MeshDrawingFramePipeline 在 pipeline.cpp。
// UE 对照基线：本地 UnrealEngine 5.8.4，以下源码位置均相对 Engine/Source/Runtime。
// 构建和运行入口见 docs/guide/build-test.md 的 Mesh Drawing Pipeline 教学样例。

namespace radray::examples::mesh_drawing {
int RunSample(render::RenderBackend backend, const std::filesystem::path& shaderDirectory,
              const std::filesystem::path& output, uint32_t slotCount, uint32_t frameCount) {
    if (slotCount == 0 || frameCount < 2) return 2;
    if (!PrepareOutputDirectory(output)) return 2;
    GpuResources gpu;
    if (!gpu.Initialize(backend, shaderDirectory)) return 3;

    // 步骤 1：准备一份立方体几何，再把索引分成两个 Section。
    // 为什么：submesh 是索引范围与材质槽，所有立方体和 Section 都能共享同一份顶点缓冲。
    // UE5 对照：Engine/Public/StaticMeshResources.h 的 FStaticMeshLODResources / FStaticMeshSection。
    // 本例简化：八个 POSITION 顶点、36 个索引，无资产加载、法线、UV 或 LOD；材质用纯色表示。
    const array<array<float, 3>, 8> vertices{{{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}}};
    const array<uint32_t, 36> indices{
        0, 3, 2, 0, 2, 1,   // 前面
        4, 5, 6, 4, 6, 7,   // 后面
        0, 4, 7, 0, 7, 3,   // 左面
        1, 2, 6, 1, 6, 5,   // 右面
        0, 1, 5, 0, 5, 4,   // 底面：前 30 个索引使用材质槽 0
        3, 7, 6, 3, 6, 2};  // 顶面：最后 6 个索引使用材质槽 1
    const array<MeshSection, 2> sections{
        MeshSection{.MaterialSlot = 0, .FirstIndex = 0, .IndexCount = 30},
        MeshSection{.MaterialSlot = 1, .FirstIndex = 30, .IndexCount = 6}};
    if (!gpu.PrepareGeometry(std::as_bytes(std::span{vertices}), indices)) return 3;

    VertexInput vertexInput;
    vertexInput.Layouts.push_back({0, sizeof(vertices[0]), render::VertexStepMode::Vertex});
    vertexInput.Layouts.push_back({1, sizeof(InstanceData), render::VertexStepMode::Instance});
    vertexInput.Attributes.push_back({0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0});
    for (uint32_t column = 0; column < 4; ++column) {
        vertexInput.Attributes.push_back({1, column * 16, "INST_COL", column, render::VertexFormat::FLOAT32X4, column + 1});
    }
    VertexInputStream stream;
    stream.Buffer = gpu.VertexBuffer.Get();
    stream.Size = sizeof(vertices);
    vertexInput.Streams.push_back(stream);
    if (!gpu.CreatePipelines(vertexInput, gpu.OpaqueResource) ||
        !gpu.CreatePipelines(vertexInput, gpu.TwoSidedResource) ||
        !gpu.CreatePipelines(vertexInput, gpu.TransparentResource)) return 3;
    RADRAY_INFO_LOG("步骤 1：一份顶点/索引缓冲，两个 Section，已准备 Depth/Color/Outline/Transparent 所需 PSO");

    // 步骤 M2：声明材质参数默认值，再创建只覆盖参数的 MaterialInstance。
    // 为什么：蓝色和绿色可以共享 shader 与固定性质，改变颜色无需复制几何或重新创建 PSO。
    // UE5 对照：Engine/Public/Materials/Material.h 的 UMaterial；MaterialInstance.h 的 UMaterialInstance；
    // MaterialInstanceDynamic.h 的 UMaterialInstanceDynamic::SetScalarParameterValue。
    // 本例简化：无 UObject/材质图/多层父实例；双面描边和透明各由独立 Resource 表达，参数不改变 shader 变体。
    const array<MaterialParameter, 2> declarations{{{"BaseColor", array<float, 4>{0.72f, 0.25f, 0.09f, 1}},
                                                    {"Brightness", 1.0f}}};
    const auto opaqueMaterial = Material::Create(gpu.OpaqueResource, declarations);
    const auto twoSidedMaterial = Material::Create(gpu.TwoSidedResource, declarations);
    const auto transparentMaterial = Material::Create(gpu.TransparentResource, declarations);
    if (!opaqueMaterial || !twoSidedMaterial || !transparentMaterial) return 4;
    const auto baseColorIndex = opaqueMaterial->FindParameter("BaseColor");
    const auto brightnessIndex = opaqueMaterial->FindParameter("Brightness");
    if (!baseColorIndex || !brightnessIndex) return 4;
    const MaterialParameterIndices parameters{*baseColorIndex, *brightnessIndex};
    array<MaterialInstance, 8> instances{
        MaterialInstance{*opaqueMaterial}, MaterialInstance{*opaqueMaterial},
        MaterialInstance{*opaqueMaterial}, MaterialInstance{*opaqueMaterial},
        MaterialInstance{*twoSidedMaterial}, MaterialInstance{*twoSidedMaterial},
        MaterialInstance{*transparentMaterial}, MaterialInstance{*transparentMaterial}};
    if (!instances[1].SetParameter(parameters.BaseColor, array<float, 4>{0.12f, 0.58f, 0.88f, 1}) ||
        !instances[2].SetParameter(parameters.BaseColor, array<float, 4>{0.18f, 0.68f, 0.30f, 1}) ||
        !instances[3].SetParameter(parameters.BaseColor, array<float, 4>{0.55f, 0.90f, 0.18f, 1}) ||
        !instances[4].SetParameter(parameters.BaseColor, array<float, 4>{0.54f, 0.24f, 0.75f, 1}) ||
        !instances[5].SetParameter(parameters.BaseColor, array<float, 4>{0.92f, 0.42f, 0.62f, 1}) ||
        !instances[6].SetParameter(parameters.BaseColor, array<float, 4>{0.08f, 0.70f, 1.0f, 0.38f}) ||
        !instances[7].SetParameter(parameters.BaseColor, array<float, 4>{0.25f, 0.90f, 1.0f, 0.48f}) ||
        !instances[2].SetParameter(parameters.Brightness, 0.5f)) return 4;

    // 步骤 2：登记五个 Primitive，给每个 Section 对应的 Batch 设置样例 Pass 参与规则。
    // 为什么：候选物体可以相同，Pass 接受的 Batch 却不同；同一 Primitive 的不同 Section 也可以走不同路径。
    // UE5 对照：Renderer/Public/PrimitiveSceneInfo.h 的 FPrimitiveSceneInfo，Engine/Public/MeshBatch.h 的 FMeshBatch；
    // Engine/Public/Materials/MaterialRenderProxy.h 的 FMaterialRenderProxy 表达材质身份与参数访问。
    // Engine/Private/StaticMeshSceneProxy.cpp 的 GetMeshElement / DrawStaticElements 把 Section 组织为 Batch。
    // 本例简化：BatchPassPolicy 单独保存在样例中；Scene 只拥有 Proxy，用户材质和实例在外部存活。
    Scene scene;
    array<SparseSetHandle, 8> materials;
    for (size_t index = 0; index < materials.size(); ++index) {
        MaterialRenderProxy proxy;
        proxy.Update(instances[index]);
        materials[index] = scene.AddMaterial(std::move(proxy));
    }

    struct CubeDescription {
        std::string_view Name;
        Eigen::Vector3f Position;
        float Scale;
        array<PassParticipation, 2> SectionPasses;
        array<uint32_t, 2> MaterialSlots;
    };
    const array<CubeDescription, 5> cubes{{{"A-正常物体", {-1.40f, -0.10f, 0.10f}, 1.25f, {PassParticipation::DepthAndColor, PassParticipation::DepthAndColor}, {0, 1}},
                                           {"B-只写深度的遮挡体", {-0.65f, 0.33f, -0.97f}, 0.42f, {PassParticipation::DepthOnly, PassParticipation::DepthOnly}, {0, 1}},
                                           {"C-只参加颜色", {0.20f, -0.10f, 0.15f}, 1.05f, {PassParticipation::ColorOnly, PassParticipation::ColorOnly}, {2, 3}},
                                           {"D-Section 规则不同且描边", {1.75f, -0.10f, 0.45f}, 1.05f, {PassParticipation::DepthAndColor, PassParticipation::ColorOnly}, {4, 5}},
                                           {"E-透明叠加", {0.72f, -0.10f, -0.95f}, 1.10f, {PassParticipation::DepthAndColor, PassParticipation::DepthAndColor}, {6, 7}}}};
    vector<SparseSetHandle> candidates;
    vector<BatchPassPolicy> policies;
    for (const auto& cube : cubes) {
        const auto handle = scene.AddPrimitive();
        auto& primitive = scene.Primitives.Get(handle);
        primitive.LocalToWorld = AffineTransform{ComposeTransform(
            cube.Position, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Constant(cube.Scale).eval())};
        primitive.LocalBounds = Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
            const auto& section = sections[sectionIndex];
            MeshBatch batch;
            batch.Material = materials[cube.MaterialSlots[section.MaterialSlot]];
            batch.VertexInput = &vertexInput;
            batch.Elements.push_back(MeshBatchElement{
                .IndexBuffer = gpu.IndexBuffer.Get(),
                .FirstIndex = section.FirstIndex,
                .IndexCount = section.IndexCount,
                .VertexOffset = section.VertexOffset});
            const auto batchHandle = scene.AddMeshBatch(handle, std::move(batch));
            if (!batchHandle.IsValid()) return 4;
            policies.push_back({batchHandle, cube.SectionPasses[sectionIndex], cube.Name});
        }
        candidates.push_back(handle);
    }
    RADRAY_INFO_LOG("步骤 2：{} 个 Primitive、{} 个 Batch，共享几何；演示 Batch 参与规则、材质标签筛选、模板描边和透明混合",
                    scene.Primitives.Count(), scene.Meshes.Count());

    // 步骤 3：将相机与 viewport 作为本次 Pass 的输入。
    // 为什么：场景可以被多个相机/Pass 使用，不应把最终 ViewProjection 固定在 MeshBatch 里。
    // UE5 对照：Engine/Public/SceneView.h 的 FSceneView、Renderer/Private/SceneRendering.h 的 FViewInfo；
    // Renderer/Public/MeshPassProcessor.h 的 FMeshPassProcessor 接收 view/pass 上下文。
    // 本例简化：一个固定相机、800×600 离屏附件；Vulkan 用负 viewport 高度对齐 D3D12 的图片方向。
    const auto view = LookAtLH(Eigen::Vector3f{3.6f, 2.8f, -7.0f}, Eigen::Vector3f{0, 0, 0}, Eigen::Vector3f{0, 1, 0});
    const auto projection = PerspectiveLH(std::numbers::pi_v<float> / 4, float(gpu.Width) / float(gpu.Height), 0.1f, 100.0f);
    PassContext pass{projection * view,
                     {0, 0, float(gpu.Width), float(gpu.Height), 0, 1},
                     {0, 0, gpu.Width, gpu.Height}};
    if (backend == render::RenderBackend::Vulkan) {
        pass.ViewportRect.Y = float(gpu.Height);
        pass.ViewportRect.Height = -float(gpu.Height);
    }
    RADRAY_INFO_LOG("步骤 3：创建固定视图，候选 Primitive 全部可见");

    auto pipeline = make_unique<MeshDrawingFramePipeline>(gpu, scene, pass, candidates, policies, parameters, slotCount);
    if (!pipeline->Initialize()) return 3;
    auto* drawing = pipeline.get();
    auto scheduler = FrameScheduler::Create(*gpu.Device, std::move(pipeline), slotCount);
    if (!scheduler) return 3;
    RADRAY_INFO_LOG("多槽帧循环：{} 个 Slot，连续渲染 {} 帧；Frame 1 更新绿色材质，结束后保存最后一帧", slotCount, frameCount);
    scheduler->RunFrame(scene);

    vector<InlineVector<CachedMeshDrawCommandInfo, 4>> originalCommands;
    MaterialConstants originalGreen;
    for (const auto& policy : policies) {
        const auto& batch = scene.Meshes.Get(policy.Batch);
        originalCommands.push_back(batch.CachedCommands);
        if (batch.Material != materials[2]) continue;
        for (const auto& info : batch.CachedCommands) {
            if (info.Pass != MeshPassId{1}) continue;
            const auto& push = scene.GetCachedDrawList(info.Pass)->Commands.Get(info.CommandHandle).PushConstants;
            if (!push || push->Data.size() != sizeof(originalGreen)) return 4;
            std::memcpy(&originalGreen, push->Data.data(), sizeof(originalGreen));
        }
    }
    if (originalGreen.Color != array<float, 4>{0.09f, 0.34f, 0.15f, 1.0f}) return 4;
    // 步骤 M3：只更新绿色侧面的 Proxy；Scene 只标脏引用该材质的 Batch。
    // UE5 对照：材质渲染代理更新使相关静态 MeshDrawCommand 失效；视图常量不进入缓存。
    if (!instances[2].SetParameter(parameters.Brightness, 1.0f)) return 4;
    RADRAY_INFO_LOG("材质同步前：实例 Brightness={}，Proxy 仍为 {}",
                    std::get<float>(*instances[2].GetParameter(parameters.Brightness)),
                    std::get<float>(*scene.Materials.Get(materials[2]).GetParameter(parameters.Brightness)));
    MaterialRenderProxy editedProxy;
    editedProxy.Update(instances[2]);
    if (!scene.UpdateMaterial(materials[2], std::move(editedProxy))) return 4;
    RADRAY_INFO_LOG("材质同步后：{} 个 Batch 标脏，八个实例共享三份 Resource 和六个 PSO", scene.GetDirtyBatches().size());
    scheduler->RunFrame(scene);
    if (scene.HasDirtyBatches()) return 4;
    size_t rebuiltBatches = 0;
    for (size_t index = 0; index < policies.size(); ++index) {
        const auto& batch = scene.Meshes.Get(policies[index].Batch);
        const auto& previous = originalCommands[index];
        if (previous.size() != batch.CachedCommands.size()) return 4;
        const bool edited = batch.Material == materials[2];
        bool changed = false;
        for (size_t commandIndex = 0; commandIndex < previous.size(); ++commandIndex) {
            const bool sameHandle = previous[commandIndex].CommandHandle == batch.CachedCommands[commandIndex].CommandHandle;
            if (sameHandle == edited) return 4;
            changed |= !sameHandle;
            const auto& info = batch.CachedCommands[commandIndex];
            if (edited && info.Pass == MeshPassId{1}) {
                const auto& push = scene.GetCachedDrawList(info.Pass)->Commands.Get(info.CommandHandle).PushConstants;
                MaterialConstants updated;
                if (!push || push->Data.size() != sizeof(updated)) return 4;
                std::memcpy(&updated, push->Data.data(), sizeof(updated));
                for (size_t channel = 0; channel < 3; ++channel) {
                    if (updated.Color[channel] != originalGreen.Color[channel] * 2) return 4;
                }
                if (updated.Color[3] != originalGreen.Color[3]) return 4;
            }
        }
        rebuiltBatches += changed ? 1 : 0;
    }
    if (rebuiltBatches != 1) return 4;
    RADRAY_INFO_LOG("增量缓存验证：仅绿色材质的 {} 个 Batch 重建，其余 {} 个 Batch 的 CommandHandle 保持不变", rebuiltBatches, policies.size() - rebuiltBatches);
    RADRAY_INFO_LOG("命令颜色验证：绿色材质 Brightness 从 0.5 更新为 1.0，缓存 PushConstants 的 RGB 已翻倍，alpha 保持不变");

    for (uint32_t frame = 2; frame < frameCount; ++frame) {
        scheduler->RunFrame(scene);
    }
    scheduler->WaitIdle();
    if (!drawing->SaveImage((frameCount - 1) % slotCount, output)) return 6;
    RADRAY_INFO_LOG("多槽帧循环完成：{} 帧、{} 个 Slot，所有在途帧已完成并重置录制存储", frameCount, slotCount);
    return 0;
}

int SampleMain(int argc, char** argv) {
#if defined(_WIN32)
    auto backend = render::RenderBackend::D3D12;
#else
    auto backend = render::RenderBackend::Vulkan;
#endif
    const std::filesystem::path root{RADRAY_SAMPLE_ROOT};
    auto shaderDirectory = root / "build/mesh_drawing_pipeline/shaders";
    std::filesystem::path output;
    uint32_t slotCount = 2;
    uint32_t frameCount = 6;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--help" || option == "-h") {
            fmt::print(
                "用法：radray_mesh_drawing_sample [--backend d3d12|vulkan] [--shader-dir 目录] [--output 图片.png] [--slots 槽数] [--frames 帧数]\n"
                "默认双槽六帧；槽数必须大于 0，帧数至少为 2。先运行 examples/mesh_drawing_pipeline/compile_shaders.py。\n");
            return 0;
        }
        if (option != "--backend" && option != "--shader-dir" && option != "--output" && option != "--slots" && option != "--frames") {
            RADRAY_ERR_LOG("未知参数：{}", option);
            return 2;
        }
        if (++i >= argc || std::string_view{argv[i]}.empty() || std::string_view{argv[i]}.starts_with("--")) {
            RADRAY_ERR_LOG("参数 {} 缺少有效值", option);
            return 2;
        }
        const std::string_view value{argv[i]};
        if (option == "--backend") {
            if (value == "d3d12")
                backend = render::RenderBackend::D3D12;
            else if (value == "vulkan")
                backend = render::RenderBackend::Vulkan;
            else {
                RADRAY_ERR_LOG("不支持的 backend={}，可选 d3d12 或 vulkan", value);
                return 2;
            }
        } else if (option == "--slots" || option == "--frames") {
            uint32_t count = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count == 0 || (option == "--frames" && count < 2)) {
                RADRAY_ERR_LOG("参数 {} 的值无效：{}；槽数必须大于 0，帧数至少为 2", option, value);
                return 2;
            }
            if (option == "--slots")
                slotCount = count;
            else
                frameCount = count;
        } else if (option == "--shader-dir")
            shaderDirectory = argv[i];
        else
            output = argv[i];
    }
    if (output.empty()) {
        output = root / "build/mesh_drawing_pipeline" /
                 (backend == render::RenderBackend::Vulkan ? "vulkan.png" : "d3d12.png");
    }
    return RunSample(backend, shaderDirectory, output, slotCount, frameCount);
}

}  // namespace radray::examples::mesh_drawing

int main(int argc, char** argv) {
    return radray::examples::mesh_drawing::SampleMain(argc, argv);
}
