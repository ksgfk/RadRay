# ADR-0054 显式 workload 与单队列 RenderGraph

状态: 生效；部分取代 ADR-0053 的 Frame/Targets context、隐式窗口目标与 Forward 私有相机快照位置
日期: 2026-09
影响: RenderSystem、RenderPipeline、ForwardPipeline、WindowManager、ShaderProgram 与 RHI capabilities/barriers

> - 适用: Stage A 的 output/view/workload、graph、pool/history 与 Forward 边界决策
> - 权威: 2026-09 的设计决策记录；当前行为以 `docs/architecture/renderer-foundation.md` 为准
> - 锚点: `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/src/render_framework/render_graph.cpp`, `modules/runtime/src/render_framework/view_state.cpp`, `modules/runtime/src/forward_pipeline/forward_pipeline.cpp`

## 背景

ADR-0053 解决了游戏对象与 render thread 之间的值快照和资产寿命，但 Frame/Targets context 仍把
管线录制绑定到已取得的窗口目标；多视图、离屏 graph、depth 复用与 history 缺少共同的身份和状态规则。
仅按最后 writer 建依赖会保留被 Clear 覆盖的旧工作；按裸 render pass 指针缓存 PSO 又会把兼容的
Clear/Load 操作当作两个 pipeline。扩展必须保留既有 Direct queue/flight 同步而不重建 render world。

## 决策

1. game thread 通过 per-flight frame plan 明确提交 output/view families。output 和有历史的 view
   使用单调、不复用的身份；native acquire、未写目标 clear 和最终状态转换由 host 独占。
2. 每次 Render 创建并串行执行一张 graph。内容版本用于 culling，live-pass hazard edges 用于排序；
   texture 状态按 mip/layer，buffer 按整体。所有原生资源实现先于录制，失败后提交已录 barrier 的真实状态。
3. 复用对象以每 flight 的完整 descriptor pool 管理，不做同帧 aliasing。history 独立于 transient pool，
   以成功写入 token 提交，旧 generation 借当前 flight retire bin 延迟释放。
4. pipeline context 和 pass commands facade 不提供 raw command buffer/窗口入口。Forward 只消费值
   和 resolved views；PSO compatibility 以 attachment formats/sample count 标识。
5. capability 是 RHI 的设备事实；候选格式、相对尺寸和降级选择是 runtime policy。保留既有模块边界。

## 放弃的方案

- 继续隐式枚举全部窗口并公开 AppFrameContext：无法表达零窗口工作，也无法由 graph 保证状态收口。
- 让 hazard edge 决定存活：会错误保留 discard overwrite 的旧 producer。
- 全局 transient pool、heap aliasing、多队列：需要另一套跨 flight/queue 同步，本阶段收益不足以抵消验证成本。
- 把 history 放进 transient pool：混淆物理复用与跨帧内容有效性，无法表达 skip/失败不旋转。
- 直接返回 RHI encoder：其 GetCommandBuffer 会恢复已删除的 barrier/submit 旁路，故增加窄 facade。

## 不变量与代价

PrepareFrame、asset refs 和原有 flight 同步不变；render 不访问游戏对象或操作资产引用计数。
输出不可用时 pipeline 仍执行；未写输出由 host clear；没有并行录制和跨队列任务。
pool/history 销毁 view 前必须摘 framebuffer，且只能发生在安全点。对象池可能比 aliasing 多占显存，
graph setup/payload 每帧有 CPU allocation；用创建计数与 100/1000-pass benchmark 记录后续优化基线。
v1 的 descriptor 精确复用、全 buffer 状态和单 depth/stencil aspect domain 都是明确的保守边界。
