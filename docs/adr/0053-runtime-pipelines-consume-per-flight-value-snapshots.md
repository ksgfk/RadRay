# ADR-0053 Runtime pipeline 通过 per-flight 值快照跨越线程边界

状态: 部分被 ADR-0054 的 workload/graph context 取代；资产保活与值快照继续生效；部分取代 ADR-0047 的 BindingGroupPlan 机制与 ADR-0049 的 Material 常驻 set 机制
日期: 2026-09
影响: `RenderPipeline`、`RenderSystem`、`Application::Update`、`ForwardPipeline`、`Material`、`PrimitiveSceneProxy` 与 runtime 测试

## 背景

既有 threaded runner 已通过 slot semaphore 和 flight fence 保证同一 flight 的写入、录制与
复用互斥，但此前只传递时间数据。Forward 在 render thread 仍回读 Scene、CameraComponent、
Material；game thread 同时可以修改或销毁这些对象。Scene 当前只有 proxy vectors，没有需要
稳定 handle 的持久空间索引或增量 draw cache，建立永久 render-thread scene database 会新增
同步协议，却不能消除已有的逐帧遍历。

Material 原先每个 flight 只持有一个 parameter set。同帧再次准备同一个 Material 时，若 arena
使用了另一 backing buffer，就会改写已录制 draw 引用的 set。ShaderParameterLayout 已经拥有
declaration name、真实 group 和 binding handle，另外维护 BindingGroupPlan 会重复这些事实。

## 决策

1. 保持一个 `radrayruntime`，原地把 `RenderPipeline` 简化为 game-thread `PrepareFrame` 与
   render-thread `Render`。Context 只有 Frame 和 Targets；具体 pipeline 自行持有所需服务与输入。
2. Scene/proxy/Camera/Material 保持 game-thread-only。Forward 为每个 flight 复制相机、光源、
   材质与逐 section draw 的值；render 只消费这些值。沿用 runner 既有互斥，不增加 packet、
   sequence、release acknowledgement 或另一组 semaphore。
3. `RenderSystem` 的 per-flight vector 在 game thread 保留本帧 mesh/texture owners。flight
   可复用后先清引用，再执行 AssetManager 的既有 Pump。render input 只含 raw geometry/texture
   pointers；program 由 RenderSystem 缓存保活。正常 shutdown 在 GPU idle 后按依赖顺序释放。
4. `Material::Create(program, declarationAnchor)` 选定一个 cbuffer 所在 group；Material 只保存
   CPU authoring state，并通过 `BuildRenderData` 复制参数、texture/subview、sampler、固定功能状态
   与 queue。它不拥有 flight、arena、RHI set 或 GPU 完成状态。参数身份继续遵守 ADR-0052。
5. Forward 私有 resolver 按 `ForwardView`、`ForwardMaterial`、`ForwardObject` 解析实际 group，
   验证三个 buffer dynamic、三组不同及 active texture/sampler 的组归属。无 group remap 或
   0/1/2 fallback；不兼容 program 负缓存。Material snapshot 的组还必须匹配解析出的 material 组。
6. Forward 每帧建立不可改写的 parameter sets：相同 snapshot/backing bindings 可复用，backing
   改变则新建。复用 flight 时先销毁 sets，再 reset arena；draw loop 只绑定已准备的 PSO/set/
   VB/IB 并 DrawIndexed。dynamic placement 仍通过 ADR-0051 的 declaration recipe 提供。
7. RenderSystem 统一 acquire、`initial → RenderTarget → Present` 与未写目标的 fallback clear。
   无 presentation target 时不执行 Render；Application 不再有第二条 view 录制入口。

## 取舍与边界

接受每帧复制小量 CPU 数据和重建 descriptor sets 的成本，换取明确的线程边界及资产寿命。
只有 profile 证明复制或分配成为瓶颈，或第二条实际 pipeline 出现相同需求时，才考虑持久 Scene、
共享 allocator 或跨帧 set cache。ShaderJit/artifact/program caches 继续归 RenderSystem。

MeshDrawList/RenderQueue 当前只服务 Forward；保留文件位置不代表通用 pass policy。opaque 与
transparent 由 Forward 私有函数安排。RenderGraph、MaterialTemplate/MaterialPass、Visibility、
stable handles、provider registry 与生产 compute/depth pipeline 均不属于此次改造。

`SetPipeline` 仍是装配入口，只在 runner 启动前或 GPU idle 后使用。Forward 构造时借用的 Scene
和 Camera 必须存活到最后一次 PrepareFrame；已准备 flight 不再依赖它们或 Material 的寿命。
