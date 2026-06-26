# 重写参照:已读实的底层 API 缓存(开工前缓存,随用随查)

> 用途:在 `modules/runtime` 内新建 `runtime/render/` 子目录,实现类 Unity SRP 的最小渲染框架,替换被删的 `runtime/renderer/` + material 四件套。
> 本文缓存所有写代码会反复用到的真实 API 签名,均为实读源码。配套文档:`srp_runtime_design.md`(架构)、`gltf_viewer_snapshot.md`(功能对照)。

## 放置位置(已定:选项 A)
- 新代码放 `modules/runtime/include/radray/runtime/render/` + `modules/runtime/src/render/`。
- CMake 用 `file(GLOB_RECURSE)` 自动收集 `include/*.h` + `src/*.cpp`(CMakeLists.txt:1-2),**新增文件无需改 CMake**,删文件直接删即可。
- 命名空间 `radray`。STL 用 `radray` 别名(`radray::string/vector/unordered_map`,来自 types.h)。DEBUG 宏 `RADRAY_IS_DEBUG`。fmt 格式化。

## RHI: radray::render（modules/render/include/radray/render/common.h）
Device 工厂(都返回 `Nullable<unique_ptr<T>>`，common.h:1395+）：
- `CreateShader(const ShaderDescriptor&)` :1421
- `CreateRootSignature(const RootSignatureDescriptor&)` :1423
- `CreateDescriptorSet(RootSignature*, DescriptorSetIndex)` :1425
- `CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor&)` :1427
- `CreateBuffer/CreateTexture/CreateTextureView/CreateSampler` :1415-1439

GraphicsCommandEncoder（:1526，基类 CommandEncoder :1509）录制 API：
- 基类: `BindRootSignature(RootSignature*)` :1517; `BindDescriptorSet(DescriptorSetIndex, DescriptorSet*)` :1519; `PushConstants(BindingParameterId id, const void* data, uint32_t size)` :1521; `BindBindlessArray` :1523
- 图形: `SetViewport(Viewport)` :1532; `SetScissor(Rect)` :1534; `BindVertexBuffer(span<const VertexBufferView>)` :1536; `BindIndexBuffer(IndexBufferView)` :1538; `BindGraphicsPipelineState(GraphicsPipelineState*)` :1540; `Draw(vtxCount,instCount,firstVtx,firstInst)` :1542; `DrawIndexed(idxCount,instCount,firstIdx,vtxOffset,firstInst)` :1544
- ComputeCommandEncoder :1547: `BindComputePipelineState` / `Dispatch(x,y,z)`

关键结构体（common.h）：
- `VertexBufferView{Buffer* Target; u64 Offset; u64 Size;}` :1368
- `IndexBufferView{Buffer* Target; u32 Offset; u32 Stride;}` :1374
- `VertexElement{u64 Offset; string_view Semantic; u32 SemanticIndex; VertexFormat Format; u32 Location;}` :1056
- `VertexBufferLayout{u64 ArrayStride; VertexStepMode StepMode; span<const VertexElement> Elements;}` :1064
- `PrimitiveState{Topology;FaceClockwise;Cull;Poly;StripIndexFormat;UnclippedDepth;Conservative;}` + `::Default()` :1070 (Default = TriangleList/CW/Back/Fill)
- `DepthStencilState{Format; DepthCompare; DepthBias; optional<StencilState> Stencil; bool DepthWriteEnable;}` + `::Default()`(D32_FLOAT/Less/write=true) :1139
- `DepthBiasState{int32 Constant; float SlopScale; float Clamp;}` :1131
- `MultiSampleState{u32 Count; u64 Mask; bool AlphaToCoverageEnable;}` + `::Default()`(Count=1) :1162
- `BlendComponent{Src;Dst;Op;}` :1177; `BlendState{Color;Alpha;}` + `::Default()` :1185
- `ColorTargetState{Format; optional<BlendState> Blend; ColorWrites WriteMask;}` + `::Default(format)` :1202
- `ColorAttachment{TextureView* Target; LoadAction Load; StoreAction Store; ColorClearValue;}` :839
- `DepthStencilAttachment{TextureView* Target; LoadAction DepthLoad; StoreAction DepthStore; Stencil...; ClearValue;}` :846
- `RenderPassDescriptor{span<const ColorAttachment> ColorAttachments; optional<DepthStencilAttachment>; string_view Name;}` :855
- `BindingParameterId{u32 Value;}` 隐式转 u32 :928
- `DescriptorSetIndex{u32 Value;}` 隐式转 u32 :939
- `ResourceBindingAbi{DescriptorSetIndex Set; u32 Binding; ResourceBindType Type; u32 Count; bool IsReadOnly; bool IsBindless;}` :950
- `PushConstantBindingAbi{u32 Offset; u32 Size;}` :961
- `ShaderDescriptor{span<const byte> Source; ShaderBlobCategory Category; ShaderStages Stages; optional<ShaderReflectionDesc> Reflection;}` :921
- `GraphicsPipelineStateDescriptor{RootSignature* RootSig; optional<ShaderEntry> VS; optional<ShaderEntry> PS; ...}` :1222
- `ShaderEntry{Shader* Target; string_view EntryPoint;}` :1217
- `Viewport` / `Rect` 用于 SetViewport/SetScissor（具体字段写到时再查）

## GpuSystem 三级缓存（modules/runtime/include/radray/runtime/gpu_system.h）
- `ShaderCache::GetOrCompile(const ShaderCompileDescriptor&)` → `Nullable<render::Shader*>` :266
  - 也有 `GetOrCompileEntry(...)` → `optional<CompiledShaderEntry>` :267
  - `GetOrCompileFromFile(...)` / `GetOrCompileEntryFromFile(...)` :270/276
- `RSCache::GetOrCreate(span<render::Shader*>)` → `Nullable<render::RootSignature*>` :308
  - `GetOrCreateEntry(...)` → `optional<RootSignatureEntry>` :307
- `PSOCache::GetOrCreate(const GraphicsPsoDesc&)` → `render::GraphicsPipelineState*` :383
- GpuSystem 自身也转发: `GetOrCompileShader` :481 / `GetOrCreateRootSignature` :502 等
- `ShaderCompileDescriptor{string_view Name; string_view Source; string_view EntryPoint; render::ShaderStage Stage; span<const ShaderDefine> Defines;}` :60
- `GpuRenderTargetFormats{vector<TextureFormat> ColorFormats; TextureFormat DepthFormat;}` :92
- `RootSignatureEntry{render::RootSignature* Target; RootSignatureLayoutKey Layout;}` :99
- `PSOCache::GraphicsPsoDesc`（:352）字段：
  - `RootSignature* RootSig; RootSignatureLayoutKey RootLayout; CompiledShaderEntry VS; optional<CompiledShaderEntry> PS; span<const VertexBufferLayout> VertexLayouts; PrimitiveState Primitive; optional<DepthStencilState> DepthStencil; MultiSampleState MultiSample; span<const ColorTargetState> ColorTargets;`
  - PSO key 不含材质身份/实例参数/descriptor set（注释 :381-382）
- `CompiledShaderEntry` / `ShaderCompileKey` / `RootSignatureLayoutKey` 来自 shader_identity.h (KEEP)
- `ShaderDefine` / `ShaderVariantKey` 来自 shader_variant.h（key 容器保留，PixelShaderMode 策略删）

## 几何（KEEP）
- `StaticMesh : Asset`（static_mesh.h:30）持 `MeshResource`（CPU）+ `render::RenderMesh`（GPU）。
  - `GetRenderMesh()` → `render::RenderMesh*` :61；`GetSections()` → `vector<StaticMeshSection>&` :45
  - `StaticMeshSection{PrimitiveIndex; FirstIndex; IndexCount; MinVertexIndex; MaxVertexIndex;}` :14
  - `GetBoundsMin/Max()` :49-50
- `render::RenderMesh`（gpu_resource.h:14）: `vector<unique_ptr<Buffer>> _buffers; vector<DrawData> _drawDatas;`，`DrawData{VertexBufferView Vbv; IndexBufferView Ibv;}`

## 旧 renderer/ 现状（将被删，仅作迁移参照）
- `SceneView{ViewMatrix; ProjMatrix; ViewProjMatrix; EyePosition; ViewportWidth; ViewportHeight;}`（scene_renderer.h:20）——**新框架需保留等价 SceneView 类型**（camera_component.FillSceneView 依赖它）。
- `VisiblePrimitiveList{vector<const PrimitiveSceneProxy*> Primitives;}` :191
- `MeshDrawCommand`（:151）字段：Vbv/Ibv/IndexCount/FirstIndex/VertexOffset; Pso; RootSig; PushConstantId + vector<byte> PushConstantData; array<BoundDescriptorSet,2> DescriptorSets + count; u64 SortKey。**这个结构基本可复用**。
- `BoundDescriptorSet{DescriptorSetIndex Set; render::DescriptorSet* Handle;}` :144
- `MeshPassRenderState`（:205）含 `::PreZ()/Shadow()/OpaqueBase()/Transparent()` 预设——**render state 预设可直接搬到新框架**。
- `PrimitiveFilter = std::function<bool(const PrimitiveSceneProxy&)>` :200
- `RenderContext`（render_context.h:24）god-struct——删，按绑定频率拆给各 pass。
- 旧 `RenderPass`（render_pass.h:17）只有 `GetName()` + `Execute(RenderContext&)`——新框架重写为 SRP 风格（Event/Tags/Filtering/RenderState/RTFormats/ViewSet/WritePerObject/Execute）。
- 旧 `RenderPipeline`（render_pipeline.h:18）: `AddPass` + `Render(RenderContext&)` 按序执行——新框架加 SortStable + RenderPassEvent。

## 接缝（适配，不推翻）
- `World`（world.h:15）持 `unique_ptr<Scene> _scene`（:42），`GetScene()` :36 —— Scene 在 renderer/ 会删，需重新指向新 Scene 类型。
- `CameraComponent::FillSceneView(SceneView&, w, h)`（camera_component.h:39）—— 依赖 SceneView 类型。
- `gltf_asset` —— material 绑定 + ExportToScene 重做，loader 核心保留。

## MUST-REWRITE 清单（删 + 重写）
renderer/ 全目录 + material.{h,cpp} + material_instance + material_parameter_layout + material_render_proxy + components/{primitive_component,static_mesh_component,light_component} + shader_variant 里的 PixelShaderMode 策略部分。

## 三个 proxy 镜像组件（与 proxy 同步重写）
PrimitiveComponent↔PrimitiveSceneProxy / StaticMeshComponent↔StaticMeshSceneProxy / LightComponent↔LightSceneProxy。CameraComponent 不镜像（产 SceneView 值）。

## 落地阶段
1. 词汇+缓存薄封装（RenderPassEvent/TagSet/WantedLightModes/KeywordSet/PerObjectData/SceneView/ShaderVariantCache 包装 GpuSystem 三级缓存）
2. 数据三层（Renderer/Material/Shader 接口 + 最简 Shader）
3. 机器层（DrawingSettings/FilteringSettings/RendererList + CreateRendererList: filter→tag→variant→PSO→sort）
4. 编排层（RenderPipelineExecutor SortStable+Execute + RenderPipeline cull+相机循环 + 通用 DrawObjectsPass）
5. 接缝适配（World.Scene / camera SceneView / 删旧 renderer+material）
6. 重写 gltf_viewer（BasePass + ShadowCaster，用 tag，无 PixelShaderMode/Override）

每阶段必须可编译。构建：`cmake --build build_debug --parallel 24`（先 `cmake --preset win-x64-debug`）。不要 build 和 test 并发。

## 实施进度（执行中）
> 新框架命名空间定为 `radray::srp`（与旧 `radray::` renderer/material 并存,避免 SceneView/RenderPass/FilteringSettings/MeshDrawCommand 等名字冲突;旧代码 Phase 5 删除）。代码落在 `modules/runtime/include/radray/runtime/render/` + `src/render/`。
- Phase 0 ✅ 基线 build_debug 干净。
- Phase 1 ✅ 词汇头:render_pass_event.h / tag_set.h(TagSet 用 std::optional,不用 Nullable<string_view> 会 strlen(null) 崩) / keyword_set.h(= ShaderVariantKey 别名) / per_object_data.h(EnumFlags) / sorting.h(SortingCriteria/RenderQueue/RenderQueueRange) / scene_view.h。测试 test_srp_vocab.cpp 全过。
- Phase 2 ✅ shader.h/.cpp(Shader 具体类,多 LightMode pass,ResolveTag 按优先级;ShaderId{uint64};ShaderPassSource{path,vs,optional ps,TagSet}) + shader_variant_cache.h/.cpp(key=(ShaderId,lightMode,KeywordSet),miss 时经 GpuSystem 编译 VS/PS+取 RS;Shader 加了 move 构造) + material.h(抽象,BlendMode/Queue/GetDescriptorSet) + renderer.h(抽象,BatchElement/VertexLayout/WorldMatrix/语义/GetMaterial)。测试 test_srp_data_layer.cpp 全过。
- Phase 3 ✅ render_state.h(MeshPassRenderState 预设 PreZ/Shadow/OpaqueBase/Opaque/Transparent) + renderer_list.h(FilteringSettings/DrawingSettings/BoundDescriptorSet/MeshDrawCommand/RendererList) + culling_results.h + render_context.h/.cpp(CreateRendererList: filter→ResolveTag→variant→PSO→push-constant 反射→sort;DrawRendererList 录制循环含冗余绑定跳过) + render_pass.h/.cpp(抽象基类 + Execute 默认实现) + filtering_settings.cpp(Test)。测试 test_srp_machine.cpp 全过。CreateRendererList 内部对每 renderer 调 pass.RenderState(mat) 解析渲染状态。
- Phase 4 ✅ 编排层:render_pipeline_executor.h(RenderPipelineExecutor:EnqueuePass/SortStable 稳定插入排序/Execute+清空队列) + draw_objects_pass.h(通用具体 DrawObjectsPass final,Desc 配置+std::function 钩子 RenderStateFn/ViewSetFn/PerObjectFn) + render_pipeline.h(RenderPipeline:RenderSingleCamera/Render(span<CullingResults>),SetupPassesFn 钩子;不做 cull/Submit,留给调用方,Phase5 接入)。测试 test_srp_orchestration.cpp 5 个全过。
- Phase 5/6:用户决定【清理旧基础设施(重实现,不保留旧 helper),gltf_viewer 用新设施重写,参考 gltf_viewer_snapshot.md】。

### Phase 5/6 执行顺序（consumer-first，保证每步可编译）
顺序理由:gltf_viewer + 多个旧测试依赖旧 renderer/material 树;先在旧树仍在时把消费者改到 srp,再最后删旧树。
**特性范围【修正:特性对齐 snapshot,不删特性】**:用户回复“参考 gltf_viewer_snapshot.md”= 以快照的完整特性表为目标。复现:5 图形 pass(ShadowCaster CSM-4 / AdditionalShadow 点聚光 / PreZ / Base 不透明 principled BRDF 多光 / Transparent) + 1 debug compute(preview atlas) + alpha test + tone mapping。关键:这些高级特性都作为 **gltf_viewer 私有代码**建在最小 srp 框架上(shadow/light 数据 = pass 私有 space0,不进 runtime);框架仍保持最小。**gltf_viewer.hlsl 基本不变**(同样 space0/space1/gScene 绑定,只是由新 plumbing 喂数据)。
旧 SceneLightBuffer 逻辑 → 移入 gltf_viewer 私有 LightBuffer helper(产 space0 descriptor set,供 BasePass/TransparentPass 的 ViewSetFn 返回)。ComputeDirectionalCascades / additional shadow 计算 → 移入 gltf_viewer。
ShadowCaster/AdditionalShadow pass = 自定义 srp::RenderPass 子类(覆写 Execute 做多 slice 循环:每 slice BeginRenderPass + CreateRendererList(带 ShadowCaster tag/PassKeywords) + DrawRendererList)。PreZ/Base/Transparent = DrawObjectsPass 配置。
Shader 多 pass:LightMode "UniversalForward"(VSMain+PSMain) / "ShadowCaster"(VSMain+SHADOW_CASTER define + PSDepthOnlyMain) / "DepthOnly"(VSMain+PSDepthOnlyMain)。SHADOW_CASTER 由 pass PassKeywords 注入;ALPHA_TEST 由 material masked 时 MaterialKeywords 注入。
增量策略:先做通 base(material+mesh+cull+space0 light buffer + PreZ/Base/Transparent)跑通,再加 shadow(CSM/additional/preview)。
6a(新增不删): srp::Scene+Light 注册表 / StandardMaterial(具体 Material,反射驱动 space1 set) / StaticMeshRenderer(具体 Renderer over StaticMesh) / Cull helper + space0 builder。
6b: 重写 gltf_viewer.hlsl(简化 space0)/component 改挂 srp::Scene/gltf_asset ExportToScene → StandardMaterial/World→srp::Scene/CameraComponent::FillSceneView→srp::SceneView/gltf_viewer.cpp passes。
5: 删 modules/runtime/.../renderer/ 整棑 + material.{h,cpp}+material_instance+material_render_proxy+material_parameter_layout + scene_light_buffer + 旧 proxy/component;修/删旧测试(test_additional_shadow/test_render_pipeline/test_material_*/test_render_state_presets/test_pso_cache/test_render_graph_valid)。
6c: 全制 build green + gltf_viewer 验证。

### 重写所需精确 API(实读确认)
**StaticMesh几何**: StaticMesh::GetRenderMesh()→render::RenderMesh*(可胺null);RenderMesh 公开字段 `vector<DrawData> _drawDatas`,`DrawData{VertexBufferView Vbv; IndexBufferView Ibv;}`(无 getter,直接访问);GetSections()→vector<StaticMeshSection{PrimitiveIndex,FirstIndex,IndexCount,MinVertexIndex,MaxVertexIndex}>;GetMeshResource().Primitives[i].VertexBuffers→vector<VertexBufferEntry{string Semantic;uint32_t SemanticIndex,BufferIndex;VertexDataType Type;uint16_t ComponentCount;uint32_t Offset,Stride;}>。MeshBatchElement{Vbv,Ibv,IndexCount,FirstIndex,VertexOffset=0}。顶点布局:ToVertexFormat(FLOAT/UINT/SINT,componentCount)→VertexFormat;VertexElement{Offset,Semantic(string_view!指向资产字符串),SemanticIndex,Format,Location(递增)};VertexBufferLayout{Stride,VertexStepMode::Vertex,span<VertexElement>}。Section.IndexCount/FirstIndex 或 IndexBufferEntry.IndexCount。注意:VertexElement.Semantic 是 span/string_view,需保 backing 存僻存活。
**Camera**: ComputeViewMatrix()=LookAt(GetWorldRotation(),GetWorldLocation());ProjMatrix=PerspectiveLH<float>(_fovY,aspect,_nearZ,_farZ);ViewProj=Proj*View(列向量);FillSceneView 写 View/Proj/ViewProj/EyePosition(=GetWorldLocation)/ViewportW/H。
**GpuSystem**: GetOrCompileShaderEntryFromFile(path,entry,ShaderStage,name,span<ShaderDefine>)→optional<CompiledShaderEntry{render::Shader* Target; ShaderCompileKey Key;}>;GetOrCreateRootSignatureEntry(span<render::Shader*>)→optional<RootSignatureEntry{RootSignature* Target; RootSignatureLayoutKey Layout;}>;GetPSOCache().GetOrCreate(GraphicsPsoDesc)→GraphicsPipelineState*;GetFrameUploadScheduler();GetDevice()。GraphicsPsoDesc{RootSig,RootLayout,VS(CompiledShaderEntry),PS(optional),VertexLayouts(span),Primitive,DepthStencil(optional),MultiSample=Default(),ColorTargets(span<ColorTargetState>)}。
**ShaderVariantKey**: .Add(name,value={});.Merge();.Defines()→span<ShaderDefine> 送编译。shader_define::AlphaTest="ALPHA_TEST",ShadowCaster="SHADOW_CASTER"(但新框架用字符串 LightMode tag,不用 PixelShaderMode)。
**RHI 设施**: device->CreateDescriptorSet(rootSig,DescriptorSetIndex)→Nullable<unique_ptr<DescriptorSet>>;CreateBuffer(BufferDescriptor{Size,Memory::Upload,Usage=CBuffer|MapWrite})→...;CreateSampler(SamplerDescriptor)→...。DescriptorSet::WriteResource(name或id, BufferBindingDescriptor{Target,Range,Usage=BufferViewUsage::CBuffer})绑cbuffer;WriteResource(name或id, ResourceView*/TextureView*)绑贴图;WriteSampler(name或id, Sampler*)。RootSignature::FindParameterId(name)→optional<BindingParameterId>;FindPushConstantRange(id)→Nullable<const PushConstantRange*{...Size}>。Buffer::Map(off,size)/Unmap。CBuffer 对齐:device->GetDetail().CBufferAlignment。
**Light参数**(旧 LightSceneProxy 可参考): LightType{Directional,Point,Spot};Direction/Color/Intensity/CastShadow/ShadowDepthBias/ShadowNormalBias/Position/Range/SpotInner/SpotOuter。
**shader_variant_cache.cpp Compile 模式**已验证可用:VS(必)+可选PS→构shader指针数组(1或2)→GetOrCreateRootSignatureEntry→ShaderVariant{VS,PS,RootSig,RootLayout}。
