> - 适用: 实现 material 层与 mesh draw 提交路径，让 runtime 第一次能把 `Scene` 的内容画出来
> - 权威: 本文是已完成 draw path 的历史实施与验收记录；layout producer 的当前裁决以 ADR-0051 为准，本文不重新裁决
> - 状态: 实现完成并通过验收（2026-08）
> - 锚点: `docs/adr/0044-material-owns-full-render-state-baseline.md`, `docs/adr/0045-shader-parameter-packing-driven-by-type-tree.md`, `docs/adr/0046-pso-cache-belongs-to-shader-program.md`, `docs/adr/0047-binding-groups-belong-to-the-concrete-pipeline.md`, `docs/adr/0048-vulkan-y-flip-belongs-to-a-runtime-helper.md`, `docs/adr/0049-dynamic-residency-policy-comes-from-the-pipeline.md`, `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/include/radray/runtime/render_framework/scene.h`, `modules/runtime/include/radray/runtime/render_framework/primitive_scene_proxy.h`, `modules/runtime/include/radray/runtime/static_mesh.h`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/render/include/radray/render/backend_shader_artifact.h`, `modules/render/src/shader_artifact.cpp`, `modules/core/include/radray/vertex_data.h`, `shaderlib/lighting/lights.hlsli`, `examples/example_lambert_sphere/example_lambert_sphere.cpp`

# Material 与 mesh draw 提交路径

> Layout 后续修正：本文完成时使用的 group-wide residency policy 已被 ADR-0051 部分取代；
> per-object/per-view `DynamicCBufferArena`、每 `(layout, flight)` 一个 set、per-draw 只提交 offset 的
> 数据路径继续有效。其 layout producer 将迁移为精确 Target layout modifier、resolved native
> destination 与 `BindingHandle + Offset`，见 `shader-layout-contract-correction.md`。

## 目标

让 runtime 能把 `Scene` 里的 primitive 画出来。当前 `modules/runtime/src` 全域没有
`DrawIndexed`，`BeginRenderPass` 只有一处（框架兜底 clear），`Scene::Primitives()` 与
`Scene::Lights()` 零调用点，`PrimitiveComponent::CreateSceneProxy()` 返回 nullptr ——
**runtime 至今不会画任何东西**。

缺的不是某个功能，而是一个归属：没有对象拥有"用哪个 shader、哪套参数值、哪段几何、什么固定
功能状态"这四件事的组合。四样原料都已就绪：

| 原料 | 现状 |
|---|---|
| artifact / layout / `BindingHandle` | 已交付，ADR-0016 与 ADR-0043 落地，182 个测试兜底 |
| `GpuMesh::DrawData`、`StaticMeshSection`、bounds | 类型齐全，零消费者 |
| type tree（`WireTypeRecord`）| compiler 已输出，唯一消费者是 decoder 测试 |
| dynamic buffer binding | 两个后端全通，零生产者 |
| 帧节奏、flight、upload arena | 稳定 |

`example_lambert_sphere.cpp` 的 765 行里约 500 行是这个空缺的直接后果。

## 范围

**做**：`PrimitiveVertexLayout`、type-tree 参数打包、residency 策略入口、`ShaderProgram` 与
PSO map、`Material`、`StaticMeshComponent` 与 proxy、draw collector 与执行器、
不透明/半透明分离与排序、内置 `ForwardPipeline`、光照上行通道、depth attachment 最小子集、
viewport helper、example 重写、文档同步。

**不做**（各有理由，不得作为完成条件）：

- **视锥剔除**。bounds 已在 `StaticMesh` 里，但剔除会引入 frustum 类型与 visibility list；
  排序与队列先落地，剔除是纯增量。
- **RT pool 与自动 barrier**。只做"depth attachment 由 pipeline 基类提供"这个最小子集
  （约 60 行）。ADR-0012 的显式状态转换原则对 pass 自有资源不变。
- **`MaterialInstance`**。没有编辑器与材质资产格式时，两级参数结构只增加生命周期问题。
- **material 资产格式**（`.mat` JSON 与 importer）。ADR-0016 定 HLSL 是唯一 authoring 权威，
  材质文件格式会立刻成为"谁声明 binding"的第二份真相。第一期 material 由应用代码构造。
- **per-object StructuredBuffer**。理由见 ADR-0049；换 SB 时是局部替换。
- **instancing、GPU-driven、shadow pass、post-processing、skybox**。
- **shader variant 选择策略、AOT cook、artifact index、install/export 层**。

## M1：`PrimitiveVertexLayout` 与 PSO vertex input 匹配

**前置**：无。

`PrimitiveVertexLayout` 在 ADR-0016 与 CONTEXT.md 里已是权威术语 —— "PSO builder 用 compiler
output 与 `PrimitiveVertexLayout` 解析出 native vertex input state" —— 但 `rg` 在代码中零命中。
样例因此手写了 semantic→offset 的 switch，还硬编码"必须恰好 3 个属性"。

**实现项**：

- 新增 `PrimitiveVertexLayout`：`vector<VertexBufferLayout>` + `vector<VertexAttribute>`，
  从 `MeshResource` 的 `VertexBufferEntry`（Semantic / SemanticIndex / Type / ComponentCount /
  Offset / Stride）派生。它是**几何侧的物理字节布局权威**，compiler 不拥有 stride/slot/step/
  format/offset（CONTEXT.md `Vertex input ownership`）。
- `GpuMesh` 或 `StaticMesh` 持有它。当前 `GpuMesh::DrawData` 只有 Vbv/Ibv，
  stride 与 attribute 事实只存在于 `StaticMesh::_meshResource` 里，GPU 侧取不到。
- `MeshPrimitive` 增加 `PrimitiveTopology`，默认 `TriangleList`（ADR-0044）。改动
  `modules/core/include/radray/vertex_data.h`。
- 提供 layout + artifact `VertexInputs()` → `VertexInputState` 的解析入口，复用已有的
  `ValidateVertexInputState` 与 `ValidateVertexInputStateAgainstArtifact`，失配 fail closed。
- `ResourceUploader::UploadMeshResource` 当前只读 `prim.VertexBuffers[0]`，多顶点流网格
  **静默丢数据**。改为显式拒绝（返回 `nullopt` + 报错），不在本期支持多流。

**检查站**：

- [x] **M1-C01**：`PrimitiveVertexLayout` 从 `MeshResource` 派生的结果与 artifact
      `VertexInputs()` 匹配后建出 PSO；semantic 缺失、location 重复、format UNKNOWN、
      未声明 slot、offset/stride 不兼容五类失配在创建任何 native PSO 前失败。
- [x] **M1-C02**：多顶点流 `MeshResource` 上传显式失败并报错，不再静默只取第一流。
- [x] **M1-C03**：`MeshPrimitive` 的 topology 进入 PSO；不含 topology 的旧数据默认
      `TriangleList` 且行为不变。
- [x] **M1-C04**：`rg` 在 examples 与 runtime 中找不到 semantic→offset 的手写映射或
      "属性个数必须等于 N" 的断言。

## M2：type tree 驱动的参数打包

**前置**：无（可与 M1 并行）。

**实现项**（ADR-0045）：

- 从 `ShaderArtifactView::Types()` 与 `Bindings()` 预建扁平参数名索引：
  `名字 → (binding, byteOffset, Kind, Size, Stride, ElementCount)`。撞名时构建失败。
- typed 写入入口（scalar / vector / matrix / texture / sampler）。Kind 与 Size 不匹配即拒绝。
- 输出连续 bytes 交给 `DynamicCBufferArena::Reservation`；纹理与 sampler 走
  `ShaderParameterSet::Set`。
- 打包器**不改动 RHI**：`ShaderParameterValue` 是绑定描述而非数据，cbuffer 内容必须由调用方
  写入 upload slice。

**检查站**：

- [x] **M2-C01**：复用 `nested_types` fixture（8 条 type record，含 struct / vector / scalar /
      array / matrix 与 `TypeIndex` 引用）验证嵌套成员、数组 stride 与矩阵的写入偏移正确。
- [x] **M2-C02**：撞名参数使索引构建失败；未知名字写入失败；Kind 或 Size 不匹配的写入返回
      false 且不修改任何字节。
- [x] **M2-C03**：未写入的参数保持零值。
- [x] **M2-C04**：`rg` 在 runtime 与 examples 的 shader 参数路径中找不到针对 HLSL
      cbuffer/struct 的 mirror struct 或 `offsetof` 断言。
- [x] **M2-C05**：打包器不调用任何 DXIL/SPIR-V reflection API。

## M3：residency 策略入口

**前置**：无（可与 M1/M2 并行）。

**实现项**（ADR-0049）：

- `CreateBackendShaderArtifact` 增加 layout policy 入参，携带"哪些 group 使用 dynamic buffer
  binding"。
- `modules/render/src/shader_artifact.cpp` 现在把 wire binding type code `1` 一律映射为
  `CBuffer`；改为按策略映射 `CBuffer` 或 `DynamicCBuffer`。
- explicit serialized Root Signature 与非空策略并存时 layout 创建失败；策略引用不存在的 group
  时失败。
- 保持 ADR-0043 的其余不变量：device/request/envelope 三方 target 一致才进 typed 入口，
  失败不尝试另一 lane，不新增公共 layout descriptor。

**检查站**：

- [x] **M3-C01**：同一 artifact 在空策略下产出 `CBuffer`、在指定 group 的策略下产出
      `DynamicCBuffer`；两个后端各自映射到 root descriptor 与 `*_BUFFER_DYNAMIC`。
- [x] **M3-C02**：`multiple_root_constants` fixture（带 explicit `[RootSignature]`）配非空策略时
      layout 创建失败，不静默忽略策略。
- [x] **M3-C03**：策略引用不存在的 group 时失败。
- [x] **M3-C04**：dynamic binding 经 `BindShaderParameterSet` 的 dynamicOffsets 生效，双后端
      GPU readback 验证两个不同 offset 读到不同数据。
- [x] **M3-C05**：ADR-0043 的 target 三方一致性负例仍全部 fail closed。

## M4：`ShaderProgram` 与 PSO map

**前置**：M1、M2、M3。

**实现项**（ADR-0046）：

- `ShaderProgram` 拥有 `BackendShaderArtifact`（含 layout）、各 stage `render::Shader`、
  PSO map、M2 的扁平参数索引。一个 program = 一个 concrete Variant。
- `PsoKey` = material 状态基线 + `PrimitiveVertexLayout`/topology + pass attachment 事实
  （format / sample count / `CompatibleRenderPass`）。不含 layout 指针、bytecode 或 program 指针。
- `RenderSystem` 持 program 缓存，key = `(逻辑 SourceName, 规范化 keyword assignments)`；
  持有 `ShaderJit`（JIT 关闭时退化为不可用）。编译失败缓存负结果。无 fallback shader。
- include path 与 shader 源根从 `ApplicationRuntimeDescriptor` 新增字段传入，
  `RenderSystem::OnInitialize` 时取用（与已有 `AssetRoot` / `RenderCachePath` 同类）。

**检查站**：

- [x] **M4-C01**：同一 program 下改变 material 状态、vertex layout 或 attachment 格式各自命中
      不同 PSO；重复请求命中缓存。
- [x] **M4-C02**：program 析构后其全部 PSO 一并析构；不存在 detach 或 refcount 路径；
      `rg PipelineStateCache` 与 `rg SharedPipelineLayout` 无命中。
- [x] **M4-C03**：编译失败被缓存为负结果，同一 key 不在每帧重新触发编译；日志不刷屏。
- [x] **M4-C04**：`RADRAY_ENABLE_SHADER_JIT=OFF` 时 `RenderSystem` 仍可构造，program 请求
      明确失败，不崩溃。

## M5：`Material`

**前置**：M4。

**实现项**（ADR-0044）：

- 删除 `MaterialRenderState`（`render_pipeline.h:56`，当前零使用点）。
- `Material` 持有：`ShaderProgram*`、参数值（经 M2 打包）、纹理与 sampler 引用、
  完整固定功能状态基线（`PrimitiveState` 去 topology、`DepthStencilState` 去 `Format`、
  `optional<BlendState>` + `ColorWrites`）、`RenderQueue`。
- material 的纹理/sampler 写在常驻 set，仅纹理变更时按 flight 数轮转重建；数值参数每帧
  重新打包进 arena 并以 dynamic 绑定。
- material 不含 `TextureFormat`、`SampleCount`、`RenderPass*` 或 topology。

**检查站**：

- [x] **M5-C01**：`rg MaterialRenderState` 无命中。
- [x] **M5-C02**：同一个 material 实例在两个 attachment 格式不同的 pass 中使用成功，
      且不需要复制材质。
- [x] **M5-C03**：纹理变更后按 flight 轮转的 set 生效，且 GPU 仍在读旧帧时不发生改写。
- [x] **M5-C04**：material 头文件不 include 任何提供 `TextureFormat` 之外 attachment 事实的类型。

## M6：primitive 侧代理与 draw 提交

**前置**：M5。

**实现项**：

- `StaticMeshComponent`：持 `StreamingAssetRef<StaticMesh>` 与 material 列表（按 section）。
- `StaticMeshSceneProxy`：覆写 `GetLocalToWorld` 与 `GetDrawArgs`，**自持一份
  `StreamingAssetRef<StaticMesh>`**（`MeshDrawArgs::Geometry` 是指向资产内部的裸指针，
  保命责任在 proxy，见 `docs/architecture/asset-system.md`）。
- draw collector：遍历 `Scene::Primitives()`，产出 draw item（几何 + material + local-to-world +
  section 索引）。**collector 主动遍历**，proxy 只暴露事实。
- 排序：material 的 `RenderQueue` < 2500 走不透明（按 program/PSO 分组以减少切换），
  >= 2500 走透明（按视深 back-to-front）。draw list 每相机每帧重建（vector 复用容量），
  不做持久 draw command 缓存。
- 执行器：按 ADR-0049 的 dynamic 路径录制 —— per-view 与 per-object 各一个常驻 set，
  per-draw 只给 dynamic offset。成功录制后**由框架回写 `RenderPipelineTarget::ContentDrawn`**，
  不再要求 pass 作者手工遍历 `ctx.Targets`。

**检查站**：

- [x] **M6-C01**：`Scene::Primitives()` 与 `Scene::Lights()` 各自有生产消费者；
      `PrimitiveComponent::CreateSceneProxy()` 的派生实现返回非空。
- [x] **M6-C02**：多 section 网格按 section 绑定不同 material 并各自成为一个 draw item；
      `StaticMeshSection` 的 `FirstIndex` / `IndexCount` / `VertexOffset` 全部进入 draw。
- [x] **M6-C03**：不透明按 program 聚簇、透明按视深降序；同 queue 内顺序稳定。
- [x] **M6-C04**：per-draw 路径不创建 `ShaderParameterSet`、不写 descriptor，只下发
      dynamic offset（用 D3D12/Vulkan 的调用计数或 RenderDoc 之外的可断言手段验证）。
- [x] **M6-C05**：proxy 销毁后不存在悬垂的 `MeshDrawArgs::Geometry`；组件
      `MarkRenderStateDirty` 后 proxy 重建且资产引用不泄漏。
- [x] **M6-C06**：`ContentDrawn` 由框架回写；pass 代码中无手工遍历 `ctx.Targets` 的回写。

## M7：内置 `ForwardPipeline`

**前置**：M6。

**实现项**（ADR-0047、ADR-0048）：

- `modules/runtime/{include/radray/runtime,src}/forward_pipeline/`：`ForwardPipeline` +
  它的 opaque/transparent pass + `BindingGroupPlan{0, 1, 2}`。
- **depth attachment 最小子集**：由 pipeline 基类或 `ForwardPipeline` 提供 depth texture/view
  的建立、按 size 复用、resize 重建与 framebuffer 缓存失效。不引入 RT pool，不做自动 barrier。
- **viewport helper**：`MakeViewport(backend, width, height)`，Y 翻转的唯一实现。
- **光照上行通道**：`Scene::Lights()` → `LightSceneProxy::GetLightRenderParameters` →
  投影到 forward 的 view CB。投影在 `ForwardPipeline` 内做（它知道自己的 CB 布局），
  不改动 `LightRenderParameters`（它是引擎侧完整描述）。超过上限时按距相机距离取前 N 并
  一次性 warn，不每帧刷。
- **HLSL**：新增 `shaderlib/pipelines/forward/`，含 view/lights 的 cbuffer 布局与产品 pass；
  `shaderlib/lighting/lights.hlsli` 保留在共享层不动；**删除 `shaderlib/passes/forward.hlsl`**，
  `test_shaderlib_passes.cpp:47` 与 `test_runtime_shader_jit.cpp:465` 的 forward 用例改指向新
  产品 pass 并更新期望值；`passes/depth.hlsl` 与 `passes/compute.hlsl` 保留。
- per-view CB 第一期只放 ViewProj、EyePosition 与光照数组；不加 time / jitter / 逆矩阵。

**检查站**：

- [x] **M7-C01**：`shaderlib/passes/forward.hlsl` 不存在；`RadRayShaderLibPass` 与
      `RadRayRuntimeShaderJit` 的 forward 用例指向 `shaderlib/pipelines/forward/` 的产品 pass
      并通过。
- [x] **M7-C02**：`lights.hlsli` 不含 cbuffer 布局、group 号或 binding 号；
      forward 的 view/lights 布局只在 `shaderlib/pipelines/forward/` 声明一次。
- [x] **M7-C03**：执行器与 material 代码中无 group 数字字面量；plan 引用的 group 在 layout 中
      不存在时 pipeline 初始化失败。
- [x] **M7-C04**：runtime 与 examples 中只有一处 `backend == Vulkan` 的 viewport 分支
      （helper 内部）；两个后端画出的图像朝向一致。
- [x] **M7-C05**：光源数超上限时按距离截断并只 warn 一次；`LightRenderParameters` 未因 shader
      ABI 而改动。
- [x] **M7-C06**：resize 后 depth attachment 与 framebuffer 缓存正确重建，无陈旧
      framebuffer 命中。
- [x] **M7-C07**：`RenderSystem::_pipeline` 仍初始为 null，`ForwardPipeline` 需显式
      `SetPipeline` 注入。

## M8：example 重写与文档收尾

**前置**：M7。

**实现项**：

- 重写 `example_lambert_sphere`：用 `AssetManager` 加载 mesh 与贴图（当前样例绕过资产系统直接
  `TriangleMesh::InitAsUVSphere` + `UploadMeshResource`，所以 `StaticMesh` 资产路径在样例里从未
  被验证），构造 material，`SpawnActor` + `StaticMeshComponent`，注入内置 `ForwardPipeline`。
  预期从 765 行降到 150–200 行（depth 管理留在 pipeline 基类，剔除不做）。
- ADR-0017 的状态改为"部分被 ADR-0043、ADR-0047、ADR-0048 取代"。
- 更新 `docs/architecture/render-framework.md`：runtime 开始提供内置 forward pipeline；
  `MaterialRenderState` / `RenderQueue` / `MeshDrawArgs` / 具体 primitive proxy 的"零消费者"
  记录全部作废；补 material、draw collector、执行器与 `BindingGroupPlan` 的形状。
- 更新 `docs/guide/shader-authoring.md`：pipeline 特有的 group 约定、`shaderlib/pipelines/`
  的身份、现有最小 pass 表格。
- 更新 `docs/architecture/shaderlib.md`：新增 `pipelines/forward/`，删除 `passes/forward.hlsl`
  条目与锚点。
- 更新 `CONTEXT.md`：新增 `Material`、`MeshDrawItem`、`PrimitiveVertexLayout`、
  `Binding group plan`、`Residency policy` 词条；`Residency` 词条补上 dynamic 选项。
- 更新 `docs/adr/README.md` 表格。

**检查站**：

- [x] **M8-C01**：重写后的 example 在 D3D12 与 Vulkan 下各自画出带纹理的 Lambert 球，
      `--multithread` 与 `--valid-layer` 均通过；Vulkan validation 无错误。
- [x] **M8-C02**：example 中不出现 `CreateGraphicsPipelineState`、`CreateShaderParameterSet`、
      `BeginRenderPass`、`DrawIndexed` 或 `ShaderJit`；mesh 与贴图都经 `AssetManager` 加载。
- [x] **M8-C03**：新增两个 suite 全绿 —— `RadRayRuntimeMaterial`（type-tree 打包、vertex layout
      匹配 fail-closed、draw list 排序、residency 策略负例）与 `RadRayRuntimeMeshDraw`
      （双后端 draw + readback，复用现有 GPU fixture，进 CTest）。
- [x] **M8-C04**：`python tools/check_docs.py` 通过；`git diff --check` 通过。
- [x] **M8-C05**：`ctest -C Debug` 全量通过，含既有 shader/JIT/Pso suites 无回归。

## 后续但不阻塞本计划

- **视锥剔除与 visibility list**：`StaticMesh` 的 bounds 已就绪，proxy 只需算世界 AABB。
- **RT pool 与自动 barrier**：depth 之外的 attachment、MSAA resolve、多 pass 共享目标。
- **instancing 与 per-object StructuredBuffer**：届时按 ADR-0049 做局部替换，
  M2 的打包器不动。
- **`MaterialInstance`、material 资产格式与 shader 资产层**：需要先裁决 variant assignment 的
  C++ 表达形式（ADR-0015 已被 ADR-0016 取代，新形式未定）。
- **shadow pass**：`shaderlib/shadow/{cascade,cube,filtering}.hlsli` 已写好且零消费者；
  `DirectionalLightSceneProxy` 已带 CSM 级联配置。
- **热重载**：JIT 已在，缺的是失效与重建策略。
- **多顶点流网格**：M1 显式拒绝，需要时再支持。
