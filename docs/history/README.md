> - 适用: 按主题查找已保存的计划、设计快照、实验、验收与附件
> - 权威: 本文是可更新的历史目录索引；各记录仅描述其注明的状态与基线，不作为当前契约
> - 锚点: `docs/history/`, `AGENTS.md`, `tools/check_docs.py`

# 开发历史目录

记录与附件统一保存在 `docs/history/`。经用户明确要求或同意后新增；AI 可询问是否留档，日常改动不自动写入。[归档规则](../guide/documentation.md#开发历史)说明命名与日期选择。

以下文件保留历史设计、实验结果、原始数据和验收覆盖；内容仅适用于记录中的基线。当前契约从[架构地图](../architecture/overview.md)进入所属页面。
CSV/JSON/图片与报告一起保留；`build*/` 中未入库的日志和 trace 不保证随 checkout 存在。

| 历史资料 | 记录范围 |
|---|---|
| [Runtime 几何池空页回收、MDI 批次构建缓存评估与瞬态参数集快轨计划](2026-11-22-runtime-geometry-trim-and-parameter-fastpath-plan.md) | 2026-11-22 基于 `af121f2`（`C1`～`C5` 落地后 `develop` 基线）四轮审查收敛定稿：制定按 `_pool ? _page : 0u` 判空的 `SubAllocation::GetPageIndex()` 稀疏页槽号直读（明确 `GetPageCount()` 不作页槽号上界，区分 `GetVertexInput()` 页存活期引用与 `GetGpuBuffer()` 底层缓冲寿命，保留析构逐页全归还断言）、基于 `Pages[index] == nullptr` 与返回 `Nullable<unique_ptr<GeometryPage>>` 统一建页工厂的 `TrimEmptyPages()` 空页撤销提交、固定 `AddPassReadAccess` 按构图时 `GpuBuffer` 身份导入已提交页及建页/重提交/`Trim` 须先于 `AddPassReadAccess` 的时序契约，保持提交阶段双槽缓存不动并仅评估精确保留无绑定槽跨池合批谓词的 `BuildMeshPassIndirectDraws` 活跃批次签名标量缓存，以及拆分 `TransientParameterSetPool` 有序输入零拷贝 + 全链路 `activeBindings` 读写 + 首次创建先入池 `Sets` 再推进 `Used` 与 `ResetBindingsForReuse()` 回滚快轨（`R3a`）和类内自包含 `kEmptyBucket`、桶命中同步写回、`FullTableClearParameterPool` 含 `BeginFrame()` 同步重置及正负双向三方基准门禁的 1-entry MRU 评估项（`R3b`） |
| [Runtime 场景提交、几何池与 RDG 资源声明 CPU 热路径优化计划](2026-11-22-runtime-cpu-hotpath-and-rdg-access-optimization-plan.md) | 2026-11-22 基于阶段三 MDI 底座落地后的 `develop` 基线，制定 `SortVisibleCommands` 奇数趟基数排序按奇偶趟预对齐 `capacity()` 后的 $O(1)$ 向量缓冲交换（消除 1/3/5/7 字节差异键下末尾整表 `std::copy_n`）、`MeshDrawCommandElements` 入库预验 `ValidatedExtraMask` 与含 `IndexBuffer` 状态缓存绑定的单元素直通快轨、`VertexBufferPool`/`IndexBufferPool` $O(1)$ 增量空闲容量计数、`RDGUserPass` 自适应小表线性扫描（$\le 16$ 项零分配）加 `data.MemoryResource` 纯 `slot` 开放寻址哈希桶（$> 16$ 项按 Pass 实际访问规模分配并回查 `Accesses[slot]`）的 `AddAccess`/`ResolveTextureView` 平均 $O(1)$ 查重，以及 `Scene::GatherVisibleMeshDrawCommands`/`GatherVisiblePrimitives` 双入口 `std::span` 槽表外提与 Google Benchmark 配对测量计划（`C1`～`C5`） |
| [Runtime 与 RHI 阶段二实机接入反馈优化与阶段三 MDI 场景底座实施计划](2026-11-21-runtime-phase2-optimization-and-phase3-mdi-plan.md) | 2026-11-21 结合下游高频即时命令流与大视距世界实机压测反馈及静态审阅，制定 RDG 构图期零全局堆分配（Pass 回调/名称 PMR 化、登记前 RAII 异常安全析构与 `Generated` 64B SBO，含 `128 KiB` 帧槽保留容量内零分配与溢出块归还）、`TransientParameterSetPool` 开放寻址平坦去重表（按实际占用清桶，校准分配探针，覆盖参数集解绑复用及高峰后稀疏帧）、RDG 默认 `ShaderRead` 全 mip 视图对齐（保留 `Prepare` 默认视图预建及 `RenderTarget`/`Depth*` 单 mip 查询契约）与 `RDGTransientResourcePool` 8 帧滑动窗口淘汰/缓冲分桶复用（含大桶失败回退原请求尺寸）、`RDGAttachmentSlice` 单级附件范围与 `RDGBuilder` 默认真实设备约束及同页 `Vertex` / `Indirect` 切片偏移 0 / 4 B 验收、DXIL `cbuffer` 免手写 `[RootSignature]` 提升为 Push Constants（同步迁移 `ActivePushes` 与离线 CLI `radray_shader_reflect`）及 `AllowDxilOnly` 的纯 SPIR-V 编译和跨变体映射合并（`O1`～`O5`），以及 `DeviceContext` 公开构造函数默认初始化 `gpu::PipelineCache`（含先于 `_device.reset()` 的安全析构顺序）、`SortVisibleCommands` 稳态有序 $O(N)$ 短路与覆盖 `Scene`/动态 Batch/含 `ArrayStride`+`StepMode` 去重的顶点与实例流容量校验的全链路无索引 `MeshDrawCommand`、`PrepareLayout` 计数与 `Write` 打包解耦（消除生成器登记顺序依赖）、构图期设备级直接回退、按 `IndirectFirstInstance` 逐批回退与按 `slice.Offset` 录制的 MDI 自动合批提交器及纯值类型 `MaterialParameterValue`（含行/列主序矩阵自动转置）扩展（`M1`～`M3`）的实施计划 |
| [Runtime 与 RHI 阶段二实施复盘、架构成因与过度防御清理计划](2026-11-21-runtime-phase2-review-and-cleanup-plan.md) | `c6d2e85` 基线下复盘阶段二实施四项问题与 `DeviceLost` Corner Case 膨胀的高层架构成因，并制定修复 `FrameScheduler::RunFrame` 丢失返回遗漏复位 Debug `_inFrameCallback`、精简 `Fence::WaitFor` 冗余查询（保持丢失态恒返回 `false` 契约）、移除 `DeviceVulkan::_lostIdleComplete` 伪多线程等待（保留单次同步 `vkDeviceWaitIdle` 与返回码检查）及在图返回时已丢失的情况下跳过帧尾时间戳、查询解析和读回屏障，并配对调用逻辑 `CommandBuffer::End()` 的修订迭代计划 |
| [Runtime 与 RHI 阶段二实施记录](2026-11-21-runtime-phase2-implementation-notes.md) | 实施中遇到的问题、修复与实测结果 |
| [Runtime 与 RHI 第二阶段：RDG 局部写入保活、顺序屏障、子区域读回与设备丢失终态实施计划](2026-11-20-runtime-phase2-rdg-and-readback-plan.md) | 2026-11-20 承接第一阶段落地后的 `develop` 基线，补齐 RDG 光栅 `SetRenderArea` 与附件子资源范围声明、非整纹理覆盖写 `Preserve` 保活防死代码误裁与 `CanMerge` 收紧、多 mip/多层与子矩形 `AddClearColor`/`AddClearDepthStencil`、`RDGPassHandle`（含非动态缓冲零字节空上传返回无效句柄）+ `AddOrderDependency`/`AddOrderBarrier` 显式顺序控制、含多切片 `RowsPerImage` 对称处理与 Vulkan 队列族传输约束（`granularity × blockDim` 压缩块步长及纯传输队列 `bufferOffset` 4 字节下限）的 RHI `CopyTextureToBufferRegion`、单平面深度整子资源 `CopyTextureToTexture`（Vulkan 非图形队列仅支持单采样深度拷贝） 与保持旧重载整 mip 契约的 `AsyncReadbackQueue` 子区域异步读回，以及覆盖资源/命令缓冲（含录制开启与中途丢失时校验层状态闭合）/队列/交换链的 `Device::IsDeviceLost()` 单向终态（Vulkan 全程仅单次 `vkDeviceWaitIdle` 且早于首次帧槽回收）、`Prepare/Record/Submit` 设备丢失非 abort 传导（保留健康设备可选 `PrepareTiming` `WARN` 降级出帧，必需资源 OOM 与提交违约维持 fail-fast）、防伪造完成度 `Fence` 及完整释放原生句柄的丢失态安全析构实施计划（附阶段三至五引擎侧前置约束） |
| [Runtime 与 RHI 第一阶段通用渲染底座补齐计划](2026-11-19-runtime-phase1-foundations-plan.md) | `4a8bf33` 基线下纵向贯通 RHI/RDG 全链路 BCn 块布局与 `Cube`/`Dim3D`（含非紧密 `RowsPerImage`）上传及全能力 `GpuTexture`/子视图缓存/`TextureAsset`、升级 D3D12 Tier 1 百万资源堆与 Sampler Table 去重并在 `FrameSlot` 内聚先于帧对象析构解绑的 `TransientParameterSetPool`、新增 D3D12 自持输入副本的 `PipelineCache` 与固定资产源联合退役 `PsoCache`、补齐三路所有权分流子视图/单 mip 整资源 Clear/`RDGBlackboard`/四态 `AsyncReadbackQueue`/内建 GPU 时间戳采样的已确认实施计划 |
| [Runtime 资产仓库极简收敛、事务热替换与统一帧退役重构计划](2026-11-19-runtime-asset-hot-reload-plan.md) | `aea0dee6` 基线下剥离 `AssetManager` 内置异步协程调度器与 `ReplaceShader` 特化后门，收敛为生而有效的 8 字节稳定句柄仓库、正反向依赖追踪、`Revision` 内容版本、`AssetTransaction` 原子事务热替换（含 `ClassifyReload`、`StageMaterial` 与三色 DFS 验环）及零堆分配侵入式 `AssetGarbageBatch` 帧首退役机制的已确认重构计划 |
| [Runtime MeshPassProcessor 无状态化、单步发布与移除软失败路径重构计划](2026-11-09-runtime-stateless-mesh-pass-processor-plan.md) | `75da7a9` 基线下移除 `MeshPassProcessor` 内部可变 `DrawListContext` 绑定与哑上下文占位、将 `MeshPassDrawListContext` 两阶段提交协议收敛为单步 `AddCommand`、取消命令构建软失败回滚与跨帧重试路径（全链路 `void` + 违约 `RADRAY_ABORT`）的已确认重构计划 |
| [Runtime 帧槽准备阶段、RDG 只读组合状态、几何子分配池与分页批量上传演进计划](2026-11-09-runtime-geometry-pool-and-upload-plan.md) | 2026-11-09 的设计快照：BeginFrame、BufferStates、SpanAllocator、顶点/索引池与分页上传；当前已实现接口与约束见 [Rendering runtime](../architecture/runtime.md#几何子分配池与持久映射上传) |
| [Runtime 架构边界收紧、状态驻留与数据导向重构计划](2026-11-08-runtime-dod-and-state-interning-plan.md) | 2026-11-08 基于架构审视，收紧 `Scene` 与缓存池封装边界（终结公开裸容器后门）、实现 `VertexBuffers`/`Payloads` 跨 Batch 引用计数驻留并将 `MeshDrawCommand` 收敛至 `64 B` 单缓存行、正交拆分 32 位 `StateSortKey` 与 `VisibilitySortKey` 支持 Pass 级 `StateFirst`/`DepthFirst` 排序、构建期预计算 `ContiguousMergeMask` 合并单实例连续子网格、引入 `16 B` Pass 热表与零分配稳定基数排序及修复 `SparseSet` 几何扩容的分阶段重构计划 |
| [Runtime Batch 级命令收敛与热路径直达优化计划（A + B 通用方案）](2026-11-06-runtime-batch-command-and-fast-submit-plan.md) | `7e465fe` 基线下定位前两轮优化暴露的三大结构性缺陷（Element 过早拍扁导致缓存膨胀与 `InlineVector` 溢堆、`VisibleMeshDrawCommand` 7B 空洞膨胀至 `40 B`、`Submit` 内层动态构造/排序开销），排除单流内联特化，制定回归 Batch 级缓存 + `ElementMask` 连发 Draw（方向 A）与预排序连续顶点流 + 定长槽位直查 + `32 B` 可见项（方向 B）的通用重构计划 |
| [Runtime MeshDrawCommand SoA 绑定池拆分优化计划](2026-11-06-runtime-mdc-soa-pools-plan.md) | 2026-11-06 针对命令快照移除后的绑定数据布局，将 `VertexBuffers`、`ParameterSets`、`PushConstants` 拆入独立 SoA 池、将 `MeshDrawCommand` 收敛为平凡热结构并支持同 Batch 句柄共享与提交短路去重的实施计划 |
| [Runtime Scene 帧内借用对齐与 MeshDrawCommand 寻址/布局优化计划](2026-11-06-runtime-scene-borrow-and-mdc-layout-plan.md) | 2026-11-06 针对每帧命令快照的额外复制与清理，对齐 UE5 `FScene` 渲染线程模型放开 `RunFrame` 内 `Scene` 只读借用、移除快照拷贝、改为直达命令寻址并重排 `MeshDrawCommand` 热冷缓存行的阶段性计划 |
| [Runtime RDG Pass 重排与合并实现计划](2026-11-06-runtime-rdg-pass-reorder-merge-plan.md) | `6050639` 基线下补齐 RDG WAR 依赖、确定性贪心拓扑重排、Raster/Compute Pass 合并折叠与批量 Barrier 下发的已确认实现计划 |
| [Runtime RDG 接入与 FramePipeline 跨线程接口拆分计划](2026-11-05-runtime-rdg-pipeline-interface-plan.md) | `e443cef` 基线下针对 RDG 剔除避免 MeshPass 无效收集/排序、切断 `Record` 对 `Scene` 借用以支持跨线程分离的分阶段接口演进计划 |
| [Runtime MDC 性能优化计划](2026-11-04-runtime-mdc-performance-plan.md) | `c8bc3bd` 基线下針對 `SubmitMeshDrawCommands` 预检/状态去重、`Scene` $O(1)$ 脏追踪及 `GatherVisible`/`VertexBuffers` 内联的已确认设计方案 |
| [Runtime 精简前设计归档](2026-10-31-runtime-design-archive.md) | `07d41dc` 的对象关系、场景/绘制/RDG 分层、所有权、帧提交和旧测试入口；重设计以保留的环境与设备为起点 |
| [显式 RHI 绑定布局转向与实施计划](2026-10-22-explicit-rhi-binding-layout-rationale.md) | shader artifact 布局权威转向调用方定义的原因、实施前所有权快照、当时的接口与验收要求；记录保留实施前的设计决定 |
| [Runtime 完整归档](2026-10-21-runtime-archive.md) | 源码恢复、设计与操作快照、草案附件及归档时验证入口 |
| [Runtime 重写原因](2026-10-21-runtime-rewrite-rationale.md) | 补丁叠加的基础问题、用户决策、源码核查与下一版方向 |
| [旧渲染框架设计](2026-09-14-render-framework-design.md) | a68237f 时的分层、数据流与重构范围 |
| [旧渲染框架附件](2026-09-14-render-framework-legacy.md) | 原 Scene/proxy、Forward、服务和材质契约 |
| [旧 Renderer foundation 附件](2026-09-14-renderer-foundation-legacy.md) | 原 RenderGraph、pool/history、draw list |
| [旧 ImGui 附件](2026-09-14-runtime-imgui-legacy.md) | 原 UI 模块的集成、输入和字体设计 |
| [生命周期 v2 验收](2026-09-20-lifecycle-v2-validation.md) | 2026-09-20 覆盖边界与 CSV |
| [Scene 变换实验](2026-09-23-scene-transform-benchmark.md) | 世界矩阵转到 RT 的收益和代价 |
| [直接导入实验与回退](2026-09-23-scene-direct-import-benchmark.md) | 未采纳的 freeze/ack、全量求值方案及配对数据 |
| [World 紧凑变换存储](2026-09-24-world-transform-store-benchmark.md) | 已采纳布局的性能与接口迁移记录，含 JSON 原始结果 |
| [100k 场景诊断](2026-10-06-scene-sync-100k-performance.md) | 自动实例化之前的 copy/draw 瓶颈；后续实现不能沿用旧帧率 |
| [第一阶段验收](2026-10-07-phase1-rhi-command-allocator-validation.md) | allocator/pipeline 接入时的覆盖与 CSV |
| [材质和实例化验收](2026-10-13-material-instancing-performance.md) | 双后端 CPU/GPU、churn 和内存对照，含回退项及图表 |
| [GPU 生命周期输入计划](2026-10-13-gpu-lifecycle-plan-vulkan12-per-flight.md) | 7a807 基线的迁移目标与验收矩阵，已执行且后续 API 有变化 |
| [GPU 生命周期实施验收](2026-10-13-gpu-lifecycle-implementation.md) | 2026-10-13–14 的实际覆盖、限制及性能证据；不替代当前 RHI |
| [帧循环重构输入计划](2026-10-16-frame-loop-refactor-plan.md) | 93a8117 基线的旧等待关系、职责与确认后的重构契约 |
| [帧循环重构实施验收](2026-10-16-frame-loop-refactor-validation.md) | 消息/进度状态机、延迟输入、GPU 排空的测试与性能证据及限制 |
| [Render Framework 重构输入](2026-10-16-render-framework-refactor-plan.md) | f20c56f 与 sampler 工作区补丁的所有权快照、完整组复用目标与前后验收口径 |
| [Render Framework 重构验收](2026-10-16-render-framework-refactor-validation.md) | 双后端 descriptor 前后 42 项、真实完成与退出确认、完整回归、Release 性能回退及环境限制 |
| [Render Framework 回退定位](2026-10-16-render-framework-regression-diagnosis.md) | 未改生产代码的分段计时、CPU 完成清理与固定绑定值规模实验；定位重复请求展开与临时对象周转 |

## 补充记录与归档附件

| 文档 | 文件日期 |
|---|---|
| [Render Framework CPU 回退修复计划](2026-10-16-render-framework-regression-fix-plan.md) | `2026-10-16` |
| [开发时资产数据库](2026-10-21-runtime-asset-database-snapshot.md) | `2026-10-21` |
| [资产系统](2026-10-21-runtime-asset-system-snapshot.md) | `2026-10-21` |
| [构建与测试](2026-10-21-runtime-build-test-snapshot.md) | `2026-10-21` |
| [帧节奏与 GPU 上传](2026-10-21-runtime-frame-and-gpu-snapshot.md) | `2026-10-21` |
| [实例绘制](2026-10-21-runtime-instancing-snapshot.md) | `2026-10-21` |
| [下一版帧循环草案原文附件](2026-10-21-runtime-next-loop-draft.md) | `2026-10-21` |
| [性能测量](2026-10-21-runtime-performance-snapshot.md) | `2026-10-21` |
| [Runtime 宿主、World 与渲染场景](2026-10-21-runtime-render-framework-snapshot.md) | `2026-10-21` |
| [Renderer foundation 状态](2026-10-21-runtime-renderer-foundation-snapshot.md) | `2026-10-21` |
| [ImGui 适配状态](2026-10-21-runtime-runtime-imgui-snapshot.md) | `2026-10-21` |
| [Shader pipeline](2026-10-21-runtime-shader-pipeline-snapshot.md) | `2026-10-21` |
| [共享文档中的 runtime 说明](2026-10-21-runtime-shared-doc-excerpts.md) | `2026-10-21` |

非 Markdown 的 CSV、JSON 和图片由对应报告中的附件链接定位，保留原文件与原始数据。
