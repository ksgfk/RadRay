> - 适用: 2026-10-13–14 GPU 生命周期、Vulkan 1.2 与 per-flight Scene 计划实施验收
> - 权威: 本次执行状态与测量快照，不作为长期契约；当前行为以 architecture/guide 为准
> - 锚点: `modules/runtime/src/gpu_system.cpp`, `modules/runtime/src/frame_timeline.cpp`, `modules/runtime/src/gpu_frame_resources.cpp`, `modules/render/src/vk/vulkan_render_pass_cache.cpp`, `tools/run_gpu_lifecycle_benchmark.py`

# GPU 生命周期计划实施与验收

状态：P0–P9 全部实现并完成本机验收。输入是 [原始完整计划](gpu-lifecycle-plan-vulkan12-per-flight.md) 及同轮补充说明 a.md。实施基线为 `7a807f037c5e15045bf2fc6c702d8819842b9ed5`，最终代码与文档仍在工作树，未提交。已知 descriptor 容量边界及未具备的 Vulkan 1.2-only 硬件分别如实列出，不计作已执行场景。

当前合同见 [帧与 GPU](../architecture/frame-and-gpu.md)、[RHI 与后端](../architecture/render-rhi.md)、[渲染框架](../architecture/render-framework.md)、[资产系统](../architecture/asset-system.md)，运行入口见 [构建与测试](../guide/build-test.md#gpu-生命周期回归)。

## 阶段与落点

| 阶段 | 实施落点与完成条件 |
|---|---|
| P0 | 固定 7a807 基线，保留原始 Release 二进制；整理下表的资源 owner 和调用入口，核查 Vulkan 1.2 feature/API 边界；性能结果使用同机配对测量。 |
| P1 | GT 提前保留 FrameSerial；命令按 Idle/Recording/Closed/Accepted/Submitted/Discarded 转移；真实 final proof 覆盖的提交前缀先结束 native command epoch，再清 frame resources、读取结果、发布完成，最后 GT 释放 owner。 |
| P2 | Scene 每个 stream 独立暂存并登记 RequiredWork，再清 Pending；可选目标单独编码。隐藏、不可呈现、PSO/后续流失败都不撤销已接纳更新。删除 Attempt 与完成执行反馈；OnOutput 单独报告输出状态。 |
| P3 | GpuFrameResources 组合 HostWrites、staging、uploader、constant/storage arenas、参数池。staging 只管理一个 epoch；封口拒绝未 Commit reservation 和后续写入；SPS 首次绑定即冻结，真实完成后先清旧 metadata/typed views 再复用存储。 |
| P4 | 公共附件值描述及 Prepare token 替代 RP/FB owner。Vulkan 私有 typed RP/FB 表区分使用 pin 与预算内 idle，command 去重持有，view 稳定 ID/局部失效，预算失败在 Prepare 暴露；D3D12 直接映射。 |
| P5 | RenderScene 持有共享 primitive/section/material 模板；Apply 根据最终改绑值局部更新，transform 不重建模板。MaterialPass 为共享不可变定义，惰性 native plan 归定义，默认参数仍按 Material 独立；删除 SceneDraw/RasterDraw 长期旁路缓存。 |
| P6 | 移除 AssetManager GPU DeferDestroy、scope 与 pending batches。TextureAsset 卸载先 views 后 texture；Scene typed refs、GT frame owner 和 RT GPU owner 接入真实使用期；shutdown 先释放材质 SPS 再拆 program/layout 服务。 |
| P7 | typed 帧目标池提供带 epoch/最终状态的 lease，有限数量/字节预算；新逻辑借用不继承内容有效性。history、异步 GPU 写入和延迟 CPU readback 由过程 owner 明确保留，真实 fence 与结果消费分别证明。 |
| P8 | FrameTimeline 的单一 CompletedSerial 账本负责完成与等待；预分配 F 项通知环与一槽一条未消费通知，等待绑定 TargetSerial；重复、旧代、外部身份和取消不能释放新域。 |
| P9 | 全量 Debug/Release 回归、关闭 JIT/profiler 构建、两后端 F1–F4/失败/取消/关停/预算回归，配对性能与完整压力矩阵；同步长期文档并保存输入与本记录。 |

## 资源接入核对

| 资源/调用方 | 创建与逻辑 owner | 实际使用/退休证明 |
|---|---|---|
| Scene 矩阵/实例参数 | SceneGpuData 的 F 份镜像，容量与 generation 分别维护 | 当前 writable flight 扩容，必要更新接纳后清 Pending；typed Scene owner 至匹配真实完成。 |
| Material 默认常量、静态 SPS、纹理 | MaterialGpuState / Material 资产及其 typed 默认 refs | SceneWriter 资产使用计数与退休包；临时 override/manual SRV 使用者显式保留。 |
| Mesh GPU bins | StaticMeshRenderData / GpuMesh，Immutable section/binding 模板 | Scene 绑定 owner、删除包；独立 native producer 使用 GT/RT frame 包。 |
| staging、常量/storage 切片、动态 SPS | GpuFlightSlot::FrameResources，一个 epoch | 最后应用 batch、profiler 与目标收尾的 final fence；封口及旧借用清理后复用。 |
| 帧内 render target | GpuFrameResources typed TextureLease pool | 只在开放 epoch 借用，不接 external target；owner 保留纹理和 view，native epoch 结束后复用。 |
| 历史/异步写入/CPU readback | 显式过程 owner，按资源版本持有 | GPU 使用 proof 与 CPU 消费权独立；取消通知不取消已接纳 GPU 使用。 |
| Window / external swapchain | WindowManager / 外部 provider | 借用 target 描述、acquire token 和平台 present 同步；target 生命周期覆盖所有命令引用。 |
| Vulkan RP/FB | DeviceVulkan 两张 typed native cache | prepare/录制时取得 command pin，Reset 结束 epoch；无永久 attachment/资产 owner，view 销毁前仅局部清 idle FB。 |
| Vulkan PSO 代表 RP | cache 临时创建引用 | CreateGraphicsPipeline 返回即可释放；PSO 存兼容签名，真实 Begin 的 RP 由 command 单独持有。 |
| profiler query/readback | GpuFrameProfiler 与 flight | native epoch reset 后、帧内存复用前 resolve；没有用取消续体作唯一 owner。 |
| standalone/共享 Device 宿主 | 外部 device/queue owner，各自 GpuSystem/FrameTimeline | 独立 serial/final proof 与 completion 消费；A 的 Reset 不释放 B 的 pin。 |

## Vulkan 能力边界

Instance、VMA 与默认 SPIR-V target 使用 Vulkan 1.2；loader/physical device 低于 1.2 明确拒绝。timelineSemaphore 是必需且显式检查的 1.2 feature。传统单 subpass RP/FB 为唯一主路径，dynamic rendering、imagelessFramebuffer、separateDepthStencilLayouts、synchronization2 均未启用。depth-only 仍使用 coupled depth/stencil core layout。

测试机器为 RTX 4080，原生复现报告 physical API 1.4.351，测试中请求 1.2 instance 并禁用上述 optional features；没有物理 Vulkan 1.2-only 设备。未以较新功能可用性替代最低功能边界。已有 device limit/property 查询可读取较新 driver 的上限，不用于启用较新执行 API。

## 构建与正确性证据

| 配置 | 结果 | 原始日志 |
|---|---|---|
| Debug：D3D12/Vulkan、JIT、profiler、examples、benchmarks | 710 项、0 失败，445.52 s，含共享设备、跨线程消息等待、transfer-first 回读及恢复的 Vulkan 多窗口用例 | `build_release/gpu-lifecycle-v7-debug-ctest.log` |
| Release：同上 | 708 项、0 失败，424.31 s，与 Debug 相同的发布路径与窗口回归 | `build_release/gpu-lifecycle-v7-release-ctest.log` |
| Release runtime-only：shader compiler/JIT/tools/profiler OFF | 605 项、0 失败，189.25 s，含共享设备、消息等待、transfer-first 与恢复的 Vulkan 多窗口用例 | `build_release/gpu-lifecycle-v7-runtime-only-ctest.log` |

两个完整功能配置都只有 1 项预期 skip：Vulkan cache 参数化测试的 D3D12 分支。Debug 多出的 2 项是仅 Debug 开启的诊断测试。runtime-only 有 21 项预期 skip，其中 20 项 SceneDraw 用例明确需要 JIT，另 1 项是上述 cache 的 D3D12 参数；其余已发现的两后端原生测试正常执行。原生 host signal gate 通过后端 API 直接释放，相关 GpuSystem final-fence/共享 host 场景关闭 validation，使用真实 fence、cache pin 和独立 owner 断言；附件、Scene draw 及 WSI 的 debug/sync validation suites 另行运行。基线注释的 5 项 Vulkan 多窗口同步验证用例已在上游修复层的原生默认复现（Debug/Release 各 5 次、errors=0）后恢复。依据上游 [问题 13117](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/13117) 与 [修复 13248](https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/13248)，独立构建 VVL 合并提交 da7669a、API 1.4.364，仅以进程 VK_LAYER_PATH 选择；旧 SDK 1.4.357.0 的这些用例明确 version skip。恢复的用例均实际执行。窗口压力复验揭示 acquire 只等待颜色/计算阶段的缺口，原 7a807 基线也复现 FIFO fence 超时；按通用 RHI 的首次访问契约改为 ALL_COMMANDS，并增加先复制前次呈现像素、再清除/呈现的完整图像 oracle。修正后两个生命周期及 transfer-first 用例各重复 25 次（75 个独立进程），0 失败；v5 WSI 专项的 Debug/Release 各 15 个用例重复 5 次，共 150 次全部通过，耗时 83.02 / 80.87 s。没有空批次 workaround 或 validation error filter。窗口可见用例指定屏内 Y，避免单独指定 X 时 Win32 将窗口放在 Y=-32768。另以有界等待处理跨线程 SendMessage，四个真实 host signal gate 用例各重复 20 次通过，posted 输入仍在真实完成后分发。runtime-only 最终复验完成。runtime-only 链接项目不引用 shadercompiler/DXC/TracyClient，未用输出目录遗留 DLL 作为依赖证明。native correctness fixture 要求两后端可用且 validation 有效，不能将初始化失败降级成通过。

新增 MRT/只读深度、4×MSAA reload/resolve 实际回读，以及多 FB 局部 view 失效和销毁 PSO 创建期代表 RP 的用例。只读深度回读首次揭示 StoreOp 的同步缺口，修正 Vulkan DepthRead access mask 后，Debug 附件专项 15 项、0 失败，14.19 s；不保留失败结果作为验收。

### 原计划正确性编号映射

编号是原计划的验收场景，不是新增 48 个独立 executable；复用已有 Google Test fixture 与参数化矩阵。

| 原计划 | 实际 suite / 证明 |
|---|---|
| C01–C05 | RenderScopeKeys、PipelineCacheKey：执行键、兼容签名、FB view/extent/layers、clear/load/store、无 stencil 操作归一化；RenderScopeTest：RenderArea 与实际内容。 |
| C06–C12 | RenderScopeTest.VulkanNativeReusePinsBudgetAndLocalViewInvalidation：新 view ID、command 去重 pin、idle 复用/预算、同 view 多 FB 的局部失效、第二步 vkCreateFramebuffer 失败注入；PreparedEpochClearReadbackAndForeignToken：旧 epoch/foreign token；RadRayRenderPsoSmoke.Vulkan：idle RP 预算为 0，PSO 代表对象退出后实际绘制。 |
| C13–C15 | PipelineMaterial.SceneTemplatesShareGeometryAndOnlyChangeWithFinalBindings、PublishedMaterialsShareOnePassDefinitionAndKeepIndependentDefaults；SceneDraw 自动实例测试在多 view/大量 transform 变化中验证模板稳定；RasterDraw 静态组 probe 验证共享计划与独立默认值。 |
| C16–C18 | FrameTimeline、FrameTimelineDeathTest：真实 serial、重复/旧代/未消费通知、取消与 terminal；WorldLifecycle.SchedulerFreezesItsBatchAndRevalidatesCanceledWaiters；SceneDelivery.AbandonDoesNotPublishOrRequireCompletion、CPU-only runner。 |
| G01 | Backends/SceneGpu.ReadbackFlightIsolationAndGenerationReuse、GpuFrameTargets：F1/2/3/4；SceneDeliveryRunner、SceneDraw、RasterDraw：命令与参数版本/owner。 |
| G02–G03 | GpuSystemTest.*OnlyFinalFenceRetiresFlight：第一批真实附件已执行，后续 gate 未释放时 native RP/FB pin 与 RT owner 仍在；RenderScopeTest：prepare 后、End 后、未 Submit 时销毁 view 被诊断；GpuSceneLifetime：逻辑 actor/asset 退出不结束接受的 draw。 |
| G04–G06 | RasterDraw.HiddenExternalSurfacePreservesIndependentApplicationWork、RuntimeFoundation.D3D12DroppedPresentationRecovers、SceneGpuParameters：必要更新与输出独立；SceneGpuAllocation、FrameUploadTest：多范围原子暂存、失败重试；SceneDraw.*LateDropRetriesLatest：后续 prepare 失败仍执行已接纳 Scene 更新。 |
| G07–G09 | GpuSceneLifetime.*DirectDrawOwnerSurvivesCanceledNotification；GpuRetainedResult.*CpuCancellationKeepsPendingGpuWriteAlive；RenderScopeTest 两 allocator 持有同条目及同 command 多 Prepare：全部 epoch 结束后才进入 idle。 |
| G10–G11 | RenderScopeTest.MultipleColorsAndReadOnlyDepthPreserveStoredPixels、ReadOnlyDepthStencilPreservesBothNativePlanes、PartialAreaClearPreservesAttachmentContentsOutsideTheArea、MultisampleResolvePreservesClearThroughCompatibleReload；DeviceCapabilitiesTest 的多格式/采样；SceneDraw depth-only；RasterDraw depth 与颜色 oracle。 |
| G12–G13 | 两后端 native suites 在 instance 1.2、无 dynamic/imageless/separate DS/synchronization2 的唯一传统路径执行；实际 driver 支持 1.3+，不是 1.2-only 硬件。 |
| G14–G16 | RenderScopeTest 的新 view ID/局部清理、多 load/store 共用 FB；RuntimeVulkanSwapChain.RepeatedPresentAndImmediateRecreation；RadRayRenderPsoSmoke.Vulkan 的 0 idle RP 预算及实际像素回读。 |
| G17–G19 | SceneGpuAllocation、SceneGpuParameters、SceneGpu 的 F1–F4 readback；SceneDelivery/WorldLifecycle generation 复用；同 serial 多次 Prepare 固定结果，包括失败与空结果。 |
| G20–G21 | GpuSystemAttachment.*HostsShareDeviceAndRetireIndependently：同 Device、queue、RP/FB 下两个独立 host，各有 final fence；B gate 未释放时 A 完成只退 A pin。RenderScopeTest 的 Clear 走 Device 关闭缓存的相同检查路径，仍有 pin 时 death test 明确诊断。 |
| G22 | RasterDraw.PersistentPureDefaultsSurviveViewsFlightsAndTemporaryOverride、PersistentConstantsInMixedGroupsSurviveViewsFlightsAndTemporaryOverride、ProbeShowsNoStaticGroupWritesAfterWarmupAndSafeServiceReplacement；两后端实际结果与 fixture probe。 |
| G23–G24 | GpuRetainedResult：history 版本替换、旧 reader gate、延迟 CPU readback 与 CPU 取消；FrameResources typed pool：有限预算与内容有效性独立。 |
| G25–G27 | RuntimeFoundation、Application、SceneDeliveryRunner.*ExitDrainsPublishedFlights、WorldLifecycle.PartialBootstrapShutsDownWithoutAPublishedFlight、RuntimeVulkanSwapChain、GpuSceneLifetime、PipelineMaterial.SharedMaterialRetainsTextureUntilBothScenesRetire；真实使用者 owner 至 final proof。 |
| G28–G30 | RenderScopeTest 预算仅退 idle、全部 pinned 时 Prepare 失败；SceneGpuLifetime 的覆盖 fence 删除包与多 flight prefix；FrameResources、RasterDraw probe 在完成后清旧 SPS metadata/typed views，再完整写入新借用。 |

## 性能证据

完整压力覆盖已执行 3,548 个注册用例。3,484 个正常运行，64 个 D3D12 `N100000/M100000` 用例在初始化时明确报告需要超过 100k descriptors，而现有 renderer heap 容量为 65,536；这是已知容量边界，未把这些未执行的 GPU workload 算作通过。没有其他运行错误。原始数据在 `build_release/gpu-lifecycle-v5-coverage/coverage-final/`，核对结果为 `coverage-audit.json`。该轮请求 1x，SceneSync/Lifecycle Frame 的固定 256 次迭代注册仍生效；这一轮只证明可运行性，不用于比较性能收益。

| 入口 | 注册用例 | 正常执行 | 已知容量边界 |
|---|---:|---:|---:|
| bench_material_scene | 2,310 | 2,246 | 64 |
| bench_scene_gpu | 368 | 368 | 0 |
| bench_command_allocator | 118 | 118 | 0 |
| bench_raster_prepare | 6 | 6 | 0 |
| bench_scene_sync | 330 | 330 | 0 |
| bench_lifecycle | 416 | 416 | 0 |

Vulkan `FullFrame/N100000/M1/V3/dirty0` 的 Scene buffer 分配容量在 F1/F2/F3/F4 分别为 8/16/24/32 MiB；这是保留多副本的实际测量，不是单副本显存优化。

单次更新后静止的专项在两个后端、F1–F4、1/3 view 和 CpuSync/PrepareAndRecord/SubmitFrame 上执行 48 个用例，每个用例三次 benchmark repetition、每次 1,000 帧。每次 repetition 的上传总量恰为 `64 × F` bytes、`F` 个 range，没有静止后的重传。结果见 `build_release/gpu-lifecycle-v5-once/once-audit.json`。目标专项执行 78 个用例、234 条 repetition；其中 144 条真实窗口数据在稳定、resize 与 output_skip 下均每帧执行 64 bytes 必要 copy，正常输出数为 W，output_skip 的平均跳过数为 W/2。回读与错误核对见 `build_release/gpu-lifecycle-v5-targets/targets-audit.json`。

最终性能对照已完成，初步正向区间逐项用独立长采样复验后，没有超出测量噪声的可重复 workload 退化。已根据 Tracy 将空 Scene 模板改为惰性分配，并在 Scene GPU packing 中直接借用矩阵；D3D12 附件状态仅在首次 Prepare 时建立。无目标/外部同步的 flight 将 profiler head、应用命令和 tail 按原顺序合并提交，保留目标和外部同步边界。单 Actor 删除在身份解析后只扫描 owner 指针、稳定移除；避免读取 100k 个 Actor 的生命周期，并以首位/中间/末位删除测试核对回调前顺序、组件注销和旧身份失效。五组独立 50x 配对的 DeleteAndSync 从 575.908 μs 降到 78.836 μs，比值 95% 区间为 0.134–0.140；四个 Reset/Record 专项五组配对均未显示可靠退化。专项原始数据在 `build_release/gpu-lifecycle-v5-lifecycle/` 和 `build_release/gpu-lifecycle-v5-allocator/`。

v5 主矩阵有 273 个配对 workload、150 项 GPU 比较，补充 F2/F4 的 48 个稳态、16 个首次使用/扩容以及 72 个密集更新 workload 后，共 409 个独立 workload、286 项 GPU 比较。补充矩阵均无正向 95% 区间，全部矩阵无 GPU 退化信号，原始目录为 `gpu-lifecycle-v5-broad/`、`gpu-lifecycle-v5-flights/`、`gpu-lifecycle-v5-flights-cold/`、`gpu-lifecycle-v5-dense/`。主矩阵的四条初步正向指标对应三个 workload：D3D12 M32/K1 Reset、Vulkan M1/K1 Record/总时间、Lifecycle Frame 10k/F1；D3D12 和 Lifecycle 的五组 1s 配对均跨过比值 1。

Vulkan 的约 10 ns Record 信号通过保持 profiler 热字段位置消除；Reset 只在 allocator 确实有 Prepare 结果时遍历附件 metadata，copy-only allocator 跳过这部分工作。v7 六个专项五组配对没有正向区间，但其中 M1 的一组 baseline/final 和另一组 final 都出现约双倍耗时，原因未确认，保留原始数据且不以宽区间作为通过证据。独立 M1 五组 1s 复测中，Record 为 0.370→0.366 μs，总时间为 0.511→0.507 μs，Reset 为 0.142→0.141 μs；对应比值区间为 0.982–0.995、0.976–0.997、0.956–1.007。完整 20 个 Vulkan M/K 组合三组配对仅 M32/K1 Reset 有初步正向区间；该项隔离五组复测的 Reset 为 4.634→4.566 μs、区间 0.947–1.019，总时间为 18.017→17.764 μs、区间 0.962–1.014，没有重复退化。结果见 `gpu-lifecycle-v7-allocator-focus/`、`gpu-lifecycle-v7-allocator-matrix/`、`gpu-lifecycle-v7-m1-isolated/`、`gpu-lifecycle-v7-m32-isolated/`。v5 与 v6 二进制均归档，v7 全量正确性和固定 F2 trace 均已完成。

| v5 F2、10k 对象、3 view、dirty1 的 CPU PrepareRecord | Baseline μs | Final μs | 配对比值 95% 区间 |
|---|---:|---:|---:|
| D3D12 / 1 材质 | 1130.997 | 821.134 | 0.666–0.806 |
| D3D12 / 100 材质 | 1533.079 | 1179.757 | 0.724–0.809 |
| Vulkan / 1 材质 | 1117.324 | 817.948 | 0.697–0.776 |
| Vulkan / 100 材质 | 1594.631 | 1148.398 | 0.645–0.801 |

这张表只使用固定同名 F2 的 benchmark JSON。旧 v5 M100 Tracy 的无锚点 filter 选入 final 的 F1/F3/F4，最后一条 warmup message 对应 F4，baseline 则只有 F2；该组完成延迟比较已从 `capture-audit.json` 排除，原始文件保留以解释采样错误。默认 filter 已改为完整 F2 名称。最终 v7 重新采集 36 份 trace 和 24 条内存曲线，所有对照都通过过滤器一致性核对，没有排除项。M1、scope、window 和 memory 的原过滤器也都有首尾锚点。

### GPU、尾部延迟与原生复用

以下为最终 v7 三组独立 F2 配对，10k 对象、3 view、dirty1。GPU 总时间来自覆盖整 flight 的 profiler，完成延迟来自同一 frame 身份的开始到真实完成记录；它不是单次 draw 的时长。每列先报告各组中位数，置信区间按配对比值计算。所有 GPU 总时间及完成延迟的均值/p95 区间均低于 1。

| 后端 / workload | GPU mean ms，baseline → final | GPU p95 ms，baseline → final | 完成 mean ms，baseline → final | 完成 p95 ms，baseline → final | 完成 p95 比值 95% 区间 |
|---|---:|---:|---:|---:|---:|
| D3D12 / M1 PrepareRecord | 0.103419 → 0.081539 | 0.130048 → 0.088064 | 2.530952 → 1.845478 | 2.713660 → 2.005200 | 0.699–0.811 |
| Vulkan / M1 PrepareRecord | 0.091006 → 0.078779 | 0.094336 → 0.084544 | 2.382178 → 1.774868 | 2.542870 → 1.941060 | 0.749–0.775 |
| D3D12 / M100 FullFrame | 0.105186 → 0.081795 | 0.133120 → 0.088064 | 3.480702 → 2.606662 | 3.674175 → 2.780760 | 0.694–0.817 |
| Vulkan / M100 FullFrame | 0.092434 → 0.080112 | 0.095922 → 0.085944 | 3.308126 → 2.494305 | 3.545600 → 2.682880 | 0.667–0.886 |

这轮 D3D12 `MaterialSceneGPU` draw zone p95 的 M1/M100 初步区间略高于 1，绝对差为约 0.2–0.6 μs，单独复验而没有用整 flight 的改善掩盖。五组独立、每组 4s 的长采样中，M1 draw mean 为 74.466→74.464 μs、区间 0.9991–1.0005，p95 为 80.578→80.896 μs、区间 0.9966–1.0072；M100 mean 为 74.584→74.574 μs、区间 0.9992–1.0014，p95 为 80.579→80.899 μs、区间 0.9921–1.0092。没有重复退化，GPU 总时间和完成延迟 p95 仍可靠下降。额外 20 份 trace、原始命令与 SHA256 见 `build_release/gpu-lifecycle-v7-draw-tail/`，统计见 `draw-tail-audit.json`；该轮新增脚本 `--backend d3d12` 仅限制 trace 后端，不改变 workload。

稳定、仅改 clear、切换 load/store 的 Vulkan scope 暖机后，`vkCreateRenderPass` 和 `vkCreateFramebuffer` 均为 0；最终无目标/外部同步的 flight，每帧 RHI allocator Reset 与 queue Submit 调用均为 1，二者分别与 frame 数逐项相等。Reset 计数是 backend wrapper 调用，不将 D3D12 wrapper 内多个 native storage 的重置混为一个 native 对象；native capacity 由 allocator fixture 另行记录。所有 native 创建、epoch、缓存预算和 owner 时点均由真实 API、trace 或 fixture probe 证明，没有新增仅供测量的生产字段。

最终审计文件为 `build_release/gpu-lifecycle-v7-correctness-audit.json`、`gpu-lifecycle-v7-captures/capture-audit.json`、`gpu-lifecycle-v7-draw-tail/draw-tail-audit.json`。v5 的完整覆盖/409 workload 对照保留对应 exe 哈希；其后仅调整 Vulkan 内部热字段和附件 metadata Reset 路径，由 v7 完整 native 回归、allocator 矩阵和本节两后端 trace 复验。

所有 build、ctest、benchmark 和 trace 串行运行；不会用一次压力覆盖的计时宣称性能收益。基线是独立解包的 7a807，runtime 与 backend 的生产行为未改。九个测量相关文件包含 benchmark fixture 对各版本 API 的适配、F1–F4 注册、相同计时区间与操作，以及 BeginFrameTiming、GPU draw zone、完成时间 plot、FIFO drain、计时区间外 native allocator 快照与 backend allocator Reset/queue Submit 测量宏；基线 native RP/FB factory 追加对应创建 zone。不能把这些 API 适配文件称为逐字相同。另有一个 test-only WSI 原生复现 fixture，未参与性能测量。基线代码审计、配置和差异见 `build_release/gpu-lifecycle-v5-baseline-source-audit.json`、`gpu-lifecycle-v5-baseline-harness-audit.json`、`gpu-lifecycle-v5-baseline-harness.patch`、`gpu-lifecycle-v5-configurations.json`；补充 SceneGpu F4 只扩大 baseline fixture 的注册，旧 exe 和新 exe SHA256 分别保留。原始未加测量点的基线 exe 仍保留。

固定机器为 i7-13700K、RTX 4080、32 GB；NVIDIA driver 32.0.16.1714；CMake 4.4.4、ClangCL 22.1.3、VS 18 2026，Release、mimalloc、相同 profiler/validation 设置，高性能电源方案。CPU Prepare/Apply/Submit、GPU、尾部延迟分别报告，不将其相加为 FPS。Scene/Material benchmark 的 scene_buffer_bytes 按每 flight 实际 Buffer::GetDesc().Size 合计，包含矩阵和参数流的分配容量，不等于 driver 的物理 resident bytes。native_block_bytes / native_allocation_bytes 来自 D3D12MA/VMA，在计时循环结束后、drain 前后采样，分别表示 allocator 预留 block 与占用 allocation；它们不含 opaque RP/FB/descriptor 元数据，不能当成 OS residency。fixture 保存数值而不保留旧 RHI pointer；没有为测量向生产对象添加状态字段。

`tools/run_gpu_lifecycle_benchmark.py` 交错运行至少三组独立 baseline/final，保存可执行文件 SHA256、完整命令、log、Google Benchmark JSON 和配对比值 95% 区间。完整 case coverage 另行运行。`tools/run_material_scene_benchmark.py` 保存 Tracy CPU/GPU events、warmup messages、完成延迟 plots 与进程 private/working set 曲线；进程内存不等于 opaque native GPU 对象或显存大小。

### 占用与预算核对

以下是最终 v7 三组独立配对的最后四分之一进程采样中位数；F2、3 view、dirty0。四组 steady native allocator 的 allocation bytes 在 drain 前后相等，baseline/final 也相等；完成后池保留存储供下一 epoch 使用，不能将 drain 解释成全部物理分配归零。完整曲线及采样长度见 `build_release/gpu-lifecycle-v7-captures/memory-summary.csv`，数值汇总见 `build_release/gpu-lifecycle-v7-report-data.json`。

| 后端 / 对象 / 材质 | Private MiB，baseline → final | Working set MiB，baseline → final | Scene MiB | Native allocation MiB | Native block MiB |
|---|---:|---:|---:|---:|---:|
| D3D12 / 10k / 1 | 339.801 → 339.402 | 185.145 → 186.172 | 2 | 21.313 | 56.250 |
| D3D12 / 10k / 10k | 383.672 → 381.410 | 227.061 → 226.451 | 2 | 23.750 | 56.250 |
| Vulkan / 100k / 1 | 552.641 → 547.059 | 310.533 → 308.137 | 16 | 37.063 | 96.000 |
| Vulkan / 100k / 100k | 1238.727 → 1241.250 | 879.238 → 885.863 | 16 | 43.125 | 96.000 |

活跃引用和 idle 缓存通过真实 native / fixture probe 分开验证：

| 对象 | 活跃使用证明 | 退休 / 缓存证明与预算 |
|---|---|---|
| Vulkan RP/FB | 两条 command 使用同一 FB，Uses 为 2；A Reset 后为 1，B Reset 后为 0。command 内多次 Prepare 对同条目仅持有一个 pin。 | 默认 RP 总量/idle 为 256/128，FB 为 1,024/256。全部 pinned 的预算失败返回 Prepare 空结果；Uses=0 后可复用，view 退出只清关联 FB。driver opaque bytes 未知。 |
| 帧内 texture pool | fixture 允许两个 64×64×4 bytes active 借用，第三次失败；原生 F1–F4 每个 flight 保留独立 8×8 目标并实际回读。 | 默认每池总量/idle 为 32/8，已知纹理字节上限 256 MiB。完成后复用 native 存储但新 lease 的 Initialized=false；预算设为 0 后 fixture texture/views 均为 0。 |
| History/readback 过程 owner | 旧 reader 的真实 gate 未释放时，替换 history 和 CPU 取消均不释放旧 GPU 使用；GPU 写入完成后延迟 CPU 消费仍持有结果存储。 | 由过程 owner 在最后 GPU 使用和 CPU 消费结束后退休，不进入帧内 transient pool；测试分别检查引用释放时点，不用一个总 bytes 猜测这两个证明。 |

稳态 native 创建由 Tracy warm 区间验证；static default SPS 的分配、Set/FlushWrites 零增量由 fixture probe 验证。没有新增只供统计使用的生产字段。

## 删除与保留审计

删除公共 RP/FB 类及工厂、RenderPassRegistry 文件/测试、WindowManager registry 通知、PSO CompatibleRenderPass 指针、SceneDraw 对象记录 cache、RasterDraw plan 表、Scene Attempt/GT→RT 执行反馈、AssetManager GPU deferred、staging 嵌套 flight pending、重复 GPU completion 字段与逐 view arena Trim。

保留 F 份 Scene 镜像、Pending/generation、CPU reader lease、Scene typed 资产使用计数、MaterialGpuState、专业 allocator 分配策略、WSI、外部 provider 的真实 owner、GPU completion 及 CPU-only 时间线。没有新增全局共享状态、异常控制结构、统一 type-erased GPU 资源总管、多队列调度或 GPU WaitIdle 稳态路径。
