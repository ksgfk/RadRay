> - 适用: 理解为何整体移除旧 runtime，以及下一版重写的出发点
> - 权威: 用户重写决策、归档源码核查与草案对照；不是新架构定稿
> - 锚点: `docs/history/2026-10-21-runtime-archive.md`, `docs/history/2026-10-21-runtime-next-loop-draft.md`

# 为什么重写 runtime

状态：整体归档并移除旧实现的决策已确认；新循环尚未实现。
核查基线：`751aca4e6a70203e6a62f9de67893b33463b94fc`，文件树等同原 `ed23aef93b779082d3efab221bbe52d9f30051f0`。
完整范围和恢复方法见 [归档记录](2026-10-21-runtime-archive.md)。

## 决策主因

用户的判断是：现有底层循环长期通过局部补丁迭代，而没有重新设计基础执行模型。补丁相互叠加，越积越多；
部分系统设计仍值得借鉴，但它们依然建立在不理想的基础上。因此本次先整体归档与移除，从底层时序、职责和失败模型重新出发。

这是一项关于可理解性、可验证性与长期开发成本的架构决策。已有测试通过不等于执行模型容易维护；
本记录也不把状态字段数量本身视为错误证据，或宣称旧系统在所有场景下不正确。

## 归档版本中的具体负担

以下源码路径属于归档基线，通过归档 commit hash 查阅；结论来自源码核查，不是本次新运行的故障实验。

| 关注点 | 现有结构与代价 | 草案尝试改变的方向 |
|---|---|---|
| 消息处理和帧推进 | `application_loop.cpp` 的 Drive 泵原生事件；TickModal 又进入 PrepareFrame/ExecuteFrame。`_nativeDepth`、`_advancing`、`_modalTick`、`_driving` 等条件共同约束重入与销毁，修改一条路径必须考虑其他入口 | Host 独立泵消息，GT 运行 app，RT 负责录制提交；减少原生嵌套派发对业务帧推进的直接影响 |
| 正常运行、维护和退出 | ApplicationLoop 的 Running、MaintenanceCPU/GPU、StoppingCPU/GPU 状态与独立 drain thread 配合；PrepareFrame、AdvanceMaintenance、AdvanceStop、CollectDrain 分别推进局部步骤 | 用显式帧交接、屏障、退休与终止协议集中描述生命周期和交接条件 |
| 完成、取消与回调交付 | ApplicationScheduler、FrameTimeline、资产和窗口系统各有等待、取消和交付职责；Application 的 BeginStoppingRuntime、完成泵、Shutdown 与 DestroyRuntime 共同决定拆除顺序 | 明确完成凭据、记录的唯一处理权、安全点交付和任务 drained 确认，分别定义取消、完成、失败 |
| 底层与高层装配 | `application.cpp` 的 InitializeRuntime 直接创建窗口、GPU、RenderSystem、资产、Scene、World 和 Renderer，帧与拆除路径也知道这些系统 | 底层先独立支持无 GPU、headless、standalone 与可选 RT，再由参与者协议接入高层 |
| 失败与部分完成 | 旧路径的整体退出、GPU 排空及 abort 处理，需要结合各系统约束理解；尚没有草案提出的统一部分提交账本和 RunResult 契约 | 区分已提交凭据、acquire 消费、present 状态和部分失败，明确终止时应等待与释放的对象 |

这些要求跨越线程归属、消息泵、完成通知、资源退休与高层接入。在旧控制流上逐项加分支，会继续增加需要同时理解的条件。
本次选择先去掉旧集成基础，为后续独立设计和验收留出清楚的边界；不在归档改动中引入新协议。

## 可以借鉴的内容

- 真实 GPU 完成后再复用 flight、command storage 与资源，区分逻辑删除、退休和物理回收。
- 显式所有权、generation/身份校验，以及 CPU 交付和 GPU 使用者退出的区分。
- World/Scene 增量交付、稳定实例集合、资产引用与加载桥接、绑定存储复用。
- 双后端离屏读回、生命周期验证、基准与原始测量证据。

这些是后续评估的参考，不意味着逐文件搬回旧实现，也不意味着沿用其宿主和帧循环依赖。

## 历史性能问题的边界

[2026-10-16 回退诊断](2026-10-16-render-framework-regression-diagnosis.md)记录过：descriptor 存储复用后，
CPU 仍逐 draw 展开绑定请求，重复构造临时容器，并在帧完成时承担集中清理成本。
它证明局部缓存命中不自动消除前置准备工作，也说明数据组织和处理频率需要共同设计。

该报告有自己的基线和工作区补丁；之后原历史还有材质重构、优化提交。其耗时和缺陷不能直接推广为 A 的当前表现。
本次不重新测量已归档 runtime，也不声称这些性能问题全部仍存在。重写的主因是上述基础模型和维护方式的判断。

## 下一版草案的地位

输入是用户提供的桌面 `a.md`，完整原文与 SHA-256 见 [草案附件](2026-10-21-runtime-next-loop-draft.md)。
它提供 Host/GT/RT 分工、Q/F 闸、显式帧交接与退休、安全点、统一终止和高层解耦的探索方向，但仍包含推断和待确认事项。

草案中“新旧 Application 并存直到集成”的过渡策略已由本次整体移除决定取代。
StrictTaskScope、异常边界、RuntimeException、RunResult、RHI 返回结果、设备丢失与 Vulkan 呈现释放等条目均不因留档而获得实施批准或规范正确性背书。
后续需要独立设计、核对后端约束并验证正常、失败与终止路径；仓库现有异常和共享状态规则仍然有效。
