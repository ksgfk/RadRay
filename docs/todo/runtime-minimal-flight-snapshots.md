> - 适用: 验收 runtime 最小线程边界、Forward 值快照、Material group anchor 与 frame-local set 改造
> - 权威: 当前契约见 `docs/architecture/render-framework.md`、`docs/architecture/frame-and-gpu.md` 与 ADR-0053；本文记录实施范围与验收证据
> - 状态: R0–R4 已完成并验证（2026-09-04）
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/src/application.cpp`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/material.cpp`, `modules/runtime/src/forward_pipeline/`, `modules/runtime/tests/`, `examples/example_lambert_sphere/example_lambert_sphere.cpp`

# Runtime 最小 per-flight snapshot 实施与验收

输入为 `RadRay_Runtime_Architecture_Overdesign_Review_v3_Minimal_zh-CN.md`，审阅基线
`7860113e70414ca45cb6dc53b453b4412f8ed51f`。整合后保留该基线的 schema 7、declaration payload
owner、qualified parameter path 和 RadRay DXC `1.9.2607.radray.6`，不恢复位置配对或扁平身份。

## R0–R4

| 阶段 | 最终实现 | 验收位置 |
|---|---|---|
| R0 行为基线 | 保留双后端 Forward、shader/PSO cache、dynamic-offset readback 和 material 资源轮转；补 threaded stress 与非相机宿主 | `test_forward_pipeline.cpp`、`test_mesh_draw.cpp`、`test_render_pipeline.cpp` |
| R1 Pipeline host | PrepareFrame/Render 两入口，Context 仅 Frame/Targets；RenderSystem 接管 target 状态与 fallback clear，删除 Application view 录制钩子 | `render_pipeline.h/.cpp`、`render_system.cpp`、`application.cpp` |
| R2 Snapshot/retention | 按 flight 复制相机、材质、geometry facts 与光源；proxy/material 在 game thread 追加 owners，flight 复用先 clear refs 再 Pump | `forward_frame.h/.cpp`、`material.cpp`、`static_mesh_scene_proxy.cpp`、`render_system.cpp` |
| R3 Binding/sets | 删除 BindingGroupPlan 和 Material flight/set API，按 declaration 解析真实 group；Forward 自持 arena 和 frame-local set cache | `forward_bindings.h/.cpp`、`forward_pipeline.cpp`、`material.h` |
| R4 文档/边界 | MeshDrawList/RenderQueue 的 Forward 归属写入架构文档；追加 ADR-0053，更新术语与指南，清理 compatibility symbols | `docs/architecture/`、`CONTEXT.md`、`docs/adr/0053-runtime-pipelines-consume-per-flight-value-snapshots.md` |

基线 `test_forward_pipeline.cpp` 的真实窗口循环只使用 `Multithreaded=false`，不能证明
game/render 并行时的寿命正确性。新增双后端 stress 每次运行至少 64 个准备帧，修改 transform、
BaseColor、texture、light，并周期性销毁/重建 mesh actor；验证无 error、live ref 或 descriptor
rewrite 诊断。销毁和旧值测试在 PrepareFrame 返回、runner 发布该 flight 之前进行 mutation，
不用额外生产 semaphore。

## 详细验收映射

以下名称对应审阅报告第 6 节；合并进现有 GPU readback 的断言不重复注册另一套后端矩阵。

| 报告要求 | 当前测试/直接证据 |
|---|---|
| 6.1 双后端 Forward baseline | `RadRayRuntimeForwardPipeline.D3D12DrawsCollectedMeshThroughForwardPipeline` 与 Vulkan 对应测试：program/PSO/set、窗口循环、缓存身份与错误日志 |
| 6.1 MultithreadedDrawsWhileGameStateChanges | D3D12/Vulkan 对应测试：至少 64 帧、actor/transform/material/texture 变更及正常 shutdown |
| 6.2 PrepareFrameRunsAfterWorldTick | `RadRayRuntimeRenderPipeline.PrepareFrameRunsAfterWorldTick`：tick 值、game/render thread id 与 flight 私有副本 |
| 6.2 NonCameraPipelineUsesSameHost | D3D12/Vulkan 非相机 pipeline：无 Scene/Camera/Material 输入，target 已为 RenderTarget，设置 ContentDrawn 后提交/present；RenderSystem 仅在 ContentDrawn=false 时 fallback |
| 6.2 GenericPipelineDoesNotDependOnForward | `RuntimeLayering.GenericPipelineDoesNotDependOnForward` 扫描宿主及 render_framework 的 include、类型与阶段名 |
| 6.3 BuildRenderDataCopiesNumericAndResourceState | `RadRayRuntimeMaterial` 同名测试：float/vector/matrix bytes、raw texture/handle/element/subview、完整 sampler/state/queue、retained ref 与独立 storage |
| 6.3 RenderDataDoesNotChangeAfterMaterialMutation | 同名测试：A 保持旧 bytes/texture/sampler/blend/depth/queue，B 反映新值，释放 authoring owner 后 refs 保活 |
| 6.4 RetainedAssetLivesUntilFlightReuse | `RadRayRuntimeRenderSystem` 同名测试：另一个 flight 不回收，复用后在 game thread 恰好一次 unload/destruct |
| 6.4 PreparedFrameSurvivesActorAndMaterialDestruction | D3D12/Vulkan 测试：发布前销毁 mesh/light/camera actor、Material 和应用 refs，旧 flight 仍录制且 geometry/texture 有效，复用后回收 |
| 6.4 PreparedFrameUsesOldCameraAndMaterialValues | 同名测试：旧 flight 的 material/view packed bytes 保持不变，下一快照才使用新 camera/BaseColor |
| 6.5 FrameInputContainsNoGameObjects | 同名静态测试检查 input/material snapshot 的字段与 pointer 类型；render 源码检查禁止读取 game objects/ref API |
| 6.5 CollectsEachSectionWithCopiedFacts | 同名测试：两个 section、两个 material、复制的 transform、geometry/index range/material index 与 mesh/texture owners |
| 6.6 ResolvesProductionDeclarations | `RadRayRuntimeForwardBindings` 同名测试：生产 shader 三个 declaration 的 index/group/dynamic 约束 |
| 6.6 NonCanonicalGroupsAreUsedVerbatim | D3D12 测试使用 4/7/9，Vulkan 使用 2/5/8；真实 pipeline 创建 PSO/sets 并录制，启用 validation |
| 6.6 MissingRequiredDeclarationFailsClosed | 同名测试把 ForwardObject 改为 ObjectData：无 PSO/draw，多帧只记录一次 incompatible program |
| 6.6 NonDynamicRequiredBufferFailsClosed | 同名测试移除 object dynamic recipe：resolver 拒绝、无 PSO/draw |
| 6.7 CreateUsesDeclarationAnchor / UnknownAnchorFails | `RadRayRuntimeMaterial` 同名测试：真实 group、qualified setter、跨组拒绝、unknown/resource anchor 拒绝 |
| 6.8 DifferentBackingTargetsCreateDistinctSets | `test_mesh_draw.cpp::RunMeshDraw` 在已有双后端测试中创建第二 backing set，检查 set 地址不同、原生 set A 的绑定仍为 A，随后用 A 完成 GPU readback |
| 6.8 SameFrameReusesIdenticalPreparedSet | 同一 helper 检查重复准备返回同一 set；`ForwardPipeline::Impl::BeginFrame` 清 Prepared/ProgramSets/MaterialSets 后才 Reset arena |
| 6.8 保留 dynamic-offset GPU readback | `RadRayRuntimeMeshDraw.D3D12DynamicOffsetsAndIndexedDraw` 与 Vulkan 对应测试：同一 set、两个 offset，实际像素分别为红/绿 |
| 6.9 LegacyPipelineScaffoldingRemoved | `RuntimeLayering.LegacyPipelineScaffoldingRemoved` 扫描生产代码与 example |
| 6.9 ForwardSpecificDrawPolicyDoesNotLeakFurther | 同名测试检查 generic host 不引用 queue/opaque/transparent；文档明确 MeshDrawList 只服务 Forward |

补充集成回归 `RadRayRuntimeMaterial.ScopedStoragePreservesQualifiedParameterIdentity` 验证共享
payload root 的两个 declaration 仍有独立参数身份；选择一个 group 后，另一个 group 的 bytes
为空，ambiguous leaf 和跨组写入均不改变已选中的值。

## 最终不变量的代码证据

| 报告第 9 节 | 实现锚点 |
|---|---|
| 1 单一 runtime；2 两阶段边界 | `modules/runtime/CMakeLists.txt` 与 `RenderPipeline` |
| 3 每 flight 独占；4 Scene/proxy 只在 game thread；5 render 不读 game objects/ref | 既有 runner 的 flight semaphore/fence、`Application::Update`、`CollectFrameInput`、Forward Render/PrepareTarget/Execute |
| 6 retained refs 由宿主 game thread 持有；7 flight 回收释放 | `RenderSystem::_retainedAssets`、`BeginUpdateForFlight`、`Material::BuildRenderData`、`PrimitiveSceneProxy::CollectAssetReferences` |
| 8 target 固定状态转换 | `RenderSystem::Render`、EnsureRenderTargetState/ClearTarget/EnsurePresentState |
| 9 group 来自 metadata；10 Material 只知自己 group | Forward resolver 与 `Material::Create`；setter 做 group 检查 |
| 11 Material 无 GPU 时序；12 frame-local sets 不改写 | Material CPU ResourceState、ForwardMaterialSets::GetOrCreate、BeginFrame 清理顺序 |
| 13 program caches 留在 RenderSystem | `_shaderArtifacts`、`_shaderPrograms`、`_shaderJit`；GPU idle 后 shutdown |
| 14 不新增未来系统；15 RenderGraph 不在范围内 | 保留 Scene vectors；无新 runtime target、frame protocol、provider、allocator service 或 multipass material |

## 验证命令与结果

Windows Ninja/MSVC，D3D12 + Vulkan、shader compiler/JIT/tests 全部开启。命令在 x64 VS 开发者
环境执行，build 与 ctest 串行。

```powershell
python tools/fetch_sdks.py restore --only radray_dxc
cmake --preset win-x64-debug
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -j 1 -R "RadRayRuntime(Material|MeshDraw|Forward|RenderPipeline|RenderSystem|ShaderJit)|RuntimeLayering|RadRayShaderContract|RadRayRenderShader(Artifact|Layout)|RadRayDxcMetadata|RadRayShaderCompilerClient|RadRayShaderLibPass|D3D12DeviceFixture|VulkanDeviceFixture" --output-on-failure --timeout 40
python tools/check_docs.py
git diff --check
```

2026-09-04 验证结果：

- Debug 全量构建成功，包含 example、runtime、render 与 compiler tests。
- 上述 120 项相关 CTest 全部通过，无 skipped case；D3D12/Vulkan 实际设备均执行成功。
- `tools/check_docs.py` 与 `git diff --check` 通过。
- 生产源码检索无旧 pipeline/material compatibility symbols；单一 runtime target 与原有 link
  edges 保持不变。`ninja -C build_debug -t commands radrayrender radrayshader` 的编译/归档命令未
  引入 compiler client 或 DXC headers；已有的集中 DLL 部署步骤保留。

工作树已与审阅基线整合。实际 SDK 的 ABI/schema 已核对为 4/7；安装记录曾与解压文件不一致，使用仓库脚本
`restore --only radray_dxc --force` 重新安装了 manifest 固定的 `.6` archive。

## 验证中发现的 DXC package 限制

`1.9.2607.radray.6` 的 DXIL type metadata 在含隐式寄存器尾部 padding 的 vector 后可能发布
`Size != Stride`。本次 fixture 原顺序为 `float4 BaseColor; float Roughness; float2 Tint;
float3 NormalBias; float4x4 MaterialTransform;`，其中 Tint 为 Size=12/Stride=8，NormalBias 为
Size=16/Stride=12；SPIR-V 对应 Size=8、12。runtime 依现有 wire 契约返回 InvalidTypeRecord。

当前 fixture 保留所有数值类型，并把 NormalBias/Roughness 放入同一寄存器、为 Tint 补显式
MaterialPadding，避免把 group/snapshot 测试与这一 compiler padding 问题混合。未修改 SDK
源码或放宽 decoder；该 package 对上述原始合法 HLSL 的支持问题尚未修复，不属于 runtime
线程边界计划。复现可将上述字段顺序放回 fixture，再用 `radray_shader_compile --shader-root
modules/runtime/tests/data --source forward_groups.hlsl --include-path shaderlib --target all --output
<temporary-prefix>` 对照两 lane 的 WireTypeRecord。
