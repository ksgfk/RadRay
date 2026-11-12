> - 适用: 创建 runtime 环境与设备，组织材质与 Scene 绘制输入，扩展 MeshPassProcessor，使用 RDG 驱动多槽帧循环
> - 权威: 本文维护 runtime 当前接口边界与所有权；底层资源契约见 RHI 文档
> - 锚点: `modules/runtime/include/radray/runtime/`, `modules/runtime/src/`, `modules/runtime/CMakeLists.txt`, `modules/core/include/radray/sparse_set.h`

# Rendering runtime

`RadRay::runtime` 是 C++20 静态库，实际 target 为 `radrayruntime`，公共依赖 `RadRay::render`，
并通过它依赖 core。公共类型位于 `namespace radray`，RHI 类型位于 `radray::render`。
构建开关与包消费见[构建与测试](../guide/build-test.md#runtime-构建)。

当前模块包含 `gpu_env.h/.cpp` 中的 `Environment`、`gpu_device.h/.cpp` 中的 `DeviceContext`，
以及材质定义、实例参数、渲染快照，Scene 的 CPU 绘制输入数据、MeshDrawCommand、Processor 接口与命令输出 Context，
以及增量命令缓存、按 Pass 的可见/动态命令列表、多槽帧循环、几何子分配池、分页上传与最小可用 RDG（Render Dependency Graph，渲染依赖图）。

### 头文件布局

公共头位于 `modules/runtime/include/radray/runtime/`，按概念分组：一个头文件集中声明同一阶段的类型，
不为单个数据结构单独建头。声明了非内联函数的头文件有同名 `.cpp`；仅含数据或内联、模板定义的头文件不需要 `.cpp`。
包含关系自上而下单向依赖，下表从底层到上层排列：

| 头文件 | 声明 | `.cpp` |
|---|---|---|
| `gpu_env.h` | `Environment` | 有 |
| `gpu_device.h` | `DeviceContext` | 有 |
| `scene_handle.h` | `SceneHandle<T>`、`MeshBatchHandle`、`PrimitiveHandle`、`MaterialHandle` | 无 |
| `material_tag.h` | `MaterialTagId`、`MaterialTag`、`MaterialTagTable`、`MaterialTagSet` | 有 |
| `material.h` | `MaterialRenderState`、`MaterialProperties`、`MaterialShaderProgram`、`MaterialResource`、`MaterialParameterValue`、`MaterialParameter`、`Material`、`MaterialInstance`、`MaterialRenderProxy` | 有 |
| `vertex_input.h` | `VertexInputAttribute`、`VertexInputStream`、`VertexInput` | 无 |
| `mesh_draw_command.h` | `MeshPassId`、命令绑定、`MeshDrawCommand`、`PassBatchCacheSlot`、`CachedPassMeshDrawList`、`VisibleMeshDrawCommand`、`MeshPassSubmitArgs`；`ValidateVertexInput`、`ValidateMeshDrawCommand`、`SubmitMeshDrawCommands` | 有 |
| `mesh_batch.h` | `MeshSection`、`MeshBatchElement`、`MeshBatch`、`PrimitiveSceneInfo` | 无 |
| `mesh_pass_processor.h` | `MeshBatchContext`、`MeshDrawCommandSharedState`、`MeshPassDrawListContext` 及缓存/动态两个实现、`MeshPassProcessor`、`MeshPassRegistration` | 有 |
| `scene.h` | `VisibleMeshBatch`、`Scene` | 有 |
| `frame_slot.h` | `MeshPassFrameDrawList`、`FrameSlot` | 有 |
| `frame_pipeline.h` | `FramePipeline` | 有 |
| `rdg.h` | `RDGBuilder`、`CompiledRDG`、`RDGTransientResourcePool`、Texture/Buffer 句柄、Raster/Compute/Copy Builder 与 Context | 有 |
| `render_graph_pipeline.h` | `RenderGraphFramePipeline` | 有 |
| `frame_scheduler.h` | `FrameScheduler` | 有 |
| `geometry_buffer_pool.h` | `VertexBufferPool` / `IndexBufferPool`、descriptor 与只移动子分配句柄 | 有 |
| `upload_buffer.h` | `MappedUploadBuffer`、`BatchedBufferUpload` | 有 |

`ValidateVertexInput` 与命令校验共用缓冲视图检查，因此和命令校验一起放在 `mesh_draw_command.h`；
`MeshBatch` 只描述输入，与命令缓存独立；Scene 私有表维护归属、脏状态与 Pass 缓存。
Scene 管理关联与缓存同步，runtime 提供通用命令构建、校验和录制；可见性、具体 Pass 筛选、shader 参数的 GPU 布局、PSO 选择、Pass 边界和提交由调用方实现。
当前 RDG 是基于现有 RHI 的整资源、单队列实现；模块不提供完整 Renderer 或 PSO 缓存。

## 多槽帧循环

当前实现串行调用、单 graphics queue、可配置槽数的 frames in flight 循环。
每个槽保存一帧的录制资源，只有其上次提交完成才能复用；不同槽允许 CPU 录制与 GPU 执行重叠。
没有渲染线程或交换链呈现集成，槽索引与交换链图像索引相互独立。

| 文件对 | 职责 |
|---|---|
| `frame_slot.h/.cpp` | `FrameSlot` 拥有按 Pass 的 VisibleCommands/DynamicContext、帧内和退役 ObjectArena、command allocator、可复用的 command buffer 池；不保存源 Scene 指针 |
| `frame_pipeline.h/.cpp` | `FramePipeline` 定义 Pass 登记、`Extract`、`Record` 和 `Wait`；Record 负责 Pass 调度、申请命令、录制与提交 |
| `frame_scheduler.h/.cpp` | `FrameScheduler` 独占一个 Pipeline 和多个 Slot，负责选槽、调用等待、重置、阶段调用与轮转 |

`FrameScheduler::Create(device, pipeline, slotCount = 2)` 按值消费 Pipeline 的 owner，失败返回空。
槽数必须大于 0，创建后固定；`GetSlotCount()` 返回槽数，`FrameSlot::GetIndex()` 返回稳定的 `[0, slotCount)` 索引。
`FrameSlot::GetFrameId()` 返回 `uint64_t` 帧编号，在各 Scheduler 内从 0 独立递增，与循环使用的槽索引无关。
Scheduler 在 BeginFrame 中等待槽的旧帧完成并写入新编号，因此准备、Extract、Record 和对应的 Wait 看到同一个编号。
每次 Record 正常返回后递增编号，空帧也会递增；帧执行错误带诊断终止程序，不提供回滚或重试。
WaitIdle 不重置计数，槽清理后仍保留最后分配的编号。计数到达 `uint64_t` 最大值时带诊断终止，不回绕。
DeviceContext 必须晚于 Scheduler 销毁。每个 Slot 的 `GetDevice()`、`GetQueue()` 返回所属设备和队列的借用引用。
`BeginFrame() -> FrameSlot&` 开启下一槽，在场景更新和上传准备之前完成等待、allocator Reset、退役清理和帧数据清理。
开启后重复调用直接返回同一槽，不重复等待、重置 arena 或释放准备期登记的退役对象。
`GetCurrentSlot()` 仅在 BeginFrame 至 Record 返回之间返回该槽，其他时间为空。
准备过程未调用 RunFrame 时，下一次 BeginFrame 保留本轮准备；WaitIdle 则清理并取消准备，保留下一槽索引与帧编号。
`RunFrame()` 可直接调用；尚未开启槽时内部自动调用 BeginFrame。完整顺序为：

```text
BeginFrame（只在未开启时执行）：选槽 → 若有在途提交：Pipeline.Wait(slot)，清除在途标记
    → Reset command allocator → Reset 退役 arena → 清理槽内 CPU 数据 → Reset 帧内 arena → 分配当前帧编号
    → 返回当前槽，调用方准备具体管线输入、扩容本槽上传缓冲或登记退役资源
    → 使用 Scene 时：显式 SyncCachedMeshDrawCommands(pipeline.GetMeshPassRegistrations())
    → 将输入设置到具体管线
RunFrame：复用已开启槽（或先自动 BeginFrame）
    → Extract(slot)：按需收集 Pass 的缓存命令借用、动态命令和当帧覆盖项
    → Record(slot)：继续只读借用管线输入，排序可见命令、准备数据、申请 command buffer、Begin/End、自行提交
    → 标记在途，下一帧编号递增，当前槽索引 = (当前槽索引 + 1) % slotCount
    → RunFrame 返回，结束外部输入借用
```

`RunFrame` 返回 `void`，正常返回只表示 Pipeline 已完成本帧录制与提交，不保证异步工作已完成。新槽和已回收的槽不调用 Wait；
在途标记表示存在尚未由 Scheduler 回收的 Record；即使没有提交或底层已经完成，也会在复用前调用一次 Wait。
`WaitIdle()` 先等待所有在途槽，再按三个全槽循环清理：先对**全部槽**调用 `ResetCommandStorage()`，
解除 command allocator 所有 epoch 引用，然后对全部槽调用 `ResetRetiredResources()`，最后对全部槽调用 `ResetFrameData()`，清空 CPU 数据和帧内 ObjectArena。
不能逐槽完成全部清理，否则 Slot 0 的 arena 可能释放仍被 Slot 1 旧录制引用的 Buffer；
它保留当前索引，重复调用不会重复等待已回收提交。Scheduler 析构先执行 WaitIdle，
再销毁全部 Slot，最后销毁 Pipeline，确保 Pipeline 拥有的资源晚于命令存储释放。
Slot 自身不持有 fence 或完成值，Reset 和析构均不等待。
所有调用和析构遵守 [RHI 串行调用与命令存储契约](render-rhi.md#命令录制存储)，不可并发或重入。

### 帧内分配与资源释放

每个 Slot 独占一个使用默认 `ObjectArenaOptions` 的 arena，`GetArena()` 返回其借用引用。
FramePipeline 通过各阶段的 `FrameSlot&` 访问当前槽的 arena。同一个 Pipeline 服务多个在途槽，
因此帧内存储的所有权归各 Slot，回收时机由 Scheduler 控制。
调用方在 BeginFrame 之后、Pipeline 在 Extract / Record 中通过它分配临时存储、`Emplace<T>` 构造帧内对象，或登记拥有资源的 RAII 对象。
对象和存储一直有效到对应 Wait 返回，随后在槽重置时统一回收。
帧内分配不跨槽共享，CPU 写入仍须遵守各槽数据隔离要求。

转移 `unique_ptr` 所有权即可延迟释放 GPU 资源，例如已成功创建的 `unique_ptr<render::Buffer> buffer`：

```cpp
auto* owner = slot.GetArena().Emplace<unique_ptr<render::Buffer>>(std::move(buffer));
auto* borrowedBuffer = owner->get();
```

后续录制可借用 `borrowedBuffer`，资源 owner 在槽回收时析构。槽内 owner 默认只供该槽使用；
若其他槽也引用它，必须在 owner 槽再次复用前调用上述 `WaitIdle()`，或将共享 owner 交给下述 `RetireResource`。`shared_ptr` 可同样保活共享资源；
自定义清理通过 `MakeScopeGuard` 创建回调，再以 `Emplace<decltype(cleanup)>(std::move(cleanup))` 登记。
非平凡对象按构造完成的逆序析构，因此有依赖的资源应先登记被依赖者、后登记引用者。
arena 的底层分配、保留预算和 allocator 契约见 [ObjectArena 与 ArenaAllocator](core-facilities.md#objectarena-与-arenaallocator)。

回收先 Reset command allocator，解除全部旧录制的资源引用，再 Reset 退役 arena，随后清空各 Pass 的可见/动态命令，最后 Reset 帧内 arena。
帧内 arena 的清理回调执行时 CPU 记录已为空；退役回调发生在 CPU 记录清理之前，不得假定记录已为空。
两类回调执行时 Pipeline 和 DeviceContext 仍有效，回调不得重入 Slot、arena 或 Scheduler。
正常槽复用与 Slot 析构沿用这一清理顺序，arena 成员析构归还全部保留内存。正常槽复用和 WaitIdle 使用 Reset，按默认预算保留存储。
【调用方不得手动 Reset / Release 槽内 arena，不得手动销毁已登记对象；借用指针在回收后失效。】
使用 `ArenaAllocator` 的外部容器必须在槽回收前销毁，clear 不足以结束存储借用；也可将容器本体登记到 arena，
由它先销毁容器再回收存储。按 Pass 列表自身继续使用现有容器分配方式。

`slot.RetireResource(std::move(owner))` 接收 RAII owner，存入该 Slot 私有的 `_retiredArena`。
仅允许在当前槽 BeginFrame 返回后至 Record 返回前调用；Wait、清理回调和未开启帧时通过保存的槽引用登记均违反调用前置条件。
`RADRAY_IS_DEBUG` 下用槽内回调活动标记检查这一窗口，违规时带诊断终止；Release 不保存该标记，也不执行窗口检查。
准备、Extract / Record 共用同一登记窗口，无需借用 Scene。重复 BeginFrame 不缩短已登记资源的寿命。
调用前必须停止对旧资源的新使用；已有录制可以继续提交和执行。参数组等依赖对象应与底层 Buffer/布局组合为具有正确析构顺序的一个 owner。
登记时当前槽已完成本轮清理；下一次复用该槽前，固定轮转已使其他槽在登记后重置过 allocator，
再等待并重置当前槽 allocator，即可安全释放其全部退役 owner。没有批次、槽完成掩码或额外调度状态。
退役 arena 使用 1 KiB 块、4 KiB 动态存储保留上限，空 arena 不分配堆内存；同一队列内按登记完成的逆序析构，不承诺跨槽析构顺序。
`WaitIdle()` 在全部 allocator 重置后清空所有退役队列，因此不需要先轮转完未使用槽。Slot 析构最终释放 arena 的保留存储。
未开启帧时产生的旧 owner 可在下一次 BeginFrame 返回后直接转交，或在 `WaitIdle()` 后释放。
这只覆盖本 Scheduler 的单队列槽，不等待外部录制或其他队列。

### 命令录制与 Pipeline 契约

Pipeline 在 `Record(slot)` 中通过 `slot.AcquireCommandBuffer()` 申请命令缓冲。
Slot 初始不创建 command buffer，每次申请返回本轮尚未借出的一个 buffer；缓存不足时创建，创建失败返回空。
同一轮内每次申请返回不同对象，所有权始终留在 Slot。槽重置时 allocator 统一 Reset，借用结束，
下一轮从池首复用；池保留历史最大数量，不因本轮使用较少而销毁多余对象。
Pipeline 不得跨槽复用保存的借用，每条命令在本轮最多 Begin/End 一次、提交一次。
同槽共享一个 allocator，为满足 D3D12 契约，同一时间只允许一条命令处于 Begin/End 区间。

Record 自行决定申请数量（可以为 0）、Begin/End、提交批次和时机，通过 `slot.GetQueue()` 提交。
Scheduler 不接收或操作单条 command buffer；BeginFrame 仅开启准备窗口，具体准备和提交由调用方及 Pipeline 执行。
Record 正常返回前，Pipeline 必须保存足够的按槽完成状态，供 `Wait(slot)` 等待本槽全部提交。
`Wait` 是同步 `void` 接口，
返回即保证全部槽内工作完成、录制存储和帧数据可重置或销毁；不允许将超时当作完成返回。
具体实现可以使用 fence、事件或其他机制；无提交或同步完成的 Record 可搭配空操作 Wait。

Extract / Record 均为 `void noexcept`，只允许正常完成或在发现错误的位置带诊断终止程序。
无效输入、资源准备或录制失败由 Pipeline 使用 `RADRAY_ABORT` 处理，不依赖仅在 Debug 生效的断言；
Scheduler 没有失败回滚、部分提交恢复或重试分支，不捕获异常恢复执行。
Record 正常返回前必须关闭全部 encoder 并 End 全部已开启的 command buffer；Extract 不得提交异步工作。
没有可见物体或没有绘制命令是正常空帧，不属于失败。

Pipeline 按槽索引维护完成状态及可复用的上传缓冲、常量等可写帧资源；可以预分配或首次使用时创建。
仅供当前帧使用的资源可交由槽内 arena 持有并释放。
这些资源必须按槽独立分配或使用互不重叠的存储区域，在对应 Wait 返回前不能覆盖。
同一个 Pipeline 可以被多个槽依次调用，但不能共用一块每帧覆盖的上传数据。

帧入口仅有 `RunFrame()`；FramePipeline 及 RDG 扩展钩子不接收 Scene，帧管线头文件也不包含 scene.h。
具体管线持有自己的内容源；使用 Scene 的调用方在提交前设置管线输入，并显式调用
`scene.SyncCachedMeshDrawCommands(pipeline.GetMeshPassRegistrations())`。调度器不自动同步缓存。
每次同步必须传入当前 registrations，不能仅以 `HasDirtyBatches()` 决定是否调用；缺失 pass cache 或 processor/registration/sort mode 变化可以发生在没有脏 batch 的情况下。
`GetMeshPassRegistrations()` 默认返回空 span，纯 RDG 管线可保留默认值。FrameSlot 的 mesh 绘制列表与排序流程继续可用。
外部输入在 RunFrame 返回前必须有效且只读，Wait、槽回收、arena 清理及析构不得访问这些借用输入。
多个 Scene 各自拥有独立容器及缓存；管线可以在帧间切换输入。FrameSlot 不保存 Scene 指针，也不提供 GetScene。
调用方前置条件不由运行时锁或跨线程变更队列维护。

缓存命令的 CPU 数据只需覆盖本帧 Record；命令编码完成后，帧间可以删除 Batch、重建缓存或销毁 Scene，无需等到 Wait。
Wait、槽清理以及 arena 对象的析构/清理回调不得解引用留下的 Scene、Batch 或缓存命令借用。
需要在 Wait 使用的 CPU 数据应保存到槽内 arena 或 Pipeline 的对应槽状态。
底层 GPU 资源寿命独立于 CPU 命令：外部资源必须保持有效至最后一次引用它的 command allocator 重置，或通过 `RetireResource` 保活。

`slot.GetPassDrawList(pass)` 按紧凑 `MeshPassId::Value` 取得槽内列表；不能跨后续列表扩容保存其引用。
`MeshPassFrameDrawList` 包含 `VisibleCommands` 和自动绑定到该列表的 `DynamicContext`，不拥有缓存命令副本。
列表搬移时会重新绑定 DynamicContext，避免 vector 扩容后继续向旧列表写入。
Extract 按本帧激活的 Pass 调用 `Scene::GatherVisibleMeshDrawCommands(..., drawList)` 或 `GatherVisiblePrimitives(..., drawList)`，
保存命中缓存命令的只读指针；也可调用 `processor.AddMeshBatch(context, drawList.DynamicContext)` 生成当帧命令。
未请求的 Pass 不进行当帧提取；持久缓存同步仍独立处理全部登记 Pass 的脏 Batch。
Record 中的 `SortVisibleCommands()` 与非 const 的 `Submit(encoder, submitArgs)` 都先调用 DynamicContext 的 `FinalizeVisibleCommands()`，
完成动态命令指针填充并冻结命令存储；两者执行前必须完成全部动态命令生成。
排序按 SortKey 稳定升序进行；Submit 统一使用可见项的命令指针，并转发 Pass 级参数组与实例流。
不需要这些绑定时可省略 `submitArgs`；提交入口不再接收缓存列表或动态命令数组。
槽清理只清空可见列表、动态命令与动态绑定池并保留容量，不解引用缓存指针；没有缓存命令复制和快照析构。

当前仍串行执行 Extract / Record，没有独立渲染线程或额外的业务 RenderPass 抽象。

## 几何子分配池与持久映射上传

`geometry_buffer_pool.h/.cpp` 提供对称的 VertexBufferPool 与 IndexBufferPool。Create 借用设备并复制布局、属性和调试名前缀；
Allocate 才按需创建 Device 页，页在池寿命内保留，空闲区间由 [BestFitAllocator](core-facilities.md#bestfitallocator) 管理。
池与设备必须晚于所有子分配句柄销毁；释放区间前须完成 GPU 使用并重置相关 command allocator，可将旧 mesh owner 交给 FrameSlot::RetireResource。
分配零元素或创建页失败返回空值；超过默认页容量的请求创建可容纳整段的大页，不跨页切分一次分配。
GetPageCount / GetFreeBytes 分别返回保留页数和可重用空闲字节。所有操作串行调用。

VertexBufferPoolDescriptor 指定 VertexStride、VertexStreamBinding、PageVertexCapacity（默认 150,000）、AdditionalUsage 和布局/属性。
布局必须恰有一个 Vertex 流，binding/stride 与描述符一致，另可有一个 Instance 流；所有布局和属性通过 RHI 顶点输入校验。
顶点页字节数不超过 uint32 上限；Usage 至少含 Vertex | CopyDestination。
每页的 VertexInput 地址稳定，同页句柄的 GetVertexInput 返回同一对象，仅绑定页顶点流，实例流由 MeshPassSubmitArgs::InstanceStream 延迟注入。
VertexBufferSubAllocation 提供 GetBuffer、GetVertexStride、GetFirstVertex、GetVertexCount、GetByteOffset、GetByteSize。

IndexBufferPoolDescriptor 指定 IndexStride（2 或 4，默认 4）、PageIndexCapacity（默认 300,000）与 AdditionalUsage。
索引页 Usage 至少含 Index | CopyDestination。IndexBufferSubAllocation 提供对应索引范围和字节范围访问器；
GetIndexBufferView 返回 Offset=0 的整页视图，应配合 GetFirstIndex 使用，不能再叠加 GetByteOffset。
构造 MeshBatchElement 时，可设 IndexBufferOffset=0、FirstIndex=GetFirstIndex()+局部索引起点，并显式限定范围不超过该子分配。
两类句柄仅可移动，空句柄不释放区间；读取资源和字节视图要求句柄非空。

两池的 AddPassReadAccess 遍历保留页，以 `(Common, Common)` 导入 RDG 并声明读取，默认状态分别为 Vertex 和 Index。
可传 Vertex | ShaderRead 等合法只读组合（须通过 AdditionalUsage 增加相应用途）；多次导入与上传器共享同一物理句柄。
依赖以整页表达，不需要逐 Primitive 收集几何资源；未使用的页也会参加存活 Pass 的整页状态转换。

`upload_buffer.h/.cpp` 的 MappedUploadBuffer::Create(device, capacity, usage=CopySource) 创建 Upload 内存，
自动增加 MapWrite 和 PersistentMap 并持久映射。Get / GetData / GetCapacity 返回缓冲、映射地址和容量；Flush 验证范围并刷新写入。
Reserve 在容量不足时按 2 的幂创建新缓冲，再释放旧映射，不保留旧内容；失败保留旧对象，溢出请求返回 false。
调用者须先等待旧 GPU 使用并重置所有引用该资源的 allocator，且使用同一设备；槽独占的实例/常量流可在 BeginFrame 之后安全扩容。

BatchedBufferUpload 按槽独立创建，默认页大小 1 MiB。Acquire(dst, offset, bytes, alignment=16) 直接返回持久映射页内的写指针，
Stage 使用同一分配路径复制 CPU 字节。目标须为 Device CopyDestination Buffer，范围非空且合法，对齐须为非零 2 的幂（对齐页内源偏移）。
现有页不足时追加 `max(defaultPageSize, bit_ceil(bytes))` 的新页，已用页不重分配、不取消映射、不复制旧映射内容。
所有 Acquire 返回的指针跨后续 Acquire 保持有效；必须在 CommitToGraph 之前完成写入，不能在提交后继续 Acquire 或 Stage。
Reset 仅在槽已完成 GPU 使用并重置录制存储后调用，清空区间和页游标，保留所有页供后续使用。

CommitToGraph 刷新活动页，以 `(HostWrite, HostWrite)` 导入源页，以 `(Common, Common)` 导入去重目标并 MarkOutput，
将全部复制记录自持有地捕获到一个 Copy Pass。空批次直接成功；每次 Reset 后只能 Commit 一次。
目标在仅上传帧也恢复 Common，因此可在后续帧直接由两池 AddPassReadAccess 声明读取。
GetCapacity 返回全部保留页容量，GetStagedByteSize 返回本轮有效数据字节，不包括对齐空隙。

## RDG 与 RenderGraphFramePipeline

`rdg.h/.cpp` 在 runtime 内实现纯 CPU 构图/裁剪与 Record 期执行，不改动 RHI。
`RenderGraphFramePipeline` 是可派生的模板方法基类；构造参数为 `DeviceContext&` 与正的槽数，必须与 Scheduler 的设备、槽数一致。
它将 `Extract` / `Record` 声明为 `final`，每槽保存独立的瞬态资源池和本帧编译图。子类实现以下钩子，并继续实现 `Wait` 和按需覆盖 `GetMeshPassRegistrations`：

| 钩子 | 阶段与职责 |
|---|---|
| `ExtractFrameData(slot)` | 读取具体管线输入，提取候选可见集、变换及待上传的 CPU 字节；不做逐 Pass 命令收集或 GPU 上传 |
| `SetupGraph(graph, slot)` | 可只读访问管线输入 并按需声明资源、上传、Pass 与输出，不创建 GPU 对象或 Map |
| `ExtractMeshPass(pass, slot, drawList)` | Compile 成功后，仅按首次声明顺序对去重的存活 MeshPassId 调用，收集缓存命令借用和动态命令 |
| `SubmitFrame(slot, commandBuffer)` | Record 已排序存活列表、执行图并 End 命令缓冲后调用；提交到本槽队列并保存供 Wait 使用的完成状态 |

基类的 Record 顺序为：存活 MeshPass 排序 → 本槽池 `BeginFrame` → 申请一条 command buffer 并 Begin → `CompiledRDG::Execute` → End → `SubmitFrame`。
`ExtractMeshPass` 有带终止诊断的默认实现；纯 RDG 管线无须覆盖，声明了存活 mesh pass 却未实现提取时立即终止并报告 pass ID。
构图、编译或执行失败由基类带诊断终止，没有部分提交或重试。
执行回调可借用本次 RunFrame 的管线输入，并在 Record 执行期间只读访问它。**不得捕获 Setup 局部变量的引用或其他槽的可写帧数据。**
回调对象可能在槽回收时才析构；析构不得访问已经结束的外部输入借用。

### 构图、访问与裁剪

`RDGBuilder(ObjectArena&)` 将图记录与回调对象登记到 arena；上传字节立即复制到 arena，Pass 名称也由图自持有。
Setup 回调立即执行，Execute 回调直到 Record 才执行，支持持有 move-only 数据的闭包。Builder 不拥有 arena；
`Compile()` 单次调用，冻结声明并返回 arena 中的 `CompiledRDG*`，失败记录诊断并返回空值。冻结后继续改图违反前置条件。
图、回调、句柄及其 CPU 数据在该槽回收后失效；不能手动清理槽内 arena。

资源分为导入的 `render::Texture*` / `Buffer*` 与按完整描述符声明的瞬态资源。导入需要真实 `initialState`，可选 `finalState`；
同一物理对象重复导入会合并句柄，初态和显式终态冲突时编译失败。GPU 对象的 owner 仍由调用方持有，必须活到引用它的命令存储重置；
外部附件 View 可通过 `WriteColor` / `SetDepthStencil` 的 `explicitView` 传入，目标必须与导入资源一致。
状态与依赖按整个资源追踪，Texture barrier 使用 `SubresourceRange::AllSub()`，Pass 访问接口不接受子资源范围。

`AddRasterPass`、`AddComputePass`、`AddCopyPass` 分别提供 Encoder 或 CommandBuffer Context。
Context 只允许 Resolve 当前 Pass 已声明的资源；`ResolveView` 在状态与 View 用途不匹配时返回空。
Raster 通过 `WriteColor` / `SetDepthStencil` 声明附件，颜色槽从 0 连续排列，至少有一个附件，各附件有效尺寸、层数和采样数一致。
自动 View 使用首个 mip；显式 View 可选择附件子资源，但状态仍按整个资源同步。
只读深度/模板 aspect 必须 Load / Store；丢弃 Store 使用 `StoreAction::Discard`。
Buffer 的导入边界、读取状态与瞬态池 LastState 使用 `render::BufferStates`。只读状态组合逐位要求相应 Usage：
Raster 允许 `Vertex | Index | CBuffer | ShaderRead | Indirect` 的非空子集，Compute 允许 `CBuffer | ShaderRead | Indirect` 的非空子集；Copy 读取使用 CopySource。
同一 Pass 多次 ReadBuffer 的纯只读状态自动按位合并。单独 UnorderedAccess 仍支持只读、写和读写，不能与其他位混合；
Undefined、Common、CopyDestination、HostRead、HostWrite 也只允许单态。导入边界可使用包括 CopySource 的纯只读组合。
Texture 的状态冲突、Buffer 不兼容的读写/混合状态、无效句柄、不匹配的 Usage / Pass 类型、未初始化读取及无 Writer 的瞬态输出都会使编译失败。

编译按声明顺序维护每个资源当前版本的最后 Writer 与 Readers，依赖只指向较早声明的 Pass：

- 数据依赖用于保活：Read 依赖先前 Writer；ReadWrite、普通 Write、分段 Upload 和 Load 附件写保留旧 Writer。
- 顺序依赖只约束同时存活的节点：写入必须在当前版本的所有存活 Reader 之后；Clear / DontCare 全覆盖附件写还必须在存活的旧 Writer 之后，但不因这些边保活旧 Writer 或 Reader。任一深度/模板 aspect Load 时仍保留旧 Writer 的数据依赖。
- 两类依赖分别去重，同一前驱的顺序边不能抑制数据边；构造存活子图的拓扑入度时再合并去重。
- `MarkOutput(resource)` 直接标记资源存活，并从最后 Writer 保活；`NeverCull()` 直接保活该 Pass。导入资源的写入和终态本身不自动保活 Pass。
- 从这些根只反向遍历数据依赖，过滤死 Pass、只被死 Pass 使用的瞬态资源和上传。未被任何 Pass 访问的导入 Output 仍执行终态转换。
- `GetAliveMeshPasses()` 和 `ExtractMeshPass` 始终按存活 Raster Pass 的原始声明顺序去重、提取，不受 GPU 调度顺序影响。

### 确定性重排与合并

Compile 对存活子图执行 Kahn 拓扑调度，同时遵守数据依赖与顺序依赖。就绪候选优先延续当前 Raster / Compute 合并组；
无法延续时关闭该组，优先选 Upload / Copy，再选其他 Pass；同优先级始终取原始声明下标最小者。
因此无依赖 Pass 的 GPU 执行顺序可能变化，`NeverCull` 仅保活，不建立顺序；需要先后关系时必须声明资源依赖。
Upload / Copy 各自独立成组，Raster / Compute 按以下规则合并：

- Raster 的颜色槽、附件句柄、显式 View 指针和深度状态必须相同；上一子 Pass 的附件 Store 必须为 Store，下一子 Pass 的 Load 必须为 Load。深度/模板只检查纹理实际具有的 aspect。
- 组内共享附件的同状态访问按绘制顺序保序，省略中间附件屏障。其他资源若已被组内任一前序子 Pass 访问，两次访问必须都是相同状态的只读，否则切组。
- Compute 使用相同的非附件规则：共享只读可以合并，写后读、读后写、连续写或状态变化必须切组；检查覆盖整组，不能只比较最后一个子 Pass。
- 后续子 Pass 首次使用的资源可以把转换屏障前移到组入口；其生产者已在组外完成，组内前序子 Pass 不访问它。每组全部首次访问的屏障批量下发一次，然后开启 Encoder，Encoder 内不下发屏障。
- Raster 使用首 Pass 的 Load / Clear 和末 Pass 的 Store，`AllowUavWrites` 为全组 UAV 写访问的逻辑或；每个逻辑子 Pass 回调前重置 `SetStencilReference(0)`。
- Raster / Compute 每组只开关一次 Encoder，但每个逻辑子 Pass 保留独立 debug group，并且 Context 只允许解析当前子 Pass 声明的资源。回调失败时立即停止后续回调并关闭 Encoder。

被裁剪的 MeshPass 不触发当帧提取或排序；其独占资源不向池借出，其上传不执行 Map 或 Copy。
使用 Scene 时，调用方在 RunFrame 前显式同步当前全部 registrations，不受当帧图裁剪影响。

### 上传与资源状态

`UploadBuffer(dst, offset, bytes)` 支持带偏移的分段更新。Device 目标必须声明 `CopyDestination`，执行时从本槽池借出独立的 Upload Staging Buffer，
Map / 复制 / Flush / Unmap 后录制 `CopyBufferToBuffer`。每次 Staging 借用都保留到本槽完成，后续 GPU 消费由图自动转换状态。
Upload 目标必须声明 `MapWrite`，执行时直接写目标内存。ReadBack 目标、越界范围与不支持的用途会被拒绝；空字节上传不生成节点。
**同一 Upload 目标的所有上传必须在首次 GPU 访问前声明，且区间不重叠**，包括最后被裁剪的声明也参加校验。
CPU Map 在提交前发生，不能表达同一地址在命令流中的多个版本；中途更新应使用 Device 缓冲或不同的 Upload 缓冲。

帧内按需数据只上传，不标记 Output，最后消费者被裁剪时上传也被裁剪。
通过 UploadBuffer 初始化持久资产时应同时 `MarkOutput` 并设置正确终态（例如 Vertex / Index，或几何池约定的 Common），即使本帧不绘制该资产也能完成初始化。
Scene 只要求几何缓冲对象及描述符合法，允许先注册尚未写入的 Device Buffer，再交给图执行上传。

执行按调度后的存活组推导 `(currentState, pendingWrite)`，被裁剪的 Pass 不会在状态链中留下缺口。
初态来自导入声明或池条目的真实物理终态；写类型初态保守地视为有待同步写入，槽复用后的首次同状态写也会同步。
组入口同状态连续只读省略 barrier；连续 UAV 访问中前次或本次为写时发 `BarrierUavDescriptor`；
其他状态变化或前次存在待同步写入时发 Buffer / Texture barrier，包括跨组的 `DepthWrite → DepthWrite` 和 `RenderTarget → RenderTarget`。
组内共享附件只累积写标记，第一次离开该组时仍需同步；同状态共享只读不重复发屏障。
图末对全部存活导入资源的 `finalState` 收尾屏障再批量下发一次，并将存活瞬态资源的实际终态提交回池。
Device Upload 内部借用的 Staging Buffer 仍单独处理 Map 前与 Copy 前的状态转换。
`CompiledRDG::Execute` 只允许一次，调用方提供已 Begin 的命令缓冲，负责 End / Submit；执行失败返回 false，仍关闭已开启的 encoder，帧不可重试或提交。

`ImportTexture` / `ImportBuffer` 的可选 `RDGExternalOwnership{Acquire, Release}` 声明外部所有权边界。
重复导入同一资源时所有权声明必须一致。执行按资源维护待 Acquire 与待 Release 标记：
首次组访问合并状态转换并发出 Acquire；没有执行组但被 MarkOutput 保活时，图末也消费 Acquire。
即使 Before 与 After 相同，所有权屏障仍发出；被裁剪的资源不转移所有权。
图末完成终态转换后，在独立 barrier 调用中 Release，未指定终态则保持当前状态。
这些声明只生成 RHI ExternalTransfer 屏障；外部队列等待/信号、跨进程同步与资源支持性仍由调用方负责。

### 每槽瞬态资源池

`RDGTransientResourcePool` 独占 Texture、Buffer 及其 View owner。进入槽前必须已完成 Wait 和 allocator Reset，随后 `BeginFrame()` 仅重置借出标记，
按描述符全部字段匹配复用，保留真实 `LastState`；同一执行内相同描述符的多次资源申请仍获得不同物理对象，不做别名复用。
新 Texture / Device Buffer 初态为 Undefined，新 Upload Buffer 为 HostWrite。Execute 结束时记录真实终态，不能在 Fence 完成后假定纹理自动回到 Undefined。
池保留历史峰值对象，`Clear()` / 析构前所有引用它们的命令存储必须重置，View 先于资源销毁。
瞬态 View 缓存在池内；导入资源的自动 View 由本帧 arena 持有，避免池把外部资源借用延长到后续帧。
直接使用池的 `AcquireTextureView` 时，目标资源必须活到该缓存 View 释放。

当前不提供子资源状态拆分、异步队列、资源内存别名、跨槽共享池、显式资源版本节点或自动 PSO/参数组构建。

教学样例的 `MeshDrawingFramePipeline` 使用同一流程，默认双槽连续六帧。
Pipeline 按槽持有实例缓冲、两份 Pass UBO/参数组、读回缓冲和独立 fence，颜色/深度附件由基类的每槽瞬态池持有；几何、shader、布局与 PSO 由外部共享。
Extract 更新当前槽的数据并固化各激活 Pass 的命令；Record 排序槽内列表、申请命令缓冲、完成四个 Pass 和读回拷贝，提交时递增该槽的 fence 值。
Wait 只等待该槽的 fence；主循环不逐帧等待整个队列，结束时 `WaitIdle()` 等待并重置全部槽，再保存最后一帧的图片。
Frame 0 后更新绿色材质，调用方以当前 registrations 显式同步增量命令，设置管线输入后调用 Frame 1 的 `RunFrame()`；已有在途帧的参数与附件保持独立。

## Scene 绘制输入

| 类型 | 头文件 | 当前内容 |
|---|---|---|
| `Scene` | `scene.h` | 私有 Primitive/Batch/Material SparseSet、关联账本、材质反向索引与 Pass 缓存 |
| `PrimitiveSceneInfo` | `mesh_batch.h` | 本地到世界变换、`Aabb LocalBounds`、MeshBatch 句柄列表 |
| `MeshBatch` | `mesh_batch.h` | 一个材质句柄、一个顶点输入引用、拓扑与内联 Elements |
| `MeshBatchElement` | `mesh_batch.h` | 借用的索引缓冲、索引范围和默认实例范围 |
| `VertexInput` | `vertex_input.h` | 顶点布局、属性和借用的顶点缓冲流 |
| `MeshSection` | `mesh_batch.h` | 几何资源的 submesh 分段：材质槽和索引范围；Scene 不存储它 |
| `MaterialRenderProxy` | `material.h` | 借用 MaterialResource，拥有已解析的 CPU 参数快照；见下文材质核心 |

这些类型是渲染线程持有的 CPU 绘制记录，遵守上述 RunFrame 借用边界。Scene 不存储用户侧网格和材质资产。
Scene 直接持有 `SparseSet<PrimitiveSceneInfo>`、`SparseSet<MeshBatch>` 和 `SparseSet<MaterialRenderProxy>`，
存储与账本均为私有；只通过受控方法修改，通过 const 访问器读取。它不创建或依赖 GPU Device。

### 关联与存储

- `PrimitiveSceneInfo::Meshes` 是 Scene 管理的 Batch 句柄列表，每个 Batch 属于一个 Primitive。
  `AddPrimitive` 只接受空的 Meshes；通过 `AddMeshBatch(primitive, batch)` 建立关联。
- `MeshBatch::Material` 在所属 Scene 解析；全部 Elements 共用材质、顶点输入和拓扑。
- `MeshBatch::Elements` 使用 `InlineVector<MeshBatchElement, 8>` 按值独占存储，前八项内联，最多 32 项。
  输入复制后再改原描述符不会影响 Scene；更新必须调用 `UpdateMeshBatch`。
- `VertexInput` 仍借用外部对象；必须保持地址稳定。修改 Streams 后调用 `MarkBatchDirty`，
  Debug 构建命令时按该 Processor 的 `AllowDeferredInstanceStream` 重新验证顶点布局与缓冲范围；Release 由调用方保证输入有效。
- 归属句柄、脏位、Primitive/Material 引用位置分别保存在 Scene 私有数组；公开 Batch 不含缓存或内部账本。

`GetPrimitive`、`GetMeshBatch`、`GetMaterial`、`GetPassCache` 返回可空 const 指针；
`GetCachedDrawList` 是 GetPassCache 的同义入口；未注册或撤销的 Pass 返回空。修改必须通过 `Add/Update/RemoveMaterial`、
`Add/RemovePrimitive`、`UpdatePrimitiveTransform`、`UpdatePrimitiveBounds`、`Add/Update/RemoveMeshBatch`。
变换和 bounds 更新不标脏；依赖其他外部策略的 Processor 由调用方显式标脏受影响 Batch。
`GetOwningPrimitive` 与 `IsBatchDirty` 查询当前关联和脏状态。

Batch 入库验证关联、顶点输入、全部非零索引/实例 Element 的缓冲范围与对齐；失败不改变现有记录。
空 Batch 或零索引/零实例 Element 合法，不要求几何缓冲。Scene 同步仅对原入库 Batch 复用 Element 校验；
Debug 下 Processor 若用另一份 Batch 描述构建，则执行完整 Element 校验，并重新校验借用 VertexInput 和 Processor 提供的状态。
Release 构建命令时不执行这些校验，调用方须保证借用输入与共享状态满足前置条件。

材质反向引用列表由 Add/Update/RemoveMeshBatch 同步维护，`RemoveMaterial` O(1) 判空并拒绝删除仍被引用的材质，
`MarkMaterialDirty` 只遍历引用它的 Batch。删除 Batch 时通过引用位置交换末项，移除关联与各 Pass 命令，
清除脏位而不扫描脏队列；同步时按存活代次过滤旧队列项。`HasDirtyBatches` 反映存活脏 Batch，
`GetDirtyBatches` 返回队列视图，可能含等待同步过滤的已删除句柄。删除 Primitive 级联删除其 Batch。

Scene 公共接口与关联数据使用 `scene_handle.h` 中的强类型句柄：

| 元素 | 句柄类型 |
|---|---|
| `MeshBatch` | `MeshBatchHandle = SceneHandle<MeshBatch>` |
| `PrimitiveSceneInfo` | `PrimitiveHandle = SceneHandle<PrimitiveSceneInfo>` |
| `MaterialRenderProxy` | `MaterialHandle = SceneHandle<MaterialRenderProxy>` |

三种句柄是不同的模板实例，不能相互传参、赋值或比较。每个实例仅保存一个 `SparseSetHandle`，
底层 `SparseSet` 保持不变；原始句柄的包装与解包仅由 Scene 内部执行，外部不能用原始句柄构造它们。
`MeshBatch::Material`、`PrimitiveSceneInfo::Meshes`、`MeshBatchContext`、可见 Batch 和 Scene 私有关联、脏队列
都使用对应类型。`GatherVisiblePrimitives` 接收 `span<const PrimitiveHandle>`，`GetDirtyBatches` 返回 `span<const MeshBatchHandle>`。
默认构造得到无效句柄；`IsValid()` 只检查索引是否非空，不代表记录仍存活。删除与槽位复用仍通过底层代次校验，
失效句柄的查询返回空值。`GetIndex()` 返回稀疏槽索引，`GetGeneration()` 返回代次；跨容器的相同索引不代表相同对象。
句柄不保存 Scene 身份，同类型句柄仍只可在其所属 Scene 内使用。
扩容与删除可能移动记录，不能把指针或稠密下标作为长期身份；详见 [Core / SparseSet](core-facilities.md#sparseset)。
`Scene::Reserve(primitives, batches, materials)` 按几何增长预留对象、关联账本、脏队列、各 Pass 命令与绑定池数组容量；
后续注册的 Pass 也使用已声明的 Batch 预算。绑定池按每个 Batch 一个不同状态的上界预留数组，
哈希节点和超出内联容量的元素仍按实际内容分配。超过预算时容器继续按几何增长策略扩容。
所有操作只管理 CPU 记录，不拥有 GPU 资源。

### 几何输入与 submesh

`MeshSection` 表示某个几何资源或 LOD 中的一段索引范围，`MaterialSlot` 是该资源的材质槽编号，
不是 Scene 材质句柄。上层用槽位查出材质句柄，填入 Batch 的 `Material`，
再将 `FirstIndex`、`IndexCount`、`VertexOffset` 填入 Element。Scene 不要求保留原始 Section。
普通双材质网格可生成两个各含一个 Element 的 Batch；Section 与 Batch 不建立强制一一对应关系。

当前表达直接索引实例绘制：`IndexCount` 是索引数量，三角形列表的一个三角形对应三个索引。
`IndexBufferOffset` 是字节偏移，`IndexStride` 为 2 或 4；`FirstIndex` 相对此偏移，单位是索引。
`VertexOffset` 是有符号的基础顶点偏移。每个 Batch 最多包含 32 个 Element 条目（包含空条目）；超过上限的输入被拒绝。Element 与可见命令的 `FirstInstance` 默认为 0、`InstanceCount` 默认为 1；
可见命令可覆盖该实例范围。逐实例数据由应用通过实例流提供；非索引绘制、间接绘制和 LOD 选择尚未实现。

`VertexInput` 表达普通顶点与实例缓冲输入，不实现 shader permutation、蒙皮或自定义取数机制。
`Layouts` 使用 RHI 的 `VertexBufferLayout`，通过 Binding 对应 `Streams`；
`Attributes` 使用拥有 `Semantic` 字符串的 `VertexInputAttribute`，避免长期保存 RHI `VertexAttribute` 的临时 string_view。
属性的 `BufferBinding` 必须对应布局和已绑定的流，Offset/Size 使用字节单位。
`ValidateVertexInput(input, true)` 允许唯一的 Instance 布局暂缺流；存在缺失流且 Instance 布局不止一个时拒绝输入，
避免单个 Pass 级实例流槽位的歧义。所有流均已绑定时仍可使用多个 Instance 布局。
Processor 开启 `AllowDeferredInstanceStream` 后，若恰有一个 Instance 布局，helper 将其 Binding 记录为
`MeshVertexBufferBindings::InstanceStreamBinding`。提交时通过 `MeshPassSubmitArgs::InstanceStream` 补齐或覆盖该槽位；
已有默认流时可以不传，缺失默认流时必须传入。校验函数及 Debug 提交拒绝未声明槽位或传错 Binding；
Release 提交要求调用方遵守相同契约，不允许覆盖普通 Vertex 流。

所有 GPU Buffer 引用均为借用，不因记录销毁而释放。外部资源所有者负责保持 Buffer 有效，
直到 CPU 不再使用相关描述、GPU 已完成且所有引用资源的 command allocator 已重置。
默认构造只得到未填充记录：材质句柄无效、VertexInput 和 Buffer 引用为空、索引数量为零、Primitive 包围盒为空。
用于实际绘制前，调用方必须补全引用、有效布局、缓冲范围及包围盒。
Scene 入口在所有配置下检查关联与几何；Processor helper 与提交预检仅在 Debug 校验，Release 构建与提交信任调用方提供合法输入。
实际 shader/PSO 与输入的兼容性由应用和 RHI 校验层负责。

`PrimitiveSceneInfo::LocalBounds` 使用 core 的 `Aabb`，端点位于物体本地空间；
可用 `LocalBounds.Transform(LocalToWorld)` 得到世界空间 AABB，Scene 不自动维护其副本。
空盒、包含/相交和扩展操作的契约见 [Core 数学](core-facilities.md#数学)。

例如，已由外部准备好 `VertexInput cubeVertices` 和 `render::Buffer& cubeIndices` 后，
可用下面的数据组织一个顶面金属、其余五面木头的立方体。假定索引缓冲按木头 30 个索引、金属 6 个索引排列，
索引宽度为 4 字节，立方体本地坐标范围为 [-0.5, 0.5]。木头和金属实例由调用方准备：

```cpp
#include <radray/runtime/scene.h>
#include <radray/runtime/mesh_batch.h>

// scene must stop referencing cubeVertices and cubeIndices before they are destroyed.
void AddCube(Scene& scene, const VertexInput& cubeVertices, render::Buffer& cubeIndices,
             const MaterialInstance& wood, const MaterialInstance& metal) {
    MaterialRenderProxy woodProxy, metalProxy;
    woodProxy.Update(wood);
    metalProxy.Update(metal);
    const array<MaterialHandle, 2> materials{
        scene.AddMaterial(std::move(woodProxy)),
        scene.AddMaterial(std::move(metalProxy))
    };
    const array<MeshSection, 2> sections{
        MeshSection{.MaterialSlot = 0, .FirstIndex = 0, .IndexCount = 30},
        MeshSection{.MaterialSlot = 1, .FirstIndex = 30, .IndexCount = 6}
    };

    PrimitiveSceneInfo primitive;
    primitive.LocalBounds = Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    const auto primitiveHandle = scene.AddPrimitive(std::move(primitive));
    for (const auto& section : sections) {
        MeshBatch batch;
        batch.Material = materials[section.MaterialSlot];
        batch.VertexInput = &cubeVertices;
        batch.Elements.push_back(MeshBatchElement{
            .IndexBuffer = &cubeIndices,
            .FirstIndex = section.FirstIndex,
            .IndexCount = section.IndexCount,
            .VertexOffset = section.VertexOffset});
        const auto batchHandle = scene.AddMeshBatch(primitiveHandle, std::move(batch));
        if (!batchHandle.IsValid()) return;
    }
}
```

示例在 `namespace radray` 内使用。Scene 接口自动维护关联与脏标记；材质/Pass 筛选策略、可见性和资源 owner 的退役仍由调用方组织。

## 材质核心

材质描述表面使用的 shader、固定性质和可变参数；具体 Pass 结合这些信息、几何与本次渲染状态决定如何绘制。
runtime 当前保留四个角色，均声明在 `material.h`，实现在 `material.cpp`：

| 类型 | 职责 |
|---|---|
| `MaterialResource` | 材质性质，以及已加载 shader/布局的具名程序列表 |
| `Material` | 借用一份 Resource，拥有参数名称、类型、顺序和默认值 |
| `MaterialInstance` | 借用一个 Material，按声明索引保存可选参数覆盖；未覆盖时使用默认值 |
| `MaterialRenderProxy` | 借用 Resource，拥有同步时解析后的完整参数副本 |

```text
应用持有 MaterialResource（性质、可用 shader 程序）
          ↑ 借用
应用持有 Material（参数声明与默认值）
          ↑ 借用
应用持有 MaterialInstance（参数覆盖）
          │ 显式 Proxy.Update(instance)，解析并复制参数
Scene 私有材质存储 → MaterialRenderProxy（借用 Resource + 自有参数快照）
          ↑ MeshBatch.Material 句柄
Pass Processor → 结合 Resource 选择 PSO；调用方把快照适配为 shader bindings
```

### 定义、实例与同步

`Material::Create(resource, parameters)` 复制声明，拒绝空名称、重复名称；允许零参数。
参数值 `MaterialParameterValue` 使用 variant 表达 `float`、`array<float, 4>`、
可空 `TextureView*` 和可空 `Sampler*`，空资源表示尚未赋值。参数名称拥有字符串，区分大小写。
`FindParameter` 返回声明顺序中的索引，`GetDefaultParameter` 读取默认值。
索引属于该 Material 的声明顺序；即使两个 Material 共享 Resource，也不能假设其索引含义相同。
当前没有 shader 反射或 ABI 校验，调用方负责保证声明与所选 shader 的实际输入契约匹配。

`MaterialInstance::SetParameter(index, value)` 拒绝越界或类型改变，失败不改变现有覆盖。
`ResetParameter` 恢复默认值，`GetParameter` 返回覆盖或默认值；不存在的索引返回空。
`Material` 的定义在实例使用期间不可修改、搬移、替换或销毁。
实例不支持父实例链、静态开关或由参数触发的 shader 编译；固定性质来自所引用的 Resource。

默认 Proxy 未准备，Resource 为空。`Update(material)` 复制默认值，`Update(instance)` 复制解析后的值，
二者均替换整个快照。之后修改实例不会自动影响 Proxy，必须再次 Update。
Proxy 不保留 Material/Instance 引用，同步后用户侧对象可以在无人借用时销毁。
Proxy 的参数引用不能跨 Update、对象移动或 Scene 的 SparseSet 搬移保存。
Update 与读取由调用方串行组织；`Scene::UpdateMaterial(handle, proxy)` 会标脏所有引用该 Proxy 的 Batch，
也可在直接 Update 后调用 `Scene::MarkMaterialDirty`。没有渲染线程或自动 GPU 上传。

Resource 及其借用的 Shader、PipelineLayout、TextureView、Sampler 仍由外部持有。
Resource 必须在被 Material/Proxy/样例 PSO 表引用期间地址稳定且内容不变；具名程序名称须唯一。
`FindProgram` / `GetProgram` 只做查找，不校验 shader 阶段、布局兼容性或 GPU 对象有效性。
资源参数仅复制指针，不增加引用计数；外部所有者须维持资源有效直至相关 CPU/GPU 使用结束。

### 材质与 Pass 的边界

`MaterialProperties` 包含 `MaterialTagSet Tags` 和 `MaterialRenderState RenderState`。
标签表达材质语义，状态提供具体 Pass 可沿用或覆盖的默认值。runtime 不内置标签词汇、
从标签推导状态的规则或自动状态组合 helper；标签不会自动生成 shader 功能。
样例使用 `Surface=Opaque/Transparent` 等应用标签，以 `CullMode::None` 表达关闭剔除。
关闭剔除只改变光栅化，不隐含 shader 的双面法线处理。

`MaterialRenderState` 直接使用 RHI 枚举/结构：

| 字段 | 默认值与含义 |
|---|---|
| `Cull` | `Back` |
| `Blend` | 空，关闭混合；有值时使用 `render::BlendState` |
| `ColorWriteMask` | `ColorWrite::All`；单组材质默认值，具体颜色附件由 Pass 决定 |
| `DepthTestEnable` / `DepthWriteEnable` | 均为 true |
| `DepthCompare` / `DepthBias` | `LessEqual` / 零偏移 |
| `Stencil` | 空，关闭模板测试；有值时使用 `render::StencilState` |
| `StencilReference` | `uint8_t`，默认 0，正反面共用；通过 RHI 动态设置 |

没有直接保存整个 RHI `DepthStencilState`，因为其中的附件格式属于本次 Pass。
附件格式、采样数、清除值、几何拓扑和资源同步仍由渲染流程提供。
这些属性与 Resource 一样在被引用期间不可修改；MaterialInstance 目前只覆盖参数。

`MaterialShaderProgram` 保存程序名称、借用的 Layout、VS/PS 和拥有的入口名称，PS 为空可表达纯顶点程序。
具名程序列表只保存当前可用程序，不表示材质自动参加同名 Pass。

PSO 还需要顶点输入、附件格式和经 Pass 确定的最终状态。runtime 材质不持有 PSO，不安排 Pass，
也不定义参数在 uniform buffer、descriptor 或 push constant 中的 GPU 布局。
`MeshDrawCommand` 保存 PSO、索引几何、模板参考值与绑定池句柄；顶点流、静态参数组和命令级 push constants 由列表的绑定池持有。
修改材质参数不要求重新创建 Resource/PSO；样例通过 `Scene::UpdateMaterial` 标脏关联缓存，
Processor 重建时读取新 Proxy，将 `BaseColor * Brightness` 的 RGB 和原 alpha 复制进命令的 16 字节颜色常量。
相机与局部缩放使用 Pass 级参数组，物体变换使用 Pass 级延迟实例流，均在提交时提供。
这些每帧数据变化无需重建命令；材质的稳定 GPU uniform buffer 更新路径尚未实现。

样例的 M1～M5 注释串起资源准备、定义/实例、Proxy 同步、PSO 准备和参数适配。
八个实例使用三份参数声明相同的 Material/Resource：普通不透明、双面描边不透明和 alpha 混合透明材质；
共享加载的 shader，由样例准备各 Pass 的六个 PSO。具体场景、筛选矩阵和状态见
[教学样例](../guide/build-test.md#mesh-drawing-pipeline-教学样例)。
Depth Pass 沿用材质剔除与深度偏移，显式选择 Less、开启深度测试/写入、关闭模板；
Color Pass 沿用材质颜色、深度和模板状态。双面材质使用 Always/Replace 写入模板参考值 7。
Outline Pass 筛选描边标签，以放大几何和 NotEqual 7 绘制金色轮廓，不改写深度/模板；
Transparent Pass 筛选透明标签，使用材质 alpha 混合、深度测试和关闭深度写入的状态。
附件采用 D32_FLOAT_S8_UINT，参考值由持久 `MeshDrawCommand` 保存并在 Draw 前录制；生命周期见 [RHI 模板参考值](render-rhi.md#模板参考值)。
颜色默认值/覆盖和 Brightness 都由 Processor 从 Proxy 获取，准备阶段解析参数名称，构建命令时使用索引。
Processor 同时检查各自要求的材质标签与样例 `BatchPassPolicy`。没有材质图、编译/静态变体、fallback、纹理绑定演示、
uniform expression 求值、GPU 参数缓存或热重载。

### 材质标签的身份和存储

`material_tag.h/.cpp` 提供 `MaterialTagId`、`MaterialTag`、`MaterialTagTable` 与 `MaterialTagSet`。
应用显式持有一张表供材质和 Processor 共用；表不可复制或移动，必须晚于所有使用者销毁。
没有全局注册表。ID 是表内的 32 位编号，0 无效；不能跨表比较或直接作为持久化标识。
即使两个表碰巧发出相同编号，也不表示同一个字符串。集合写入时能检查零和越界，
不能仅凭编号识别来自另一张表的 ID；同表来源是调用契约，样例还检查集合与 Processor 的表身份。

`Intern(text)` 拒绝空字符串，区分大小写，相同文字返回相同 ID；`Find(text)` 不创建条目。
`GetText(id)` 反查文字，非法编号返回空。字符串复制到连续字符池，以偏移和长度定位，
再用按文字排序的 ID 数组做准备阶段二分查找；插入索引需要移动后续条目，登记不是每 draw 操作。
字符池扩容不改变 ID；反查所得 string_view 不能跨后续 Intern 保存，长期引用使用 ID。

默认 MaterialTagSet 未绑定、为空；用 `MaterialTagSet{table}` 创建可写集合。
`Set(key, value)` 验证编号并替换同 Key 的已有值，失败不修改集合。
`Find`、`Matches` 和 `GetTags` 只读；集合使用 `InlineVector<MaterialTag, 4>`，超过四项才使用堆存储，
复制集合拥有独立的标签对但仍借用同一张表。绘制期间查找只比较 ID，不访问字符串或分配内存。
表及集合的读写由调用方串行组织；Resource 发布后不修改标签。
shader 程序名和参数名维持已有准备阶段解析方式，不使用标签表。

## Mesh Drawing Pipeline 基础接口

| 文件对 | 接口与职责 |
|---|---|
| `mesh_draw_command.h/.cpp` | 两级命令、自持有命令常量、Pass 提交参数、命令解析、校验与统一录制 |
| `mesh_pass_processor.h/.cpp` | `MeshBatchContext`、Pass 筛选接口、共享状态与 Batch 构建 helper、Pass 登记；持久缓存与当帧动态两条输出路径的 Context |
| `scene.h/.cpp` | 脏追踪、增量同步、可见命令收集 |

### 增量持久缓存

`Scene::SyncCachedMeshDrawCommands(passes)` 对脏 Batch 调用已登记 Processor。
首次同步构建全部 Batch；Pass ID、Processor 身份或 SortMode 变化时清空缓存并全量重建。
Pass ID 必须是紧凑索引，不可重复或使用最大 uint32 值，Processor 必须非空；登记违约在修改缓存前直接 `RADRAY_ABORT`。
撤销 Pass 后不再生成其命令。重复标脏只入队一次；同步跳过已删除的队列项，对每个存活脏 Batch 先清除旧命令再重建，
完成后清空脏队列与脏计数，正常返回保证 `!HasDirtyBatches()`。构建 helper 仅在 Debug 检查违约并终止程序；
Release 信任输入前置条件，不保证违约诊断。同步不保留失败回滚或跨帧重试路径。

`MeshPassProcessor` 不持有输出 Context；默认构造，调用方每次显式传入输出目标，无需绑定、切换或恢复成员状态。
`MeshBatchContext` 借用匹配的 Primitive/Batch、句柄与已解析 Material，仅在 AddMeshBatch 内有效。
`AddMeshBatch(context, output)` 返回 void，可以筛选并跳过 Batch；接受后调用 protected static 的
`BuildMeshDrawCommands(context, sharedState, output)`，同样返回 void。元素总数不得超过 32（包含空条目）；
helper 在所有配置下查找非零索引且非零实例的元素，没有可绘制元素时直接返回，不访问 VertexInput。
非空 Batch 必须有合法 VertexInput，提供 PushConstants 时其源数据必须非空且指针有效。
输入校验统一由 `#if RADRAY_IS_DEBUG` 控制：Debug 先检查元素数量上限，再判定空 Batch，
并在组装命令前校验 VertexInput 和常量数据；组装后调用 `ValidateConstruction` 验证 PSO、ParameterSets、PushConstants 和命令数据。
其中 Scene 原入库元素不重复查询索引 Buffer 描述，独立动态构建与替代 Batch 则完整校验 Element；任一校验失败直接 `RADRAY_ABORT`。
Release 不执行元素数量、指针、VertexInput 或 `ValidateConstruction` 校验，由调用方保证全部构建前置条件。
允许延迟绑定的唯一 Instance 布局槽位在组装后记录，位于 Debug 的 `ValidateConstruction` 之后，避免构建期要求尚未提供的 Pass 实例流；
提交期仍遵守实例流校验契约。
随后 Store 入池、计算状态排序键并单步发布命令。

`MeshPassDrawListContext::AddCommand(context, primaryElementIndex, initializer, stateSortKey)` 接收已组装的命令，
转移绑定引用所有权并发布排序键和主元素默认实例范围，不重写几何字段；主元素索引必须对应有效掩码的最低位。
基类的 protected 构造函数可接收已校验的 Batch 借用，公开 `HasValidatedElements` 按对象身份查询，
该 Batch 的元素在 Context 生命周期内必须保持有效且不变。Scene 将原入库 Batch 通过缓存 Context 构造函数传入。
每个缓存 Context 至多发布一条命令，发布时同时写入 Batch 槽位的状态排序键与默认实例范围。
动态 Context 立即追加命令与可见项，命令指针仍在 `FinalizeVisibleCommands` 时统一填充，以允许生成期间扩容。

每 Batch/Pass 最多一条命令，`ValidElementMask` bit i 对应源 `Batch.Elements[i]`；空 Element 不占有效位，
但保留源位编号。最低有效位的几何内联，其余按位顺序存入 ExtraElements。
默认实例范围完全相同时不存逐元素范围；存在差异时 ElementInstances 按有效位顺序存储全部范围。
构建期 PushConstants 是借用 span，入池时复制为自持有字节。缓存只保存跨视角稳定状态，
相机、物体变换与 Pass 常量不进入持久命令。

`CachedPassMeshDrawList` 私有持有连续 Commands、CommandBatchIndices 与按 Batch 槽位下标寻址的热表。
每个 `PassBatchCacheSlot` 为 16 字节，包含 CommandDenseIndex、StateSortKey、FirstInstance、InstanceCount。
追加命令写入反向归属；删除或重建交换末项并 O(1) 修正被移动 Batch 的 CommandDenseIndex。
扩容只改变地址，不改变稠密下标。对外只提供 const 命令、槽位、绑定池视图；Scene 外部无法删除其缓存。
单独拥有的缓存列表可通过 CachedPassMeshDrawListContext 构建，不能复制；移动时修正命令的池指针。

### 命令布局与状态驻留

`MeshDrawCommand` 是平凡可复制/析构的 64 字节结构，按 64 字节对齐；`VisibleMeshDrawCommand` 为 32 字节。
命令包含 PSO、主 IB/索引范围、绑定池指针、有效元素掩码、三个 32 位池索引与模板参考值。
`kInvalidBindingIndex` 为最大 uint32。池索引没有 Generation，不能跨 owner 销毁、Clear 或回收复用保存；
显式校验不会识别已复用索引对应的旧借用。命令副本只借用原引用，不获得销毁权。

`MeshDrawCommandBindingStorage` 的池均为私有：

| 内容 | 存储与生命周期 |
|---|---|
| `MeshVertexBufferBindings` | 全部 VB 按 Binding 升序正规化，连同可选 InstanceStreamBinding 按内容驻留 |
| `MeshDrawCommandPayload` | 参数组、完整动态 offset 与自持有 PushConstants 按内容驻留 |
| `MeshDrawCommandElements` | ExtraElements、ElementInstances 及预计算合并信息；逐命令独占 |

驻留池计算 64 位字段哈希，命中后完整比较内容以处理碰撞；相等条目递增引用计数并复用索引，
Destroy 递减，归零才回收。VB View 保留 64 位 Offset/Size，Binding 与 InstanceStreamBinding 保留 32 位。
`Store` 要求无已有绑定；一次 Store 获得一个引用，交给列表或由调用者 Destroy。Clear 整体释放引用。
`GetVertexBuffers`、`GetPayload`、`GetExtraElements` 返回 const 数据，地址不能跨池修改保存。
PSO、Buffer、ShaderParameterSet 仍由外部 owner 保活，载荷不再借用 Batch、VertexInput 或构建时常量源。

### 排序键与当帧可见路径

`StateSortKey` 与 `VisibilitySortKey` 均为 32 位，由 `ComposeMeshDrawSortKey` 合成 64 位键：

| MeshPassSortMode | 高 32 位 | 低 32 位 |
|---|---|---|
| StateFirst（默认） | 状态键 | 可见性键 |
| DepthFirst | 可见性键 | 状态键 |

默认状态键是 Pipeline 8 位、Payload 12 位、VB 12 位的桶组合；Pipeline 编号在所属绑定存储中按首次出现登记，
其他两项使用驻留池索引。空索引映射 0，范围内索引映射 index + 1，超出范围时混合哈希后折叠至非零桶，
不污染邻接位域。桶碰撞只影响聚类，不影响正确性。`CustomStateSortKey` 可显式替换默认状态键，包括指定 0。

缓存路径通过 `VisibleMeshBatch::VisibilitySortKey` 提供当帧键；动态路径使用 `MeshBatchContext::VisibilitySortKey`。
两者使用相同合成函数。深度键由调用方编码，按无符号升序排序；DepthFirst 不自动计算距离或反转深度。
`MeshPassRegistration::SortMode` 定义 Scene Pass 模式，Scheduler 在 Extract 前设置当前槽的对应列表。
手动混合缓存/动态命令时须先设置 MeshPassFrameDrawList 的模式，再生成命令；已有输出不能中途更改模式。

`GatherVisibleMeshDrawCommands` 检查 Batch 代次与独立脏位，直接读取 Pass 热表及命令数组，不访问冷 Batch。
Gather 的脏项检查不能检测所有漏同步：缺失 pass cache 会缺少输出，processor/registration 变化也不一定使已有 batch 变脏；调用方仍须使用当前 registrations 显式同步。
`ElementMask & ValidElementMask` 非零时仅追加一条可见项；零掩码、空元素位、不存在的位均不输出。
无效或脏 Batch 回滚本次追加；未注册、撤销或被 Processor 筛选跳过的 Pass 合法产生空输出。
`GatherVisiblePrimitives` 遍历 Primitive 的 Batch 列表；剔除与 LOD 由应用负责。
`FirstInstanceOverride` 与 `InstanceCountOverride` 独立覆盖所选全部元素；未覆盖的部分保留元素默认范围。

可见项保存排序键、const 命令指针、实例标量、元素掩码和两个 UseElement 来源标记。
标记为 true 且存在 ElementInstances 时读取逐元素值，否则读取可见项标量；手写可见项默认使用标量。
缓存指针只在帧内 Scene 借用期间有效；Scene 变化后必须重新 Gather。
同键保持输入顺序，Batch 内按源下标递增绘制；需要独立材质或排序位置的 Element 应拆成不同 Batch。

`MeshPassFrameDrawList::SortVisibleCommands` 对不超过 64 项使用原地稳定插入排序；更大数组复用常驻 scratch
执行八位一轮的稳定 LSD 基数排序，跳过全数组无差异的字节。容量足够时排序不分配临时堆缓冲。
Sort 和 Submit 都先完成动态指针发布。动态 Context 在 AddCommand 时追加元数据，
`FinalizeVisibleCommands` 在全部生成后填入稳定命令指针并冻结列表；之后不能增删或改变命令存储。
下一帧清空 VisibleCommands 并 Clear Context，保留容量。动态池由 unique_ptr 保持移动时身份稳定；
列表移动后 Context 重新绑定可见数组，已发布指针仍有效。

### 连续元素合并

`AllowElementMerging` 默认 false。启用后，仅 PointList、LineList、TriangleList 参与合并，
元素索引数分别须为正数、2 的倍数、3 的倍数。Strip 等其他拓扑不参与。
Build 只对源下标紧邻、均有效、默认 InstanceCount 为 1 且 FirstInstance 相同的元素检查：
IB View 相同、VertexOffset 相同、索引区间首尾连续，且整段累计 IndexCount 不超过 uint32 上限。
满足条件时将起始源位写入 ExtraElements 中的 `ContiguousMergeMask`；同时计算全部有效元素是否共用主 IB。

Submit 解析当前实际实例范围；只有实际 InstanceCount 为 1 时，按合并掩码与下一个可见位连续累加索引数。
可见性空洞、多实例覆盖都会截断合并；单实例统一覆盖仍可合并。提交内层不再重复拓扑、IB 相等和溢出检查。
本命令首个实际 Draw 必须检查并按需绑定 IB；只有已绘制且 AllElementsSharePrimaryIndexBuffer 为真时，
后续元素才能省略 IB 比较。合并只发生在同一 Batch 内。

### 校验与录制

`SubmitMeshDrawCommands` 检查 encoder；Debug 在首次绑定前完整预检全部命令、池身份/有效索引、几何、绑定和 Pass 参数。
实例溢出检查只针对选中元素；失败不留下部分 Draw。Release 由调用方保证命令与提交参数有效，不查询 Buffer 描述。
显式 ValidateMeshDrawCommand 在两种配置下都完整校验。检查范围包括 PSO/Buffer/参数组有效性、usage、范围、
索引宽度/对齐、重复槽位/参数组/动态 offset、实例整数范围、延迟实例槽位、PushConstants 对齐/范围，
以及顶点组顺序、掩码与扩展元素对应关系。Pass 与命令参数组不能使用同一 GroupIndex。

局部状态缓存在每次 Submit 内去重 PSO、VB、IB、StencilReference 与参数组。
VB 先比较池地址和 VertexBuffersIndex，相同则跳过组解析；变化后按槽位 0～31 定长表比较完整 View，
更高槽位直接绑定。连续脏槽位使用原数组 subspan 下发，遇到未变槽位、间隙或被 Pass 实例流覆盖时截断。
Payload 同样比较池地址和 PayloadIndex；相等时跳过解析与参数组比较，有 PushConstants 时仍每条命令写入。
PSO 切换会清空 VB/Payload 身份及参数组缓存，重绑 Pass 参数组与实例流；IB 和模板参考值保留。
零实例 Draw 跳过；零掩码项不绑定状态。空命令指针或 SetPushConstants 失败返回 false，调用方须放弃录制。

RHI 公共接口不暴露 PSO 布局和已写参数资源，runtime 无法检查布局兼容、必需参数、动态 offset 设备对齐或读取索引内容。
这些条件由应用与 RHI 校验层保证；Pass 开闭、viewport/scissor、附件、barrier、提交和等待由调用方组织。
测试位于 `test_mesh_draw_command.cpp`、`test_mesh_pass_processor.cpp`、`test_frame_slot.cpp`、`test_scene.cpp`、
`test_frame_scheduler.cpp` 和 `test_rdg.cpp`，观测状态仅由 fixture/fake 持有。

## 环境

`Environment::Create(backend, validation, appName)` 创建所选后端的 DXGI factory 或 Vulkan instance，
不创建 GPU device。`appName` 传入 Vulkan instance 描述，默认值为 `RadRay`。
validation 开启时配置 D3D12 诊断上下文和 debug layer，或 Vulkan debug layer 与 synchronization validation。

Environment 拥有 D3D12 factory，或负责 Vulkan environment 的初始化与关闭；记录创建线程、backend 和 validation 设置。
`IsOwnerThread` 提供线程身份查询，不提供自动调度或并发保护。后端可通过
`RADRAY_ENABLE_D3D12` / `RADRAY_ENABLE_VULKAN` 禁用；请求未启用的后端会失败并记录日志。

## 设备与队列

`DeviceContext::Create(environment)` 借用环境，选择高性能适配器，创建 GPU Device 和一条 Direct 队列。
默认设备不启用外部资源共享，也不接收宿主 GPU 身份；需要外部共享的调用方自行创建设备并使用现有 owner 构造函数。
创建失败记录日志并返回空 owner。

DeviceContext 也提供接收 `shared_ptr<render::Device>` 与非空 Direct 队列指针的构造函数。
调用方须保证队列属于传入设备，且底层环境寿命满足设备要求。
DeviceContext 持有共享 Device owner，队列指针从设备借用；`GetDevice` / `GetQueue` 返回借用引用。
`SupportsIndirect` 实时读取 RHI 的 `IndirectDraw` 和 `IndirectFirstInstance` 能力，不依赖后端内部头。

## 生命周期与失败

Environment 必须晚于 DeviceContext 及所有依赖它的 GPU 资源销毁。应用应先完成 GPU 工作、释放资源，
再销毁 DeviceContext，最后销毁 Environment。DeviceContext 析构会等待其 Direct 队列，然后释放 Device owner。
如果外部仍持有同一 Device 的共享引用，Environment 也必须继续存活。

调用方在环境的创建线程串行组织生命周期，RHI 的 CPU 调用遵守其串行化契约。
应用可使用上述多槽帧循环，或直接通过 RHI 组织 Pass、录制、同步和提交；帧资源释放由 Slot 的 ObjectArena 承担，
共享旧资源可通过当前 Slot 的 `RetireResource` 保活；跨 Scheduler 或跨队列的通用退役仍由应用组织。

可失败工厂返回 `Nullable<unique_ptr<T>>`，失败处记录日志，调用方检查返回值；不通过接口传递错误字符串。
Vulkan instance 和 D3D12 factory 的底层环境约束仍由 RHI 管理。
