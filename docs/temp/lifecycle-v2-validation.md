> - 适用: 生命周期 v2 实施结果与本机验收，2026-09-20
> - 权威: 临时验收记录，不替代长期接口契约；未执行项不计为通过
> - 锚点: `modules/runtime/tests/test_world_lifecycle.cpp`, `modules/runtime/tests/test_gpu_scene_lifetime.cpp`, `modules/runtime/tests/test_lifecycle_performance.cpp`

# 生命周期与增量渲染 v2 验收

状态：v2 实施阶段的历史验收记录；已运行、静态检查及未覆盖项分别记录，不视为 T01–T74 全部通过。
以下测试结果与 CSV 采集于移除运行时测试统计之前。当前实现已删除相关累计字段，测试改用自身 probe、
实际更新包与对象状态；旧 CSV 的 Gather、矩阵求值、owner 搬移计数及性能数值仅保留为历史数据，不能视为当前版本的测量。
基线：`88942aa8a6cad2e7f0d0973d91d2f95ea5ceaee1`，分支 `refactor/render-framework-reset`；开始时工作树干净。
实施依据为仓库外 `RadRay_Lifecycle_and_Incremental_Rendering_Plan_v2.md`。未查询远端，不把本地 HEAD 称为远端最新。

## 实施范围

- C0：记录基线；正确加载 MSVC 环境后，基线 runtime 195/195 通过。
- C1/C2：同步 owner/身份追加、统一 epoch、Pending、冻结销毁批次、稳定压缩、显式 teardown、初始层级/延迟重挂接及四态连接。
- C3：S0/S1/S2、UpdateSequence/FrameSerial、通知截止与 waiter 代次、窗口仅帧首维护、普通 drain 与 terminal abandon 分离。
- C4：Scene 绑定 owner + uncancelable raw frame owner，取消加载与保守退休的 scope 分离；真实 GPU 验收后移除 StaticMesh 第二层等待。
- C5：资产版本只验证/补全 sections 一次，实例借用不可变描述；相同 setter 短路；独立 LightStateUpdate；CPU reader lease。
- C6：功能、随机、规模、真实 GPU 测试与 384 配置 CPU 基准，结果见下。

没有新增通用命令总线、全 Scene 快照、每 flight 完整 Scene、任意 lambda 生命周期调度器或第二套 fence。
生产接口行为见 [runtime 契约](../architecture/render-framework.md)、[GPU 帧契约](../architecture/frame-and-gpu.md)、[资产契约](../architecture/asset-system.md)。

## 使用者与退休审计

| 使用者 | 覆盖与证据 |
|---|---|
| World/StaticMeshComponent | 组件绑定 ref + SceneWriter/RenderAssetLifetime；移除包真实完成前保持旧 geometry |
| 多 Scene / 独立 SceneWriter | 每 Scene 单独持有相同资产；SceneAssets 覆盖删除、重绑、候选撤销与旧 flight 退休 |
| 手工 draw | GT writable 阶段 RetainForFrameGT；真实 draw/readback 与取消通知并行，owner 不提前释放 |
| 已提交 raw 上传 | staging 属于 flight，目标 buffer 属于框架 owner；加载失败和取消后实际读回数据 |
| StaticMesh::GetRenderMesh | 生产代码使用者为 SceneWriter；直接使用者须显式 retain，RT 不复制 StreamingAssetRef |
| TextureAsset / SRV | 尚无完整生产使用者契约，保留 DeferDestroy，属于计划明确允许的保守迁移项 |
| 内置 Mesh/Texture importer | GPU 上传入口原本未实现，本轮不恢复加载管线；不把手工上传测试称为 importer 验收 |

StaticMesh::OnUnload 现在直接释放 GPU payload。测试断言 covering completion 后不再提交新 frame，
资产已卸载且 DeferredBatchCount/PendingDeferredCount 均为零。缺少等待设施时 DeferDestroy 保存 owner，终止前不能隐式释放。
主队列以外的异步队列未引入；现有主队列 fence 不能推广为多队列完成证明。

## 环境与验证口径

Windows x64；MSVC 19.51（Visual Studio 2026 Community），Intel Core i7-11700F（8 核/16 线程），
NVIDIA GeForce RTX 3060 12GB，驱动 591.44；Vulkan 1.4.325，安装的 validation layer 来自 SDK 1.4.357.0。
Debug/Release 使用相同实现；正式分配基准使用独立 MSVC Release `/MT`、mimalloc FULL、profiler 关闭，
禁用 RHI 后端/JIT/图片 codec。它测量 CPU 协议，不能与默认 `/MD` 构建混称相同配置。
Sanitizer 使用 ClangCL 22 的 ASan + UBSan、RelWithDebInfo、动态非 Debug CRT，关闭 mimalloc/profiler/RHI/JIT。

真实 GPU 有两组：开启 D3D12 debug layer 或 Vulkan synchronization validation 的数值读回；
关闭 validation 的可控 host-signaled fence 压力。后者避免验证层跟踪未 signal semaphore 阻塞压力协议。
不能把关闭 validation 的慢 fence 测试称为同时开启 validation。所有普通 native validation error 必须为零。

## 构建与测试结果

| 配置 | 结果 | 证据 |
|---|---|---|
| 基线 Debug runtime | 195/195 通过，176.94 s | build_debug/lifecycle-baseline-tests.log |
| Debug 全量 | 484 注册项：482 通过、1 disabled、1 选择性 benchmark skip，271.18 s | build_debug/lifecycle-debug-final-junit.xml |
| 最后 draft 跨 World 约束回归 | 56/56 通过，36.98 s；此前再次完成 Debug 全量构建 | build_debug/lifecycle-draft-regression.log |
| Release 全量 | 483 注册项：首次 480 通过、1 失败、1 disabled、1 benchmark skip；234.65 s。唯一 sampler 失败修复后相关 23/23 通过，14.77 s | build_release/lifecycle-release-junit.xml；lifecycle-vulkan-regression-junit.xml |
| 最后修改的 Debug / Release 回归 | 各 76/76 通过，分别 42.36 s / 31.71 s；包含 sampler、资产、World lifecycle、规模 | 两个构建目录的 lifecycle-closing-junit.xml |
| ClangCL ASan + UBSan | 133/133 通过，17.46 s；CPU 生命周期、身份、回调、资产、交付与 10k/100k 规模 | build_lifecycle_sanitizers/lifecycle-sanitizers-final-junit.xml |
| Release 性能矩阵 | 384/384 配置通过，27.60 s；含 RT allocator 合并 | build_lifecycle_perf/performance.log |

原始日志位于本地构建目录 `build_debug/`、`build_release/`、`build_lifecycle_perf/` 与 `build_lifecycle_sanitizers/`。
默认禁用的 DescriptorPublishBenchmark 和显式选择的 LifecyclePerformance.Matrix 单列，不把跳过项计为通过。

Release 全量检查发现原有 Vulkan sampler 哈希在当前 MSVC 优化下不能让正负零共用缓存。
独立小程序复现后，局部改为规范化 IEEE 位表示；完整 Vulkan layout 与 GPU 生命周期回归通过。
ASan 则发现原有异常关停测试在 manager 析构后 `Reset()` 已失效引用，与公开契约冲突。
该负面用例现在用独立原始存储保留故意未释放的引用，验证强制卸载后直接结束存储寿命，不再访问/析构悬空引用；
正常引用仍必须先于 manager 释放，没有引入可跨 manager 存活的控制块。

## T01–T74 对照

“分层覆盖”表示对应不变量由 CPU/真实 GPU/runner 多项用例共同验证，不表示每项都跑过完整笛卡尔积。
用例名使用 gtest suite 前缀；全部功能用例在 Debug/Release 运行，CPU 生命周期另做 ASan。

| 计划项 | 证据 | 覆盖边界 |
|---|---|---|
| T01 | WorldLifecycle.ImmediateCreationParticipatesInTheNextGlobalRound；StaticMeshScene.CreateCombinesStateAndFinalTransform | CPU 实测 |
| T02–T04 | TickAppendCanReallocateActorAndComponentOwners；CrossActorFirstTickDoesNotDependOnTraversalOrder | CPU 实测；交换源/目标顺序 |
| T05–T07 | CrossWorldAndNewWorldUseTheSameEpoch；WorldManager.TickSnapshotSurvivesCreationAndSelfDestruction | CPU 实测；暂停/恢复 |
| T08–T11 | DraftRegistrationAndRecursiveAppendAreExactlyOnce；DraftGetsItsFirstTickEpochWhenItJoinsDuringTick；SelfDestructionFinishesTheActiveRegistrationAndPairsOnlyStartedHooks | CPU 实测 |
| T12 | WorldLifecycleDeathTest；WorldManagerDeathTest；GT thread id guard | 无效阶段与 draft 越域实测；RT 创建拒绝另作静态检查 |
| T13–T17 | PendingStopsRemainingDispatchButKeepsTheCurrentStackAlive；ComponentCanDestroyItselfSiblingActorOrWorld；ParentDestructionCoversQueuedChildrenAndRejectsStaleIdentities | CPU 实测 |
| T18–T20 | SceneUpdates.DestroyBeforeCollectionCancelsCreateAndAllowsGenerationGap；SealedCreateSurvivesSourceDestructionUntilOrderedRemoval；父覆盖子测试 | CPU 实测 |
| T21–T25 | DestructionCallbacksSeeCompactedSurvivorsAndMaySpawnDrops；DestructionRequestsMadeByCallbacksWaitForTheNextBatch；CreatedThenPendingInADestroyHookNeverPublishesAnEmptyPrimitive；随机身份测试 | CPU 实测 |
| T26–T30 | PausedCpuOnlyWorldStillCommitsDestruction；InitialParentIsVisibleDuringRegistrationAndAppendSafeDuringNotification；ReparentIsDeferredAndRejectsCyclesCrossWorldAndShear；ParentRemovalDetachesOtherActorsChildrenWithKeepLocal | CPU 实测 |
| T31–T36 | ConnectingCallbacksAppendExactlyOnceAndRequestNextBatchDisconnect；DisconnectCallbacksCanCreateButCannotRejoinTheDyingScene；ReconnectSurvivesCoalescingAndPendingSourcesAreNotCollected | CPU 实测 |
| T37–T38 | CollectionRejectsFlushAndSchedulerSideEffects；SceneAssets.ReconnectRejectsOldReadyRequestAndPublishesWhilePaused；旧请求/复用 primitive 测试 | CPU 实测；所属 Application asset Pump guard 静态检查 |
| T39 | SceneDeliveryRunner / MultiWorldSceneRunner | D3D12/Vulkan；单/双线程；F=1/2/3/8 |
| T40–T42 | SceneDeliveryDeathTest.DuplicateAndOutOfOrderPacketsAreRejectedBeforeApply | CPU 实测；同 generation transform、旧 completion |
| T43 | SceneDeliveryRunner；RuntimeFoundation.DroppedPresentationRecovers | 真实 runner；有序 Apply 独立于绘制 |
| T44 | SchedulerFreezesItsBatchAndRevalidatesCanceledWaiters；AssetSlot/WindowOperations 的批次与取消测试 | CPU 实测；通知类各有入口截止 |
| T45–T47 | RuntimeFoundation.*InputDuringGpuStall / *WindowMutationWaitsForGpu / *WindowOperations | 双后端、单/双线程真实窗口；维护只等已发布边界 |
| T48–T49 | AbandonDoesNotPublishOrRequireCompletion；DuplicateAndOutOfOrderPacketsAreRejectedBeforeApply；ApplyWaitsForThePreviousCpuReader | CPU 实测；可跨线程转移 reader lease |
| T50–T51 | GpuSceneLifetime.*DelayedDrawOutlivesActor / *ThreadedThreeViewDraw | 真实 GPU、慢 fence、Actor 提前析构、无额外帧退休 |
| T52–T54 | SceneAssets.DestroyingOneSceneRetainsSharedAssetUntilLastSceneCompletes / StandaloneWriterCoalescesReplacementAndRetainsFinalBinding / SameFrameRemovalAndNewBindingKeepTheSharedAssetResident；GpuSceneLifetime.*DirectDrawOwnerSurvivesCanceledNotification | 共享/独立 writer 为 CPU owner 实测；手工 draw 为真实 GPU |
| T55 | SceneAssets 旧请求、换 mesh、注销、取消和重连用例 | CPU 异步实测 |
| T56–T57 | GpuSceneLifetime.*SubmittedUploadSurvivesLoadFailureAndCancellation / *DirectDrawOwnerSurvivesCanceledNotification | 真实上传与读回；取消只停止业务通知 |
| T58 | GpuSceneLifetime 删除包 rendered=false；SceneDelivery discard；FrameUpload | 真实 fence 仍退休；未执行的应用上传由 producer 重试，未新增自动 importer 调度 |
| T59–T61 | AssetSlotTest.CascadedDependenciesWaitForTheNextPump；Scheduler/Ready 停止；MissingCompletionFacilityKeepsDeferredOwnerUntilTerminalCleanup | CPU 实测；10k 依赖链批次回收 |
| T62–T65 | LifecycleScale.SharedMeshPreparationAndTransformCapture；LifecyclePerformance.Matrix | 10k/100k；共享准备一次；变换捕获一次；稳定压缩单独计数 |
| T66 | DeepHierarchyKeepsImmediateValuesAndSeparatesNotificationFromCapture；性能 depth=1/8 | 256 层功能测试；通知/矩阵/Gather 分开计数 |
| T67 | 性能 View=1/3；GpuSceneLifetime.*ThreadedThreeViewDraw | CPU View 扫描与真实三视口 draw 分开报告 |
| T68 | LightUsesOneTypedUpdateAndNoMeshState | 100 次 setter → 1 次 Light 更新，mesh/transform 包为空 |
| T69 | GpuSceneLifetime 全 suite；SceneAssets 重绑用例 | 删除/手工资源真实 GPU；复杂重绑由分层 owner 测试覆盖 |
| T70 | PartialBootstrapShutsDownWithoutAPublishedFlight；InitializeRuntime/DestroyRuntime 清理审计 | CPU bootstrap 实测；原生窗口初始化失败路径静态检查 |
| T71 | Ready、DestroyWorld、窗口 close 各自的 cutoff/退出测试及随机交付 | 分层覆盖；未专门注入同一原生事件栈的三方交错 |
| T72 | StoppingRejectsAssetProduction；RejectsCreationAndDriversDuringInvalidPhases；scheduler 停止；WindowOperations.ShutdownStopsPendingAndFutureOperations | CPU 实测与完整 runner 关停 |
| T73 | D3D12DeviceLossDeathTest.StopsBeforeReportingCompletion 的 5 个原生 RemoveDevice 用例；Vulkan timeline 查询失败 abort | D3D12 故障注入实测；Vulkan 仅静态检查；保留 fail-stop，无伪造成功 completion |
| T74 | RandomizedIdentityAndPendingStateMatchesReference；RandomizedSceneDeliveryMatchesLogicalValuesAcrossDelayedCompletions | 固定 seed 0x260920 / 0x74c620；3000 身份操作 + 1500 场景交付步骤 |

## 性能结果

384 组配置通过，27.60 s。矩阵为 10k/100k × changes 0/1/1%/100% × Views 1/3 × flights 1/2/3
× depth 1/8 × 共享/唯一资产 × 单/双线程。每配置先预热 flights+1 次，再采样 7 次；p95 是这 7 次的最大值。
Tick 时间包含本轮 setter；Collect/Seal、RT Apply、AABB 视图扫描、completion + AssetManager.Pump 分开计时。
CPU benchmark 不提交 GPU、不维护窗口；真 GPU/窗口行为由独立用例验证，以下数据不表示渲染 FPS。
分配统计来自现有 mimalloc FULL，RT 在 GT 等待时合并其统计；数据为 allocator 报告的累计分配字节，不是活跃内存或显存。
小于 1 μs 的时长接近计时开销，不用于推导细粒度速度提升。

原始数据：[384 组帧阶段](lifecycle-v2-perf.csv)、[144 组删除批次](lifecycle-v2-delete.csv)、[48 组创建阶段](lifecycle-v2-setup.csv)。
帧 CSV 的分配是 7 个正式样本总和；Gather/矩阵计数包含该配置的预热。删除批次依次删除 1、1%、50%，后两批的存量已减少。

以下选取 F=2、depth=1、共享资产、单线程、View=1，单位 ms；括号内为 p95：

| 对象数 | 修改数 | Tick 中位数 | Collect/Seal 中位数（p95） | RT Apply 中位数（p95） |
|---|---|---|---|---|
| 10000 | 0 | 0.0739 | 0.0001（0.0002） | 0.0001（0.0001） |
| 10000 | 1 | 0.0740 | 0.0003（0.0004） | 0.0001（0.0001） |
| 10000 | 100 | 0.0746 | 0.0083（0.0085） | 0.0017（0.0017） |
| 10000 | 10000 | 0.2167 | 0.8538（0.9247） | 0.1808（0.2381） |
| 100000 | 0 | 1.5230 | 0.0005（0.0006） | 0.0002（0.0004） |
| 100000 | 1 | 1.5481 | 0.0015（0.0019） | 0.0004（0.0006） |
| 100000 | 1000 | 1.4639 | 0.0938（0.1613） | 0.0228（0.0230） |
| 100000 | 100000 | 3.5235 | 10.9557（11.1422） | 2.4086（2.8414） |

全部 384 组在预热后的正式值修改样本中记录到 0 次分配；创建阶段全部记录到非零分配。
静止配置的 Gather 和矩阵求值均为 0；共享资产始终只验证/补全一次，变换更新不生成 mesh state。
增加 View/flight 不增加单轮源 Gather；深树仍有祖先计算与即时通知成本，未声称 dirty 合并消除了这些工作。
当前 `sizeof(Actor) + sizeof(StaticMeshComponent)` 为 392 B；不包含 owner 容器、身份表、资产或队列动态容量。

100k、F=2、浅树、共享资产的创建阶段：27.40 ms，584,978 次分配，累计 282,326,512 B。
100k、F=2、浅树、唯一资产的创建阶段：58.81 ms，1,568,017 次分配，累计 356,879,064 B。

稳定压缩成本单列，删除一个 Actor 仍访问整个受影响 owner 容器；删除半数时只压缩一次。
`compacted_owner_bytes` 是移动的 owner 记录字节，不能用来估算 mesh 数据复制量。未测量窗口维护耗时，也没有改动前同配置矩阵，故不报告提升百分比。

## 限制

- Windows 本机验证不替代其他平台；ASan/UBSan 已执行，TSan 本机无运行库，未运行。
- 保留 TextureAsset 保守退休与尚未实现的内置 importer，均已在长期契约中标出。
- 设备失效沿用现有后端 fail-stop；已执行 D3D12 RemoveDevice，未注入系统 TDR 或 Vulkan device-loss，没有新增设备重建/恢复协议。
- 未建立改动前相同矩阵的性能数据，因此只报告成本和不变量，不宣称速度提升百分比。
- Profiling/计数覆盖现有实际工作；GPU payload 精确显存占用和所有跨系统退休原因的完整 trace 尚无通用归因接口，不能把 owner 记录字节冒充显存字节。
