> - 适用: 按主题查找已保存的计划、设计快照、实验、验收与附件
> - 权威: 本文是可更新的历史目录索引；各记录仅描述其注明的状态与基线，不作为当前契约
> - 锚点: `docs/history/`, `AGENTS.md`, `tools/check_docs.py`

# 开发历史目录

记录与附件统一保存在 `docs/history/`。经用户明确要求或同意后新增；AI 可询问是否留档，日常改动不自动写入。[归档规则](../guide/documentation.md#开发历史)说明命名与日期选择。

以下文件保留历史设计、实验结果、原始数据和验收覆盖；内容仅适用于记录中的基线。当前契约从[架构地图](../architecture/overview.md)进入所属页面。
CSV/JSON/图片与报告一起保留；`build*/` 中未入库的日志和 trace 不保证随 checkout 存在。

| 历史资料 | 记录范围 |
|---|---|
| [Runtime Batch 级命令收敛与热路径直达优化计划（A + B 通用方案）](2026-11-06-runtime-batch-command-and-fast-submit-plan.md) | `7e465fe` 基线下定位前两轮优化暴露的三大结构性缺陷（Element 过早拍扁导致 5.4 倍膨胀与 `InlineVector` 溢堆、`VisibleMeshDrawCommand` 7B 空洞膨胀至 `40 B`、`Submit` 内层动态构造/排序开销），排除单流内联特化，制定回归 Batch 级缓存 + `ElementMask` 连发 Draw（方向 A）与预排序连续顶点流 + 定长槽位直查 + `32 B` 可见项（方向 B）的通用重构计划 |
| [Runtime MeshDrawCommand SoA 绑定池拆分优化计划](2026-11-06-runtime-mdc-soa-pools-plan.md) | 2026-11-06 基于移除命令快照后的 Tracy 实测（`MeshDraw.Submit` 因 `472 B` 胖命令冷读取回升至 `0.261 ms`），将 `VertexBuffers`、`ParameterSets`、`PushConstants` 拆入独立 SoA 池、将 `MeshDrawCommand` 收敛为平凡热结构并支持同 Batch 句柄共享与提交短路去重的实施计划 |
| [Runtime Scene 帧内借用对齐与 MeshDrawCommand 寻址/布局优化计划](2026-11-06-runtime-scene-borrow-and-mdc-layout-plan.md) | 2026-11-06 基于配对 Tracy 采样定位每帧命令快照净倒退根因，对齐 UE5 `FScene` 渲染线程模型放开 `RunFrame` 内 `Scene` 只读借用、移除快照拷贝、改为直达命令寻址并重排 `MeshDrawCommand` 热冷缓存行的阶段性计划 |
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
