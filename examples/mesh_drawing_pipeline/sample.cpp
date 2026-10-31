#include "sample.h"

#include <numbers>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/runtime/mesh_section.h>
#include <radray/runtime/scene.h>

#include "gpu_support.h"
#include "pipeline.h"

// 阅读入口：RunSample 按步骤 1～8 展示一帧；Processor、输出 Context 和 RecordDrawCommands 在 pipeline.cpp。
// UE 对照基线：本地 UnrealEngine 5.8.4，以下源码位置均相对 Engine/Source/Runtime。
// 构建和运行入口见 docs/guide/build-test.md 的 Mesh Drawing Pipeline 教学样例。

namespace radray::examples::mesh_drawing {
namespace {

void BuildMeshDrawCommandList(
    const Scene& scene, std::span<const SparseSetHandle> primitives, MeshPassProcessor& processor) {
    // 步骤 4：样例遍历候选 Primitive 及其 Batch，解析句柄后调用 Processor。
    // 为什么：候选来源与场景遍历由调用方组织，runtime 从单个 Batch 的处理开始。
    // UE5 对照：Renderer/Private/MeshDrawCommands.cpp 组织候选 Batch，交给 FMeshPassProcessor::AddMeshBatch。
    // 本例简化：全部候选可见；失效句柄记录错误并跳过，保留已输出命令，继续后续候选。
    for (const auto primitiveHandle : primitives) {
        if (!scene.Primitives.IsAlive(primitiveHandle)) {
            RADRAY_ERR_LOG("Invalid primitive handle: {}", primitiveHandle);
            continue;
        }
        const auto& primitive = scene.Primitives.Get(primitiveHandle);
        for (const auto batchHandle : primitive.Meshes) {
            if (!scene.Meshes.IsAlive(batchHandle)) {
                RADRAY_ERR_LOG("Invalid mesh batch handle: {}", batchHandle);
                continue;
            }
            const auto& batch = scene.Meshes.Get(batchHandle);
            if (!scene.Materials.IsAlive(batch.Material)) {
                RADRAY_ERR_LOG("Invalid material handle: {}", batch.Material);
                continue;
            }
            processor.AddMeshBatch({primitive, batch, primitiveHandle, batchHandle});
        }
    }
}

}  // namespace

int RunSample(render::RenderBackend backend, const std::filesystem::path& shaderDirectory,
              const std::filesystem::path& output) {
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
    if (!gpu.UploadGeometry(std::as_bytes(std::span{vertices}), indices)) return 3;

    VertexInput vertexInput;
    vertexInput.Layouts.push_back({0, sizeof(vertices[0]), render::VertexStepMode::Vertex});
    vertexInput.Attributes.push_back({0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0});
    VertexInputStream stream;
    stream.Buffer = gpu.VertexBuffer.Get();
    stream.Size = sizeof(vertices);
    vertexInput.Streams.push_back(stream);
    if (!gpu.CreatePipelines(vertexInput)) return 3;
    RADRAY_INFO_LOG("步骤 1：一份顶点/索引缓冲，两个 Section，已准备 Depth/Color PSO");

    // 步骤 2：登记四个 Primitive，给每个 Section 对应的 Batch 设置样例 Pass 参与规则。
    // 为什么：候选物体可以相同，Pass 接受的 Batch 却不同；同一 Primitive 的不同 Section 也可以走不同路径。
    // UE5 对照：Renderer/Public/PrimitiveSceneInfo.h 的 FPrimitiveSceneInfo，Engine/Public/MeshBatch.h 的 FMeshBatch；
    // Engine/Public/Materials/MaterialRenderProxy.h 的 FMaterialRenderProxy 表达材质身份与参数访问。
    // Engine/Private/StaticMeshSceneProxy.cpp 的 GetMeshElement / DrawStaticElements 把 Section 组织为 Batch。
    // 本例简化：BatchPassPolicy 单独保存在样例中，不修改 runtime；材质句柄仍然只映射纯色。
    Scene scene;
    array<SparseSetHandle, 6> materials;
    for (auto& material : materials) material = scene.Materials.Emplace();
    const array<MaterialColor, 6> colors{{{materials[0], {0.72f, 0.25f, 0.09f, 1}},
                                          {materials[1], {0.12f, 0.58f, 0.88f, 1}},
                                          {materials[2], {0.18f, 0.68f, 0.30f, 1}},
                                          {materials[3], {0.55f, 0.90f, 0.18f, 1}},
                                          {materials[4], {0.54f, 0.24f, 0.75f, 1}},
                                          {materials[5], {0.92f, 0.42f, 0.62f, 1}}}};
    struct CubeDescription {
        std::string_view Name;
        Eigen::Vector3f Position;
        float Scale;
        array<PassParticipation, 2> SectionPasses;
        array<uint32_t, 2> MaterialSlots;
    };
    const array<CubeDescription, 4> cubes{{{"A-正常物体", {-1.40f, -0.10f, 0.10f}, 1.25f, {PassParticipation::DepthAndColor, PassParticipation::DepthAndColor}, {0, 1}},
                                           {"B-只写深度的遮挡体", {-0.65f, 0.33f, -0.97f}, 0.42f, {PassParticipation::DepthOnly, PassParticipation::DepthOnly}, {0, 1}},
                                           {"C-只参加颜色", {0.20f, -0.10f, 0.15f}, 1.05f, {PassParticipation::ColorOnly, PassParticipation::ColorOnly}, {2, 3}},
                                           {"D-Section 规则不同", {1.75f, -0.10f, 0.45f}, 1.05f, {PassParticipation::DepthAndColor, PassParticipation::ColorOnly}, {4, 5}}}};
    vector<SparseSetHandle> candidates;
    vector<BatchPassPolicy> policies;
    for (const auto& cube : cubes) {
        const auto handle = scene.Primitives.Emplace();
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
            const auto batchHandle = scene.Meshes.Emplace(std::move(batch));
            primitive.Meshes.push_back(batchHandle);
            policies.push_back({batchHandle, cube.SectionPasses[sectionIndex], cube.Name});
        }
        candidates.push_back(handle);
    }
    RADRAY_INFO_LOG("步骤 2：{} 个 Primitive、{} 个 Batch，共享几何；分别演示正常、只写深度、只画颜色和 Section 混合规则",
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

    // 步骤 4～6：样例遍历同一候选列表，交给两个 Processor，命令分别写入各自的 DrawListContext。
    // 为什么：AddMeshBatch 返回 void；输出存储独立于筛选逻辑，构建结束后从 Context 取出本次结果。
    // UE5 对照：Renderer/Private/MeshDrawCommands.cpp 遍历候选 Batch；
    // Renderer/Public/MeshPassProcessor.h 的 FMeshPassProcessor 持有 FMeshPassDrawListContext。
    // 本例简化：每次新建输出 Context，无静态命令缓存；参数和录制仍留在样例中。
    SampleDrawListContext depthOutput{pass, "Depth", policies};
    SampleDrawListContext colorOutput{pass, "Color", policies, colors};
    DepthPassProcessor depthProcessor{depthOutput, *gpu.DepthPipeline, policies};
    ColorPassProcessor colorProcessor{colorOutput, *gpu.ColorPipeline, policies};
    BuildMeshDrawCommandList(scene, candidates, depthProcessor);
    BuildMeshDrawCommandList(scene, candidates, colorProcessor);
    if (depthOutput.HasErrors || colorOutput.HasErrors) return 4;
    const auto& depthCommands = depthOutput.Pass;
    const auto& colorCommands = colorOutput.Pass;
    RADRAY_INFO_LOG("步骤 6：Depth={}，Color={}，总计 {} 条单实例 Draw；接下来才录制 RHI",
                    depthCommands.Commands.size(), colorCommands.Commands.size(),
                    depthCommands.Commands.size() + colorCommands.Commands.size());

    // 步骤 7：主流程负责资源状态和 Pass 边界，执行器只负责每条 Draw 的绑定与绘制。
    // 为什么：Depth Pass 的结果要保存；ColorOnly 的表面没有预深度，Color Pass 还必须允许它们测试并补写深度。
    // UE5 对照：Renderer 安排 Pass，Renderer/Private/MeshPassProcessor.cpp 的 SubmitMeshDrawCommands 录制绘制；
    // UE 通常由 RDG 安排资源依赖。
    // 本例简化：显式写出 barrier 和两个 Render Pass，便于观察；不引入 Render Graph。
    auto& command = *gpu.Command;
    command.Begin();
    auto endRecording = MakeScopeGuard([&]() noexcept { command.End(); });
    const render::ResourceBarrierDescriptor initialBuffers[]{
        render::BarrierBufferDescriptor{gpu.VertexBuffer.Get(), render::BufferState::HostWrite, render::BufferState::Vertex},
        render::BarrierBufferDescriptor{gpu.IndexBuffer.Get(), render::BufferState::HostWrite, render::BufferState::Index},
        render::BarrierBufferDescriptor{gpu.Readback.Get(), render::BufferState::Undefined, render::BufferState::CopyDestination}};
    command.ResourceBarrier(initialBuffers);
    TransitionTexture(command, *gpu.DepthTexture, render::TextureState::Undefined, render::TextureState::DepthWrite);
    RADRAY_INFO_LOG("步骤 7：录制 Depth Pass，清除并写入深度");
    auto depthEncoder = command.BeginRenderPass({.DepthStencil = render::RenderPassDepthStencilAttachment{
                                                     .View = gpu.DepthWriteView.Get(), .DepthLoad = render::LoadAction::Clear, .DepthStore = render::StoreAction::Store, .Clear = {1, 0}},
                                                 .Width = gpu.Width,
                                                 .Height = gpu.Height,
                                                 .Name = "Tutorial Depth"});
    if (!depthEncoder) return 5;
    const bool depthRecorded = RecordDrawCommands(*depthEncoder, depthCommands);
    command.EndRenderPass(depthEncoder.Release());
    if (!depthRecorded) return 5;

    // 状态仍为 DepthWrite，但 Vulkan 仍需要把上一 Pass 的深度写入同步给下一 Pass 的读写。
    TransitionTexture(command, *gpu.DepthTexture, render::TextureState::DepthWrite, render::TextureState::DepthWrite);
    TransitionTexture(command, *gpu.ColorTexture, render::TextureState::Undefined, render::TextureState::RenderTarget);
    RADRAY_INFO_LOG("步骤 7：录制 Color Pass，Load 同一深度，LessEqual 测试并补写深度");
    const render::RenderPassColorAttachment colorAttachment{
        gpu.ColorView.Get(), render::LoadAction::Clear, render::StoreAction::Store, {{0.035f, 0.055f, 0.085f, 1}}};
    auto colorEncoder = command.BeginRenderPass({.Colors = std::span{&colorAttachment, 1},
                                                 .DepthStencil = render::RenderPassDepthStencilAttachment{
                                                     .View = gpu.DepthWriteView.Get(), .DepthLoad = render::LoadAction::Load, .DepthStore = render::StoreAction::Store},
                                                 .Width = gpu.Width,
                                                 .Height = gpu.Height,
                                                 .Name = "Tutorial Color"});
    if (!colorEncoder) return 5;
    const bool colorRecorded = RecordDrawCommands(*colorEncoder, colorCommands);
    command.EndRenderPass(colorEncoder.Release());
    if (!colorRecorded) return 5;

    // 步骤 8：将颜色复制到 Readback，提交并等待 GPU，然后保存图片。
    // 为什么：CommandBuffer.End 只结束 CPU 录制；Submit 后仍须等待，CPU 才能安全读取结果。
    // UE5 对照：RHI/Public/RHICommandList.h 的 FRHICommandListImmediate 提交接口；离屏读回在 Mesh Drawing Pipeline 之后。
    // 本例简化：只绘制一帧并立即 Wait；实际引擎通常多帧并行，不能每个 Pass 都等待。
    TransitionTexture(command, *gpu.ColorTexture, render::TextureState::RenderTarget, render::TextureState::CopySource);
    command.CopyTextureToBuffer(gpu.Readback.Get(), 0, gpu.ColorTexture.Get(), {0, 1, 0, 1});
    const render::ResourceBarrierDescriptor toHost = render::BarrierBufferDescriptor{
        gpu.Readback.Get(), render::BufferState::CopyDestination, render::BufferState::HostRead};
    command.ResourceBarrier(std::span{&toHost, 1});
    command.End();
    endRecording.Dismiss();
    render::CommandBuffer* submitted[]{&command};
    RADRAY_INFO_LOG("步骤 8：提交一份 CommandBuffer（两个 Pass），等待 GPU 完成");
    gpu.Device->GetQueue().Submit({.CmdBuffers = submitted});
    gpu.Device->GetQueue().Wait();
    if (!gpu.SaveImage(output)) return 6;
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
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--help" || option == "-h") {
            fmt::print(
                "用法：radray_mesh_drawing_sample [--backend d3d12|vulkan] [--shader-dir 目录] [--output 图片.png]\n"
                "先运行 examples/mesh_drawing_pipeline/compile_shaders.py；本程序不在线编译 shader。\n");
            return 0;
        }
        if (option != "--backend" && option != "--shader-dir" && option != "--output") {
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
        } else if (option == "--shader-dir")
            shaderDirectory = argv[i];
        else
            output = argv[i];
    }
    if (output.empty()) {
        output = root / "build/mesh_drawing_pipeline" /
                 (backend == render::RenderBackend::Vulkan ? "vulkan.png" : "d3d12.png");
    }
    return RunSample(backend, shaderDirectory, output);
}

}  // namespace radray::examples::mesh_drawing

int main(int argc, char** argv) {
    return radray::examples::mesh_drawing::SampleMain(argc, argv);
}
