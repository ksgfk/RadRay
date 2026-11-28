> - 适用: 创建 runtime 环境与设备，组织材质与 Scene 绘制输入，扩展 MeshPassProcessor，使用 RDG 驱动多槽帧循环
> - 权威: 本文维护 runtime 当前接口边界与所有权；底层资源契约见 RHI 文档
> - 锚点: `modules/runtime/include/radray/runtime/`, `modules/runtime/src/`, `modules/runtime/CMakeLists.txt`, `modules/core/include/radray/sparse_set.h`

# Rendering runtime

`RadRay::runtime` 是 C++20 静态库，实际 target 为 `radrayruntime`，公共依赖 `RadRay::gpu_impl`、`RadRay::shader`，
并通过它们依赖 core。公共类型位于 `namespace radray`，RHI 类型位于 `radray::gpu`。
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
| `scene_handle.h` | `SceneHandle<T>`、`MeshBatchHandle`、`PrimitiveHandle`、`MaterialProxyHandle` | 无 |
| `material_tag.h` | `MaterialTagId`、`MaterialTag`、`MaterialTagTable`、`MaterialTagSet` | 有 |
| `material.h` | `MaterialRenderState`、`MaterialProperties`、`MaterialShaderProgram`、`MaterialAsset`、`MaterialAssetDesc`、`MaterialParameterValue`、`MaterialParameter`、`MaterialInstance`、`MaterialRenderProxy` | 有 |
| `vertex_input.h` | 几何描述、`ValidateVertexInputLayout`、`ResolveVertexInputState` | 有 |
| `shader_asset.h` | `ShaderAsset`、内存与离线索引加载 | 有 |
| `gpu_pipeline_layout.h`、`pipeline_layout_registry.h` | `GpuPipelineLayout` 持有原生布局，`PipelineLayoutRegistry` 按签名复用组布局和完整布局 | 有 |
| `asset_handle.h` | `AssetId`、类型化和擦除类型的弱句柄 | 无 |
| `asset_manager.h` | `AssetManager`、`AssetRef<T>`、`AssetTransaction`、`AssetGarbageBatch` | 有 |
| `material_parameter_binding_map.h` | `MaterialParameterBindingMap`、常量写入与资源绑定 | 有 |
| `mesh_draw_command.h` | `MeshPassId`、`MeshVertexStreamLayout`、命令绑定、`MeshDrawCommand`、`PassBatchCacheSlot`、`CachedPassMeshDrawList`、`VisibleMeshDrawCommand`、`MeshPassSubmitArgs`、`RDGBufferSlice`、`MeshPassIndirectDrawCounts`、`MeshPassIndirectBatch`、`PreparedMeshPassIndirectDraws`；`ValidateVertexInput`、`ValidateMeshBatchElements`、`ValidateMeshDrawCommand`、`SubmitMeshDrawCommands`、`CountMeshPassIndirectDraws`、`BuildMeshPassIndirectDraws`、`SubmitMeshPassIndirectDraws` | 有 |
| `mesh_batch.h` | `MeshSection`、`MeshBatchElement`、`MeshBatch`、`PrimitiveSceneInfo` | 无 |
| `mesh_pass_processor.h` | `MeshBatchContext`、`MeshDrawCommandSharedState`、`MeshPassDrawListContext` 及缓存/动态两个实现、`MeshPassProcessor`、`MeshPassRegistration` | 有 |
| `scene.h` | `VisibleMeshBatch`、`Scene` | 有 |
| `frame_slot.h` | `MeshPassFrameDrawList`、`FrameSlot` | 有 |
| `frame_pipeline.h` | `FramePipeline` | 有 |
| `rdg.h` | `RDGAttachmentSlice`、`RDGBuilder`、`CompiledRDG`、`RDGTransientResourcePool`、Texture/Buffer 句柄、Raster/Compute/Copy Builder 与 Context | 有 |
| `render_graph_pipeline.h` | `RenderGraphFramePipeline` | 有 |
| `frame_scheduler.h` | `FrameScheduler` | 有 |
| `gpu_resource.h` | `GpuBuffer`、`GpuTexture`、资源描述与 `ImportPersistent` 重载 | 有 |
| `texture_asset.h` | 持有可采样纹理并分类底层对象替换 | 有 |
| `staging_buffer_pool.h`、`dynamic_buffer_arena.h` | 每槽传输暂存和管线直读切片、分页映射与聚合 Flush | 有 |
| `geometry_buffer_pool.h` | `VertexBufferPool` / `IndexBufferPool`、descriptor 与只移动子分配句柄 | 有 |

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
| `frame_slot.h/.cpp` | `FrameSlot` 拥有按 Pass 的 VisibleCommands/DynamicContext、线性 PMR 与对象清理链、临时 PMR pool、command allocator、可复用的 command buffer 池；不保存源 Scene 指针 |
| `frame_pipeline.h/.cpp` | `FramePipeline` 定义 Pass 登记及 `Extract`、`Prepare`、`Record`、`Submit`、`Wait` 五个串行阶段 |
| `frame_scheduler.h/.cpp` | `FrameScheduler` 独占一个 Pipeline 和多个 Slot，负责选槽、调用等待、重置、阶段调用与轮转 |

`FrameScheduler::Create(device, pipeline, slotCount = 2)` 按值消费 Pipeline 的 owner，创建失败返回空。
槽数必须大于 0，创建后固定；`GetSlotCount()` 返回槽数，`FrameSlot::GetIndex()` 返回稳定的 `[0, slotCount)` 索引。
`FrameSlot::GetFrameId()` 返回 `uint64_t` 帧编号，在各 Scheduler 内从 0 独立递增，与循环使用的槽索引无关。
Scheduler 在 BeginFrame 中等待槽的旧帧完成并写入新编号，因此准备、Extract、Record 和对应的 Wait 看到同一个编号。
每次 Submit 正常返回后递增编号，空帧也会递增；帧执行错误带诊断终止程序，不提供回滚或重试。
WaitIdle 不重置计数，槽清理后仍保留最后分配的编号。计数到达 `uint64_t` 最大值时带诊断终止，不回绕。
DeviceContext 必须晚于 Scheduler 销毁。每个 Slot 的 `GetDevice()`、`GetQueue()` 返回所属设备和队列的借用引用。
`BeginFrame() -> FrameSlot&` 开启下一槽，在场景更新和上传准备之前完成等待、allocator Reset、参数集解绑、帧数据清理和退役清理。
开启后重复调用直接返回同一槽，不重复等待、析构帧对象或释放准备期登记的退役对象。
`GetCurrentSlot()` 仅在 BeginFrame 至 Submit 返回之间返回该槽，其他时间为空。
准备过程未调用 RunFrame 时，下一次 BeginFrame 保留本轮准备；WaitIdle 则清理并取消准备，保留下一槽索引与帧编号。取消时帧槽析构已登记的对象，连同当帧设置的采样档位与用户序号一起清理。
`RunFrame()` 可直接调用；尚未开启槽时内部自动调用 BeginFrame。完整顺序为：

```text
BeginFrame（只在未开启时执行）：选槽 → 若有在途提交：Pipeline.Wait(slot)，清除在途标记
    → Reset command allocator → 清理槽内 CPU 数据 → 析构帧内对象 → 析构退役 owner → 分配当前帧编号
    → 返回当前槽，调用方准备具体管线输入、扩容本槽上传缓冲或登记退役资源
    → 使用 Scene 时：显式 SyncCachedMeshDrawCommands(pipeline.GetMeshPassRegistrations())
    → 将输入设置到具体管线
RunFrame：复用已开启槽（或先自动 BeginFrame）
    → Extract(slot)：按需收集 Pass 的缓存命令借用、动态命令和当帧覆盖项
    → Prepare(slot)：排序和固定可见命令，确定尺寸、物化资源、填充上传、准备绑定、取得 command buffer
    → Record(slot)：只消费冻结的数据，Begin、编码命令、End
    → Submit(slot)：提交命令，发布已提交状态并保存完成信息
    → 标记在途，下一帧编号递增，当前槽索引 = (当前槽索引 + 1) % slotCount
    → RunFrame 返回，结束外部输入借用
```

`RunFrame` 在成功提交后返回 true，GPU 的异步完成由围栏报告。设备丢失时返回 false，关闭当前槽的 `Emplace` 登记窗口，保留槽、帧号与本帧已登记的对象，供 `WaitIdle()` 回收。健康设备的内部提交被拒绝时立即终止。只有成功提交的图发布持久资源终态。每次成功提交的槽在 Wait 返回后收到一次 `OnSlotRetired(slot, deviceLost)`，随后清除在途标记。未提交或已回收的槽跳过 Wait 和退役回调。
`WaitIdle()` 先等待所有在途槽，再对**全部槽**调用 `ResetCommandStorage()`，
解除 command allocator 所有 epoch 引用。随后关闭准备窗口，按帧从旧到新对各槽调用 `ResetFrameData()`，
清空 CPU 数据并析构登记的对象；已开始但尚未提交的当前槽最后清理。
这一顺序使旧帧的绑定和 view 先于移交到较新帧的旧 owner 销毁。同槽依赖仍须遵守下述登记顺序。
不能逐槽完成等待、命令重置和对象清理，否则可能释放仍被其他槽旧录制引用的 Buffer；
它保留当前索引，重复调用不会重复等待已回收提交。Scheduler 析构先执行 WaitIdle，
再销毁全部 Slot，最后销毁 Pipeline，确保 Pipeline 拥有的资源晚于命令存储释放。
Slot 自身不持有 fence 或完成值，Reset 和析构均不等待。
FrameScheduler 的调用与析构由宿主串行组织，回调期间禁止重入。命令录制遵守 [RHI 命令存储契约](render-rhi.md#命令录制存储)。

### 帧内分配与资源释放

每个 Slot 直接拥有 `std::pmr::monotonic_buffer_resource`、一条对象清理链和一个 `std::pmr::unsynchronized_pool_resource`。
`GetMemoryResource()` 返回线性 resource，供帧对象和 RDG 跨阶段数据使用；
`GetScratchMemoryResource()` 返回临时 pool，供 Compile／Prepare 的局部工作容器使用。
线性存储的初始缓冲为 128 KiB，首次使用时分配；回收保留初始缓冲，归还额外块。
二者只供本槽串行 CPU 阶段使用，不提供并发保证，也不跨槽共享。两者默认共用槽内 `HeapMemoryResource` 作为上游。
`FrameScheduler::Create` 可传入自定义 memoryUpstream，借用的上游必须晚于调度器销毁。

`FrameSlot::GetMemoryUsage()` 按本次槽使用计量。首次分配线性初始缓冲时，`InitialBytes` 记录 128 KiB，后续帧复用该缓冲时为零。
`LinearOverflow` 和 `ScratchGrowth` 分别累计线性额外块和临时 pool 向上游成功申请的次数与字节数。归还内存不会扣减计数，复用已有空间不会增加计数。槽回收时三项清零。

调用方在当前槽 BeginFrame 后至 Submit 返回前，用 `slot.Emplace<T>(args...)` 创建帧内对象。
构造参数原样转发，对象地址与对齐保持有效直到槽回收，调用方不得手动析构或 delete 返回的对象。
平凡可析构对象只分配本体；非平凡对象和清理节点合并分配，构造成功后才挂入清理链，按构造完成的逆序析构。
分配和构造异常原样传播，失败对象不登记，其存储留到槽回收；成功的嵌套对象仍由该槽清理。
对象析构必须 noexcept；构造和清理期间不得重入槽回收或调度器。

转移 `unique_ptr` 所有权即可延迟释放 GPU 资源，例如已成功创建的 `unique_ptr<gpu::Buffer> buffer`：

```cpp
auto* owner = slot.Emplace<unique_ptr<gpu::Buffer>>(std::move(buffer));
auto* borrowedBuffer = owner->get();
```

后续录制可借用 borrowedBuffer，owner 在槽回收时析构。槽内 owner 默认只供该槽使用；
若其他槽也引用它，必须在 owner 槽再次复用前调用上述 WaitIdle，或由外部 owner／各使用槽的共享引用保活。
shared_ptr 可同样保活共享资源；自定义清理通过 Emplace 移入 MakeScopeGuard 的结果。
有依赖的资源应先登记被依赖者、后登记引用者，或归入同一个 RAII 对象。

回收先 Reset command allocator，紧接着重置槽内 `TransientParameterSetPool`，再清空各 Pass 的可见/动态命令并统一析构登记对象。
从 BeginFrame 至提交前可用 `FrameSlot::AcquireParameterSet` 获取当帧已填充的参数集。相同布局和相同绑定内容共享只读结果，绑定写入顺序可互换。自定义写入使用 `AcquireUninitializedParameterSet`。
`TransientParameterSetPool` 按布局维护开放寻址平坦去重表（`KeyBindings`、`Entries` 与 2 的幂长度 `Buckets`，装载率至多 1/2），跨帧保留数组容量；`BeginFrame()` 仅清理上一帧实际占用的桶位，容量预热后的稳态帧零堆分配/释放。
获取时按 `(BindingId, ArrayElement)` 检查输入，已严格递增的序列直接借用到本次调用结束，乱序输入使用池的临时数组排序。重复槽位返回空值。
哈希、比较、写入和持久键复制使用同一份有序序列，每次获取只定位一次布局组。写入或刷新失败时先清除部分绑定，再归还集合供重试。
每个布局按最近八次槽回收的使用峰值保留参数集，持续闲置会释放布局。`AllocateDynamicUniform` 将常量数据复制到对齐的 DynamicBufferArena 切片，返回有效大小与物理保留范围。
清理回调执行时，当前槽的 CPU 可见/动态命令记录均已清空，Pipeline 和 DeviceContext 仍有效。

【槽回收与 WaitIdle 先析构对象，再重置线性存储。】重置在原位置重建标准 monotonic resource，
重新传入保留缓冲，不依赖不同 STL 的 release 对初始缓冲及增长尺寸的恢复行为。
临时容器在局部作用域结束时向 scratch pool 归还单元，不调用 pool.release，缓存跨帧复用。
FrameSlot 析构清理全部对象后释放线性存储与 pool 的保留存储，最后销毁默认上游。
使用帧线性 resource 的外部 PMR 容器必须在帧对象清理前销毁；clear 仍保留 capacity，不能替代销毁。
将容器本体通过 Emplace 登记到槽时，须显式传入分配器；不得把它的借用带到下一帧或其他槽，
也不得手动 release 槽的 resource。下一帧重新获取帧 resource 引用；临时容器不得被录制结果引用。
可见命令、动态命令外层列表、排序暂存数组和命令缓冲列表继续保留自身 capacity，不迁入每帧重置的 arena。
线性存储不单独回收容器扩容后的旧容量；可预测大小时一次 reserve 或 resize，不逐次 reserve(size + 1)。
对象构造参数原样转发，PMR 容器必须显式选择 resource，例如：

```cpp
auto* values = slot.Emplace<std::pmr::vector<int>>(&slot.GetMemoryResource());
values->reserve(256);
values->push_back(42);
```

当前 pool_options 为每 chunk 最多 4096 个单元、最大池化请求 64 KiB；更大请求直接经过上游。
这些是标准库的调节参数，实际块策略由实现决定，不是总保留容量上限。
统计 CPU 存储时须同时计入线性 arena 的保留缓冲、额外块和 scratch pool 缓存，不把保留量称为有效数据量。

旧资源也直接用 `slot.Emplace<decltype(owner)>(std::move(owner))` 移交 RAII owner，共用上述存储和清理链。
仅允许在当前槽 BeginFrame 返回后至 Submit 返回前登记；Wait、清理回调和未开启帧时通过保存的槽引用登记均违反调用前置条件。
`RADRAY_IS_DEBUG` 下用槽内回调活动标记检查这一窗口，违规时带诊断终止；Release 不保存该标记，也不执行窗口检查。
准备及 Extract / Prepare / Record / Submit 共用同一登记窗口，无需借用 Scene。重复 BeginFrame 不缩短已登记资源的寿命。
移交旧 owner 前必须停止对旧资源的新使用；已有录制可以继续提交和执行。
当前槽已有依赖对象时，不能再单独晚登记其底层 owner；应提前登记 owner，或把依赖对象与底层资源组合为具有正确析构顺序的一个 owner。
登记时当前槽已完成本轮清理；下一次复用该槽前，固定轮转已使其他槽在登记后重置过 allocator，
并清理旧帧对象，再等待并重置当前槽 allocator 后释放移入的 owner。没有单独的退役队列、批次或槽完成掩码。
`WaitIdle()` 遵守前述从旧到新的清理顺序，不需要先轮转完未使用槽。
未开启帧时产生的旧 owner 可在下一次 BeginFrame 返回后直接转交，或在 `WaitIdle()` 后释放。
这只覆盖本 Scheduler 的单队列槽，不等待外部录制或其他队列。

### 命令录制与 Pipeline 契约

Pipeline 在 `Prepare(slot)` 中通过 `slot.AcquireCommandBuffer()` 申请命令缓冲。
Slot 初始不创建 command buffer，每次申请返回本轮尚未借出的一个 buffer；缓存不足时创建，创建失败返回空。
同一轮内每次申请返回不同对象，所有权始终留在 Slot。槽重置时 allocator 统一 Reset，借用结束，
下一轮从池首复用；池保留历史最大数量，不因本轮使用较少而销毁多余对象。
Pipeline 不得跨槽复用保存的借用，每条命令在本轮最多 Begin/End 一次、提交一次。
同槽共享一个 allocator，为满足 D3D12 契约，同一时间只允许一条命令处于 Begin/End 区间。

Prepare 确定命令缓冲数量（可以为 0），Record 完成 Begin/End，Submit 通过 `slot.GetQueue()` 提交。
Scheduler 不接收或操作单条 command buffer；BeginFrame 仅开启准备窗口，具体准备和提交由调用方及 Pipeline 执行。
Submit 正常返回前，Pipeline 必须保存足够的按槽完成状态，供 `Wait(slot)` 等待本槽全部提交。
`Wait` 是同步 `void` 接口，
返回即保证全部槽内工作完成、录制存储和帧数据可重置或销毁；不允许将超时当作完成返回。
具体实现可以使用 fence、事件或其他机制；无提交或同步完成的 Submit 可搭配空操作 Wait。

五个阶段均为 `void noexcept`，只允许正常完成或在发现错误的位置带诊断终止程序。
无效输入、资源准备或录制失败由 Pipeline 使用 `RADRAY_ABORT` 处理，不依赖仅在 Debug 生效的断言；
Scheduler 没有失败回滚、部分提交恢复或重试分支，不捕获异常恢复执行。
Record 正常返回前必须关闭全部 encoder 并 End 全部已开启的 command buffer；Extract 不得提交异步工作。
没有可见物体或没有绘制命令是正常空帧，不属于失败。

Pipeline 按槽索引维护完成状态及可复用的上传缓冲、常量等可写帧资源；可以预分配或首次使用时创建。
仅供当前帧使用的资源可交由槽内对象清理链持有并释放。
这些资源必须按槽独立分配或使用互不重叠的存储区域，在对应 Wait 返回前不能覆盖。
同一个 Pipeline 可以被多个槽依次调用，但不能共用一块每帧覆盖的上传数据。

帧入口仅有 `RunFrame()`；FramePipeline 及 RDG 扩展钩子不接收 Scene，帧管线头文件也不包含 scene.h。
具体管线持有自己的内容源；使用 Scene 的调用方在提交前设置管线输入，并显式调用
`scene.SyncCachedMeshDrawCommands(pipeline.GetMeshPassRegistrations())`。调度器不自动同步缓存。
每次同步必须传入当前 registrations，不能仅以 `HasDirtyBatches()` 决定是否调用；缺失 pass cache 或 processor/registration/sort mode 变化可以发生在没有脏 batch 的情况下。
`GetMeshPassRegistrations()` 默认返回空 span，纯 RDG 管线可保留默认值。FrameSlot 的 mesh 绘制列表与排序流程继续可用。
外部输入在 RunFrame 返回前必须有效且只读，Wait、槽回收及对象析构不得访问这些借用输入。
多个 Scene 各自拥有独立容器及缓存；管线可以在帧间切换输入。FrameSlot 不保存 Scene 指针，也不提供 GetScene。
调用方前置条件不由运行时锁或跨线程变更队列维护。

缓存命令的 CPU 数据只需覆盖本帧 Record；命令编码完成后，帧间可以删除 Batch、重建缓存或销毁 Scene，无需等到 Wait。
Wait、槽清理以及 槽内对象的析构/清理回调不得解引用留下的 Scene、Batch 或缓存命令借用。
需要在 Wait 使用的 CPU 数据应保存到槽内对象或 Pipeline 的对应槽状态。
底层 GPU 资源寿命独立于 CPU 命令：外部资源必须保持有效至最后一次引用它的 command allocator 重置，或将 owner 移入 `FrameSlot::Emplace` 保活。

`slot.GetPassDrawList(pass)` 按紧凑 `MeshPassId::Value` 取得槽内列表；不能跨后续列表扩容保存其引用。
`MeshPassFrameDrawList` 包含 `VisibleCommands`、自动绑定到该列表的 `DynamicContext` 以及当前帧本 Pass 的 `PreparedMeshPassIndirectDraws IndirectDraws`，不拥有缓存命令副本。
列表搬移时会同步移动 `IndirectDraws` 并重新绑定 DynamicContext，避免 vector 扩容后继续向旧列表写入或错配间接批次。
Extract 按本帧激活的 Pass 调用 `Scene::GatherVisibleMeshDrawCommands(..., drawList)` 或 `GatherVisiblePrimitives(..., drawList)`，
保存命中缓存命令的只读指针；也可调用 `processor.AddMeshBatch(context, drawList.DynamicContext)` 生成当帧命令。
未请求的 Pass 不进行当帧提取；持久缓存同步仍独立处理全部登记 Pass 的脏 Batch。
Prepare 中的 `SortVisibleCommands()` 先调用 DynamicContext 的 `FinalizeVisibleCommands()`，
完成动态命令指针填充；排序前必须完成全部动态命令生成。`Submit(encoder, submitArgs)` 是 const，只读消费冻结列表。
排序按 SortKey 稳定升序进行；Submit 统一使用可见项的命令指针，并转发 Pass 级参数组与实例流。
不需要这些绑定时可省略 `submitArgs`；提交入口不再接收缓存列表或动态命令数组。
槽清理只清空可见列表、动态命令与动态绑定池并保留容量，不解引用缓存指针；没有缓存命令复制和快照析构。

当前五个阶段仍串行执行，没有独立渲染线程或额外的业务 RenderPass 抽象。

## GPU 资源与批量上传

`GpuBuffer` 和 `GpuTexture` 使用线程安全的侵入式引用计数。`GpuTexture` 的 View 缓存支持并发查询和创建。提交状态的快照与发布由构图和提交线程串行组织。
初始引用数为 0；工厂分配并构造对象，
使用显式 `IntrusivePtr` 构造取得首个强引用。计数基类统一检查溢出、下溢和带引用析构，并在归零时立即销毁对象。
引用计数只表达 CPU 所有权，不等待 GPU；调用方须保活资源至 GPU 使用完成且相关命令存储引用解除。
跨帧持有资源的资源表、待上传请求和拥有参数集的对象保存强引用；需要延迟释放时，把 owner 交给当前槽的
`FrameSlot::Emplace`。几何子分配和材质绑定对象本身的所有权保持各自契约。
runtime 不登记全部资源、不绑定资源域，也不在最后一个引用释放时自动退役。

### 提交边界状态

`GpuBuffer` / `GpuTexture` 的 `GetSubmittedState()` 返回已提交工作按单队列顺序执行至末尾时的整资源交接状态，
包含图末收尾屏障，不一定等于最后一个 Pass 的使用状态。首次相关提交前保存创建初态：DeviceLocal 缓冲为 Common，可映射缓冲为 HostWrite，纹理为 Undefined。
它不表示 GPU 当前执行位置、工作已完成、内容全部初始化或资源可以回收，也不包含跨队列同步和所有权信息。
纹理全部 mip / array layer 使用同一交接状态；局部上传不会因此初始化其他区域。

`CommitSubmittedState(state)` 是公开的元数据更新接口，只能在对应命令实际提交后，按提交顺序发布与命令一致的终态。
它不录制屏障、不提交命令、不等待 GPU，也不通过 fence、版本或队列查询验证调用时点。
直接使用 RHI 的调用者须自己建立状态转换与同步，提交后通过此接口发布，再交给 RDG；状态更新本身不建立 GPU 依赖。
调用和对象生命周期遵守 RHI 的串行化契约。

持久导入在 Build 读取初态快照。依赖前一提交的新图须在前一提交和状态发布之后导入；
取得快照后不能插入使其失效的其他提交。后续发布不会修正已构建图的快照，当前没有提前构图后的状态重算。
被裁剪、失败或放弃而未提交的工作不发布状态；资源保活与回收仍按命令存储和 GPU 完成的独立契约处理。

### 缓冲与持久状态

`GpuBufferDescriptor::Memory` 指定持久缓冲的分配策略。默认 DeviceLocal 自动增加 CopyDestination。HostUpload、DeviceUploadStrict 和 DeviceUploadWithFallback 接受 GPU 只读用途并自动增加 MapWrite。创建新的 DeviceUpload 缓冲或页前，Runtime 检查可直写显存容量和实时预算。当预算已知时，预计用量（当前用量加申请字节数）至多可达预算的 85%。预算未知时按硬件支持尝试分配。Strict 在水位超限或原生创建失败时返回空值，WithFallback 转用 HostUpload。

`GetDesc()` 返回请求策略，`GetMemoryProperties()` 返回实际物理属性，`IsHostVisible()` 表示此包装具有可写映射。
工作线程按 `GetSliceAlignment()` 和 `AlignSliceSize()` 切分互不重叠的区间，使用 `GetMappedData()` 写入。对齐覆盖用途和 HostCoherentAtomSize。所有写入线程完成交接后，由提交线程串行调用 `FlushMappedRange()`。覆写区间前须等待此前使用它的 GPU 工作完成。

`ImportPersistentBorrowed` 借用包装，`ImportPersistentOwned` 在登记时去重持有强引用。DeviceLocal 读取已提交初态并以 Common 收尾。可映射缓冲以 HostWrite 进入和离开图，支持外部预写，也支持通过 `UploadBuffer` 登记图内写入。Compile 要求图内直写区间互不重叠且先于首次 GPU 读取，Prepare 直接填充并刷新目标范围。DeviceLocal 目标通过暂存页和 GPU 拷贝更新。

### 纹理与持久状态

`GpuTexture::Create(device, descriptor)` 创建设备支持的单采样 Device 纹理，`ArrayLayers` 对 3D 表示深度，对 Cube 表示面数。
省略 Usage 时，颜色格式使用 CopyDestination | Resource，深度模板格式使用 DepthStencilRead | DepthStencilWrite。显式 Usage 按传入值创建，`GetDesc()` 返回生效的用途。
具备 Resource 用途的纹理持有覆盖全部 mip／数组层的默认采样视图；纯附件的 `GetDefaultView()` 返回空值。
`GetOrCreateView` 复用同一纹理上维度、格式、规范化子资源范围和用途相同的视图。纹理所有者先释放视图，再释放原生纹理。
`TextureAsset::Create` 接收具备默认采样视图的纹理强引用。重载时保留同一纹理对象属于 Compatible，更换底层对象属于 Breaking。

`ImportPersistentBorrowed` 与 `ImportPersistentOwned` 对纹理采用相同的状态关联与所有权规则。
初态来自 `GetSubmittedState()`（新纹理为 Undefined）。具备 Resource 用途的纹理在图末进入 ShaderRead，其他纹理保持最后使用状态。
普通 CopyPass、自动上传和其他存活 Pass 都由 `CommitSubmittedStates()` 在实际提交后统一发布终态；
Build、Prepare、Record 不修改已提交状态。状态不表示全部区域已初始化，读取区域仍由调用方保证有效。
原生 RHI Import 只借用，不关联 wrapper 状态。拥有式导入在登记时保活，以物理对象去重；
被裁剪的 owner 也保留到槽位安全清理，不在 Compile 触发 GPU 对象析构。

### RDG 上传来源

| 来源 | Build | Prepare | 生命周期 |
|---|---|---|---|
| `RDGUploadSource::BorrowedBytes` | 保存 span，不复制 payload | 一次写入最终映射区间 | 调用方保活到 Prepare 消费结束 |
| `OwnedBytes` | 移入已有 `vector<byte>` | 一次写入最终映射区间，随后释放源存储 | 图接管源所有权 |
| `SnapshotBytes` | 立即复制为 OwnedBytes | 按 OwnedBytes 处理 | 原数据可在调用返回后失效 |
| `Generated` | 保存显式数据和函数指针 | 可选 PrepareLayout 决定实际尺寸，Write 直接填最终区间 | 上下文保持到生成完成 |

源 CPU 所有权与目标 GPU owner 分别表达。Build/Compile 不申请或刷新上传页，
常规 Borrow/Owned/Generate 不复制 payload；Snapshot 是调用方显式选择的付费例外。
被裁剪来源不计算布局、不写映射、不产生 Copy。CPU 消费结束、命令提交、GPU 完成是三个独立时间点。

`UploadBuffer(handle, offset, source)` 声明 Device 目标的 GPU Copy 或 Upload 目标的 HostWrite。
`UploadTexture(handle, region, RDGTextureUploadSource)` 支持单采样颜色纹理的 mip、数组层、立方体面、体纹理深度切片和 BCn 格式块。
region 直接使用 `gpu::TextureRegion2D`；Prepare 确定 staging offset/pitch 后，Record 将它与目标区域组合成 RHI 复制描述。
CPU 源的行距以字节计，RowsPerImage 以块行计。Borrow/Owned 跳过行与切片间填充，Generated 的 WriteContext 提供最终 RowPitch、RowBytes、每片 Rows 和 Depth。
staging 将每片的有效块行连续排列，按设备约束与格式块字节数的公倍数对齐行距与偏移。
上传默认不是图根；必须完成的初始化显式 MarkOutput。裁剪不会将请求自动排回下一帧。

`CreateBuffer(RDGTransientBufferDescriptor)` 创建由瞬态池管理的 DeviceLocal 独立缓冲。
`CreateDynamicBuffer(RDGDynamicBufferDescriptor{MaxSize, Usage, Alignment, Residency}, source)` 声明动态 Upload 切片。`IsDynamicBuffer(handle)` 可识别动态切片，其描述符的 Size 是上界，Memory 是首选内存域。Prepare 解析的切片给出实际大小与底层缓冲，查询底层缓冲可得实际内存域。
Device 缓冲上传与纹理上传的拷贝源都分配在 Staging 页。仅声明 `CopySource` 的动态切片也使用 Staging 页，
可直接交给自定义 CopyPass。声明了管线直读用途的切片使用 Dynamic 页，即使本帧仅作为拷贝源读取。
PrepareLayout 在可见命令固定并排序后执行，返回实际字节数；可建立索引，但不得创建 RHI 资源或生成完整 payload。
Write 接收最终映射区间，不能改变尺寸、图拓扑或登记新上传；生成器之间没有隐式执行依赖。
声明上界、逻辑有效大小和对齐保留大小分别处理；RHI BufferDescriptor::Size 始终是物理 buffer 容量。
实际大小为零时不分配、不 Flush、不 Copy，ResolveBuffer 返回空 slice，消费者跳过绑定及 draw/dispatch；已存活的 Raster Clear 仍执行。

### 每槽上传页

Pipeline 每槽持有独立的 `StagingBufferPool` 和 `DynamicBufferArena`，默认页大小为 1 MiB。Staging 的 HostUpload 页仅声明 MapWrite 和 CopySource，按传输布局对齐，精确保留切片字节数。Dynamic 页额外支持 Vertex、Index、CBuffer、Resource 和 Indirect。RDG 合并用途所需的对齐，常量切片的保留范围覆盖后端描述符的尾部填充。

`DynamicBufferResidency::HostOnly` 只使用 HostUpload 页。默认 PreferDevice 优先复用现有 DeviceUpload 页，空间不足时检查新页容量对应的预算。超限或创建失败时从 HostUpload 页分配。分配不跨页，同帧切片地址稳定。Prepare 按物理页集中刷新修改范围并汇总 GPU 只读状态，Submit 后发布实际终态。

槽完成 GPU 等待、命令存储重置、帧内对象和参数绑定清理后，两个池分别调用 `BeginFrame()`。
每个槽的 `DynamicBufferArena` 按实际分配的 `MemoryType` 保留首张标准大小页，供该域下次分配时复用，直到 `Clear()` 或析构。标准大小取配置页大小，并受设备非零 `MaxBufferSize` 限制。仅分配过超大页的域不会补建标准页。
其余动态页和全部 Staging 页在同一槽连续第八次空闲回收时释放。`GetCapacity()` 返回当前保留的物理页总容量。
`RenderGraphFramePipeline::GetUploadPageUsage(slot)` 分别返回本管线指定槽的 Staging 和 Dynamic 用量与容量。每个池的用量是各页分配游标之和，包含切片间的对齐空隙和切片尾部的保留空间。每次 `Prepare` 在分配前重置该槽的页游标，复用的页继续计入容量。
引用动态切片的参数集须通过槽的 TransientParameterSetPool 获取，或由调用方在槽回收时解除旧绑定、使用前重新绑定。释放页前须先释放关联的 TypedBuffer 视图。

CPU 验证在 `test_rdg.cpp`、`test_mapped_buffer_pools.cpp`；GPU 验证在 `test_rdg_upload_gpu.cpp`，
覆盖上传时序、部分更新、源 pitch、普通 CopyPass 纹理初始化、切片偏移、参数组复用和槽轮转。

## 几何子分配池与持久映射上传

`geometry_buffer_pool.h/.cpp` 提供对称的 VertexBufferPool 与 IndexBufferPool。Create 借用设备并复制布局、属性和调试名前缀；
Allocate 按描述符的 Memory 策略创建 GpuBuffer 页，默认使用 DeviceLocal，池持有页面强引用，空闲区间由 [BestFitAllocator](core-facilities.md#bestfitallocator) 管理。
池与设备必须晚于所有子分配句柄销毁；释放区间前须完成 GPU 使用并重置相关 command allocator，可将旧 mesh owner 移入 FrameSlot::Emplace。
分配零元素或创建页失败返回空值；超过默认页容量的请求创建可容纳整段的大页，不跨页切分一次分配。
`GetPageCount()` 返回已提交页数。`GetCapacityBytes()`、`GetFreeBytes()`、`GetAllocatedBytes()` 分别返回已提交容量、页内可复用空间和活跃区间的字节数，三者满足容量等于空闲加已分配。Release 下均为 O(1)，Debug 的空闲量查询会逐页核账。所有操作串行调用。
`TrimEmptyPages()` 撤销全部整页空闲的提交，返回撤销页数，并释放池持有的页面强引用。存活子分配保留原槽号、缓冲与偏移。
`GetPageIndex()` 返回池内稀疏槽号，空句柄（含移动后的源）返回 0。存活槽号可以大于或等于已提交页数。
分配按槽号优先尝试已提交页，随后重用首个空槽或追加新页，超大请求也可重提交到空槽。

VertexBufferPoolDescriptor 指定 VertexStride、VertexStreamBinding、PageVertexCapacity（默认 150,000）、AdditionalUsage 和布局/属性。
布局必须恰有一个 Vertex 流，binding/stride 与描述符一致，另可有一个 Instance 流；所有布局和属性通过纯几何校验，不依赖 shader Location。
顶点页字节数不超过 uint32 上限；Usage 含 Vertex，DeviceLocal 页另加 CopyDestination。
页保持提交期间，同页句柄的 GetVertexInput 返回同一地址，仅绑定页顶点流，实例流由 MeshPassSubmitArgs::InstanceStream 延迟注入。
撤销顶点页会销毁该页的 VertexInput，即使 GetGpuBuffer() 的外部强引用仍保活缓冲。调用方须在撤销前更新或移除借用该 VertexInput 的 MeshBatch，槽号复用后从新句柄重新获取引用。
VertexBufferSubAllocation 提供 GetBuffer、GetVertexStride、GetFirstVertex、GetVertexCount、GetByteOffset、GetByteSize。

IndexBufferPoolDescriptor 指定 IndexStride（2 或 4，默认 4）、PageIndexCapacity（默认 300,000）与 AdditionalUsage。
索引页 Usage 含 Index，DeviceLocal 页另加 CopyDestination。IndexBufferSubAllocation 提供对应索引范围和字节范围访问器；
GetIndexBufferView 返回 Offset=0 的整页视图，应配合 GetFirstIndex 使用，不能再叠加 GetByteOffset。
构造 MeshBatchElement 时，可设 IndexBufferOffset=0、FirstIndex=GetFirstIndex()+局部索引起点，并显式限定范围不超过该子分配。
两类句柄仅可移动，空句柄不释放区间；读取资源和字节视图要求句柄非空。
两类句柄均提供 `GetGpuBuffer()`，返回所属整页的强引用，用于 RDG 的 ImportPersistentOwned 与 UploadBuffer；
GetBuffer 仍返回绘制所需的原生缓冲。页面强引用不保留子分配区间，不能据此提前释放或复用子分配；
子分配句柄仍须活到相关 GPU 使用与命令引用结束，池仍须晚于子分配句柄销毁。

两池的 AddPassReadAccess 遍历调用时的全部已提交页（含已空闲的保留页），通过 ImportPersistentOwned 保活，从 GpuBuffer 的已提交状态转入声明的读状态（默认分别为 Vertex 和 Index），图结束时恢复该页的持久导入终态。
可传 Vertex | ShaderRead 等合法只读组合（须通过 AdditionalUsage 增加相应用途）；多次导入与上传声明共享同一物理句柄。
读依赖按具体缓冲整页声明。影响该 Pass 所读资源身份的建页、重提交和 Trim 必须先于 AddPassReadAccess 完成。
ExtractMeshPass 可从已导入的同一缓冲继续切分区间。若无法保证已有页有足够的连续区间，应在 ExtractFrameData 中先分配并持有句柄。相同槽号重新提交的缓冲需要新的读声明。

几何上传通过 RDG 的显式来源入口。目标在仅上传帧也恢复持久导入终态，后续帧可直接由两池 AddPassReadAccess 声明读取。
`test_geometry_upload_gpu.cpp` 在 D3D12／Vulkan 上通过 BorrowedBytes 声明索引与顶点上传，覆盖跨页增长、同帧绘制和后续帧绘制。

## RDG 与 RenderGraphFramePipeline

`RenderGraphFramePipeline` 固定 Extract、Prepare、Record、Submit；每槽保存瞬态池、两个上传页池、编译图和命令缓冲引用。
构造参数为 DeviceContext、正槽数及可选页大小；设备和槽数须与 Scheduler 一致。
Extract 调用 ExtractFrameData → SetupGraph → Compile → ExtractMeshPass，仅提取存活且去重的 MeshPass。
ExtractFrameData 保存稳定输入、候选集合和描述信息，不要求生成完整 payload。
Prepare 固定/排序可见命令 → 来源实际布局 → 物化资源 → 分配与填充 → 存活 Pass Prepare → Flush/屏障准备 → 获取命令缓冲。
Record 执行 Begin、编码已准备命令和 End。Submit 将编译图生成的计划交给 `SubmitFrame(FrameSlot&, RDGSubmitPlan&)`，提交成功后发布图状态。默认实现调用 `slot.GetQueue().Submit(plan.Build())`。需要记录帧槽完成值的管线可在重写中追加 signal，提交成功后更新完成值，由 Wait 等待。
未实现 ExtractMeshPass 却声明了存活 mesh pass 时带诊断终止。阶段失败不可恢复，不提供重试或部分提交。

### 构图、访问与裁剪

`RDGBuilder(slot, options)` 与 `DeviceContext::GetRDGConstraints()` 从同一设备能力取得约束。
显式传入 `RDGDeviceConstraints` 时，对齐取与设备要求的最小公倍数，容量与绑定范围取非零上限中的较小值。
构图与 Prepare 共用这组约束，包括缓冲拷贝偏移、纹理行距和填充后的暂存大小。Compile 单次冻结声明并返回 CompiledRDG，失败返回空值。
FullValidation 控制访问用途/Pass 兼容性完整诊断，句柄、依赖、范围、溢出及调度必需的结构检查始终保留；RHI validation 独立配置。

RDGData 与 CompiledRDG 通过 slot.Emplace 创建并登记清理；声明表、嵌套访问/依赖列表、导入表、
调度结果、物理状态表、切片、屏障计划、`RDGUserPass::Name`（`std::pmr::string`）与 Pass 回调包装对象均使用 slot.GetMemoryResource() 的线性 resource。
`AddRasterPass` / `AddComputePass` / `AddCopyPass` 在槽线性内存中直接构造移动独占的 RAII 类型擦除回调对象（`ErasedPassCallback`），构造完成即托管析构责任，若后续登记分配失败也会立即析构捕获的 `PassData`；稳态构图零全局堆分配与零原子引用计数开销。
RDGExecution 同样从该 resource 分配，由 CompiledRDG 的独占 owner 持有。
Compile 的局部索引数组、ready 集合，以及 Prepare 的尺寸/用途数组和上传区间 map 使用独立 scratch pool，
作用域结束即归还单元；其内容不被 Record 借用。嵌在图记录中的 Readers、依赖列表与导入表仍随图回收。
Build/Compile 使用槽的 CPU 存储，不创建原生 GPU 资源；Prepare 必须使用构图时的同一个槽。

槽回收先销毁 CompiledRDG 的 PassData，随后销毁执行数据与帧内 view，再销毁 RDGData，释放来源和 GPU owner。
未 Compile、Compile 失败和裁剪后的图也持续由槽保活至清理，不依赖 Builder 的栈生命周期。
图容器每帧重建，先析构再统一重置线性存储；临时 pool 复用阶段局部容器归还的单元。
PMR 容器的普通复制构造会选择默认资源，跨域复制必须显式传入目标资源；当前 RDG 以原域移动和显式构造维持归属。

自动 Upload/HostWrite 使用独立的连续声明表，每条记录内嵌一个目标访问；图节点通过索引关联声明，保持独立裁剪和时序。
用户 Pass 的附件、回调和访问表（`detail::RDGAccessTable`）放在另一张表中：单 Pass 独立访问资源数不超过 16 项时直接线性扫描 `Accesses` 且保持 `AccessBuckets` 为空（零桶分配）；超过 16 项时在图所属帧线性内存（`MemoryResource`）上按需建立 2 的幂长度（初始 64 桶、装载率不超过 1/2）的纯 `slot` 开放寻址哈希桶，探测命中时回查 `Accesses[slot]` 完整 `(Texture, Index)` 对，用于 `AddAccess` 查重合并与 `ResolveTextureView` 访问核验。来源以 Borrow、Owned、Generated 互斥存储，不为 Borrow 额外分配 owner。
`RDGUploadSource::Generated` 内置 64 字节对齐至 `alignof(std::max_align_t)` 的小对象缓冲（SBO）：当生成器对象满足 `sizeof <= 64 && alignof <= alignof(std::max_align_t) && is_nothrow_move_constructible` 时原位内联构造与移动，不产生堆分配；不满足条件时自动回退至堆分配以保持 `RDGUploadSource` 的 `noexcept` 移动契约；消费后释放，裁剪后的来源随帧清理释放。
实际大小、staging 和上传屏障记录仅在 Prepare 为存活上传分配，不参与 Build 的节点初始化与搬移。

`AddRasterPass(name, data, record, prepare)` 返回声明 Builder，Compute/Copy 同形。
PassData 显式持有，Record 是 `bool(const T&, Context&) noexcept` 函数指针；可选 Prepare 是 `bool(T&, RDGPrepareContext&) noexcept`。
不接受捕获执行闭包。PrepareContext 可以访问槽以准备绑定；RecordContext 只提供 encoder/command、准备好的资源与视图、
frame/slot id 和只读可见命令，不提供可变 GetSlot。
借用输入保持到消费完成，PassData 析构不得访问已结束的外部借用。

资源分为导入的 `gpu::Texture*` / `Buffer*` 与按完整描述符声明的瞬态资源。导入需要真实 `initialState`，可选 `finalState`；
同一物理对象重复导入会合并句柄，初态和显式终态冲突时编译失败。GPU 对象的 owner 仍由调用方持有，必须活到引用它的命令存储重置；
外部附件 View 可通过 `WriteColor` / `SetDepthStencil` 的 `explicitView` 传入，目标必须与导入资源一致。
状态与依赖按整个资源追踪，Texture barrier 使用 `SubresourceRange::AllSub()`。光栅附件的子资源范围决定视图及写入覆盖范围。

`AddRasterPass`、`AddComputePass`、`AddCopyPass` 分别提供 Encoder 或 CommandBuffer Context。
调用方只可查询当前 Pass 已声明的资源；`ResolveView` 在状态与 View 用途不匹配时返回空。
Raster 通过 `WriteColor` / `SetDepthStencil` 声明附件，颜色槽从 0 连续排列，至少有一个附件，各附件有效尺寸、层数和采样数一致。
自动附件视图默认选择首个 mip 和全部数组层。调用方可通过 `RDGAttachmentSlice`（固定单 mip 与数组层范围）或 `gpu::SubresourceRange` 重载指定单个 mip 与连续数组层，或传入导入纹理的显式视图。`SetRenderArea` 的矩形相对于所选 mip，省略时覆盖附件全幅。Prepare 回调可用 `RDGTextureViewDescriptor` 获取子视图，Record 使用同一描述符查找已准备的视图。
Prepare 为所有存活 `ShaderRead` 与 `UnorderedAccess` 访问预建默认视图：默认 `TextureViewUsage::Resource` 使用 `SubresourceRange::All` 全 mip 范围以 O(1) 命中持久 `GpuTexture::GetDefaultView()`；`UnorderedAccess` 与各类附件（`RenderTarget`、`Depth*`）默认视图严格保持单 mip `1u`。
`AddClearColor` 和 `AddClearDepthStencil` 的整纹理重载要求单 mip。显式范围（`RDGAttachmentSlice` 或 `gpu::SubresourceRange`）重载可清除一个 mip 的选定数组层与矩形。局部写入保留先前生产者；首次局部 Clear 可从 Undefined 开始。混合深度模板格式只清一个 aspect 时，另一个采用 Load 和 Store 并保留上一次写入。每张图的 `GetBlackboard()` 使用帧槽内存，`Emplace<T>` 每种类型只可成功一次，`Get<T>` 可查询已登记的数据。
只读深度/模板 aspect 必须 Load / Store；丢弃 Store 使用 `StoreAction::Discard`。
Buffer 的导入边界、读取状态与瞬态池 LastState 使用 `gpu::BufferStates`。只读状态组合逐位要求相应 Usage：
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
无依赖 Pass 的 GPU 执行顺序可能变化。通过 `GetHandle()` 或上传、清屏的返回值得到当前图的 Pass 句柄。空上传返回无效句柄。`AddOrderDependency(before, after)` 约束两个存活节点按声明先后执行。`AddOrderBarrier` 只可对最新节点登记一次，裁剪后该屏障位于更早与更晚的全部存活节点之间。对带外部副作用的节点使用 `NeverCull()` 保活。例如，可用首尾两个带屏障的 CopyPass 重置、解析外部查询池，并对中间打时间戳的 Pass 同时声明保活与屏障。
HostWrite 保留逻辑依赖但不产生 GPU 组。连续就绪 Upload 合批，用户 CopyPass 保持独立。
就绪上传使用有序集合，不逐上传遍历全图；Raster / Compute 按以下规则合并：

- Raster 的颜色槽、附件句柄、归一化子资源范围、显式 View 指针、有效渲染矩形和深度状态必须相同。上一子 Pass 的附件 Store 必须为 Store，下一子 Pass 的 Load 必须为 Load。深度/模板只检查纹理实际具有的 aspect。
- 组内共享附件的同状态访问按绘制顺序保序，省略中间附件屏障。其他资源若已被组内任一前序子 Pass 访问，两次访问必须都是相同状态的只读，否则切组。
- Compute 使用相同的非附件规则：共享只读可以合并，写后读、读后写、连续写或状态变化必须切组；检查覆盖整组，不能只比较最后一个子 Pass。
- 后续子 Pass 首次使用的资源可以把转换屏障前移到组入口；其生产者已在组外完成，组内前序子 Pass 不访问它。每组全部首次访问的屏障批量下发一次，然后开启 Encoder，Encoder 内不下发屏障。
- Raster 使用首 Pass 的 Load / Clear 和末 Pass 的 Store，`AllowUavWrites` 为全组 UAV 写访问的逻辑或；每个逻辑子 Pass 回调前重置 `SetStencilReference(0)`。
- Raster / Compute 每组只开关一次 Encoder，但每个逻辑子 Pass 保留独立 debug group，并且 Context 只允许解析当前子 Pass 声明的资源。回调失败时立即停止后续回调并关闭 Encoder。

被裁剪的 MeshPass 不触发当帧提取或排序；其独占资源不向池借出，其上传不申请页面或执行 Copy。
使用 Scene 时，调用方在 RunFrame 前显式同步当前全部 registrations，不受当帧图裁剪影响。

### 上传与资源状态

Device 上传的 staging 在 Prepare 写好，Copy 仍在图内对应位置执行，不跨越中间消费者。
上传组执行所有成员；已知不重叠区间省略组内 WAW，重叠写按顺序同步。纹理区间优化使用包含行间空隙的保守范围。
每个 Upload execution group 只开启一个名为 `RDG UploadBatch` 的 GPU debug/timing scope，包含组内 Copy 与重叠写屏障，
不包含组入口屏障；逐上传节点保留动态 CPU zone。空数据不执行 Copy，组内录制失败时先关闭 scope 再返回失败。
用户 CopyPass 仍使用自己的名称；HostWrite 不生成 GPU scope。底层 debug group 与 GPU timing 的对应关系见
[RHI 调试标记](render-rhi.md#调试标记与-gpu-profiling)。
依赖仍是整资源版本链，消费者未声明区间时不保证裁剪同一资源内无用的部分写入。
Upload 目标的 HostWrite 必须全部位于 GPU 使用前，且声明区间不重叠；Compile 按资源汇总并排序区间检查。
需要两次 GPU 消费间更新时使用不同动态切片或 Device 复制路径。

`ResolveBuffer(handle)` 返回 RDGBufferSlice，含 Buffer、Offset、逻辑 Size。独立资源 Offset 为零；
所有 Copy、Vertex/Index、ShaderParameterSet、动态 offset、Indirect 使用者须加上 slice.Offset。
不提供丢弃基址的 Buffer 引用解析重载。未初始化固定资源不能用零字节写入冒充初始化。

`CompiledRDG::Prepare(slot, transientPool, stagingPool, dynamicArena)`、`Record(command)` 和 `CommitSubmittedStates()` 各执行一次。
Prepare 和 Record 返回 false 时不可继续录制或提交，提交状态保持不变。Record 要求命令缓冲已 Begin，调用者负责 End。`RenderGraphFramePipeline` 在 Begin 返回时若设备已丢失，直接退出。Begin 成功后，若图录制返回时设备已丢失，管线跳过帧尾时间戳、查询解析和读回屏障，并在返回前调用一次命令缓冲的 End。
Prepare 中的内部 `detail::RDGStateTracker` 借用物理资源，按初态/池 LastState 推导并冻结屏障和预期终态；Record 只编码。
tracker 是 RDGExecution 的成员，物理状态表使用稠密资源索引和图的 PMR resource，不接管资源 owner，也不维护全局注册表。
它集中组织 HostWrite / 上传页的开始屏障、组入口与上传成员间屏障、结束转换和外部 release；
共享上传页由所属页池汇总只读用途并提供页屏障，逻辑切片不各自转换该物理页。
组内规划沿用访问列表和组标记，不为每组遍历全部资源；已知上传区间优化不改变整资源依赖模型。
初态为可写状态时保守设置 PendingWrite，同状态 UAV、写后读、附件与外部转移继续生成必要同步。
实际队列提交正常返回后调用 CommitSubmittedStates，统一发布持久 wrapper、瞬态池和上传页终态，不等待 fence。
持久 wrapper 通过公开 CommitSubmittedState 接口更新；tracker 自身只保存图内预期状态，不提前修改持久状态。
直接使用 CompiledRDG 的调用者同样负责这一边界；被裁剪或未提交的图不发布状态。

### 导入资源与队列同步

`ImportTexture` 和 `ImportBuffer` 的简式重载接受资源、初态和可选终态。`RDGTextureImportDescriptor` / `RDGBufferImportDescriptor` 还接受 `WaitFence`、`SignalFence` 和 `Ownership`。资源上的 fence 只随存活资源进入提交。图级 `AddWaitFence` 和 `AddSignalFence` 始终进入提交。队列等待约束 GPU 访问，Prepare 中写入映射内存所需的 CPU 同步由调用方负责。

`ImportBackBuffer(frame)` 借用已获取的帧，将后缓冲标为输出，终态固定为 Present。调用方保有该帧并负责提交后的 Present 或依契约 Discard。每个窗口分别导入，非空 acquire / present 对象自动进入提交。未绘制的帧仍会执行必要的终态转换，并在 BottomOfPipe 消费 acquire。同一帧可按相同初态重复导入。无效帧、与普通纹理导入混用、不同帧复用同一同步对象均使 Compile 失败。

Prepare 规划屏障时，按存活执行组中第一次实际 GPU 访问推导等待阶段，跳过 HostWrite 和实际大小为零的上传。颜色、深度模板、复制、顶点或索引、间接参数分别等待对应硬件阶段。着色器访问按 Compute 或 Raster 的顶点与像素阶段推导，同一缓冲的多个只读用途取并集。普通导入资源仅有收尾屏障，或需要外部所有权 acquire 时，资源 fence 等待使用 AllCommands。

同一 fence 的等待合并为最大值及所有等待阶段的并集。相同 signal 值只提交一次。同一图内为同一 fence 声明不同 signal 值、空 fence 目标或保留的最大 signal 值都会使 Compile 失败。重复导入同一资源时已声明的所有权、初态、终态及同方向的 fence 须一致。

独立使用编译图时，Record 成功后调用 `CreateSubmitPlan(cmdBuffers)`。计划使用帧槽内存，可在提交前追加 fence，其目标、阶段和 signal 值须有效且与已有声明兼容。`Build()` 返回借用计划的描述符，须在计划修改、销毁或帧槽回收前使用。调用方在队列提交成功后调用 `CommitSubmittedStates()`。

`RDGExternalOwnership{Acquire, Release}` 控制外部所有权屏障。首次组访问合并状态转换并发出 Acquire，只有 MarkOutput 的资源也会在图末消费待处理的 Acquire。终态转换后单独发出 Release，未指定终态时保持当前状态。同状态交接也会发出屏障。跨进程句柄传递和外部生产者的同步由调用方负责。

### 每槽瞬态资源池

`RDGTransientResourcePool` 独占 Texture、Buffer 及其 View owner。进入槽前必须已完成 Wait 和 allocator Reset，随后 `BeginFrame()` 重置上一轮借出标记，并按 8 帧滑动窗口淘汰连续 8 帧未借用的瞬态资源（销毁资源前先从缓存中擦除并释放关联视图）。
Texture 按描述符全部字段匹配复用，保留真实 `LastState`；Buffer 复用支持容量向上分桶（`<= 4096 B` 按 256 B 向上取整，`> 4096 B` 按 `std::bit_ceil` 向上取整，非零 `MaxBufferSize` 时受其上限约束），查找同用途/内存/提示下位于 `[desc.Size, bucketSize]` 内的最小空闲缓冲；未命中时先按分桶容量创建，若分桶分配失败则回退精确请求尺寸 `desc.Size` 重试，同时 RDG 逻辑切片 `RDGBufferSlice::Size` 始终保留请求大小。
同一执行内相同描述符的多次资源申请仍获得不同物理对象，不做别名复用。
新瞬态纹理和 DeviceLocal 缓冲的初态为 Undefined。实际 Submit 后记录真实终态，不能在 Fence 完成后假定纹理自动回到 Undefined。
`Clear()` / 析构前所有引用它们的命令存储必须重置，View 先于资源销毁。
瞬态纹理的 View 缓存在池内，持久 `GpuTexture` 的 View 保存在纹理的自有缓存中。裸导入纹理的自动 View 由本帧编译图持有，槽回收或 `WaitIdle()` 后即可销毁或重建交换链。
直接使用池的 `AcquireTextureView` 时，目标资源必须活到该缓存 View 释放。

当前不提供子资源状态拆分、异步队列、资源内存别名、跨槽共享池、显式资源版本节点或自动 PSO/参数组构建。

教学样例的 `MeshDrawingFramePipeline` 使用同一流程，默认双槽连续六帧。
Pipeline 按槽持有可复用参数组、读回缓冲和独立 fence；动态实例/常量由 RDG Create 分配最终 Upload 切片。
唯一实例生成器在 PrepareLayout 遍历存活 Pass 的可见并集，建立 candidate → 紧凑实例映射并改写帧内 FirstInstance，
Write 每个可见实例只计算一次矩阵；持久 MeshDrawCommand 不变。常量直接生成，两份显式来源分别供普通 Pass 与描边共享。
空实例不分配和 draw，已存活附件 Clear 和 readback 保留。颜色/深度附件由每槽池持有；几何、shader、布局与 PSO 由外部共享。
Wait 只等待该槽的 fence；主循环不逐帧等待整个队列，结束时 `WaitIdle()` 等待并重置全部槽，再保存最后一帧的图片。
Frame 0 后更新绿色材质，调用方以当前 registrations 显式同步增量命令，设置管线输入后调用 Frame 1 的 `RunFrame()`；已有在途帧的参数与附件保持独立。

### 异步读回与 GPU 计时

`AsyncReadbackQueue` 将一个提交队列的 fence 值与读回请求关联。构图前用 `ReserveTexture` 预留 ReadBack 缓冲，构图时以 `EnqueueReserved` 声明源纹理、mip 与数组层。`EnqueueTexture` 可在构图时一次完成这两步。一条队列同时至多服务一张待提交图，默认最多有 4096 个活跃请求，活跃 ID 唯一。放弃图时调用 `CancelUnsubmitted()` 回收已预留和已入图的请求，提交成功后用 `MarkSubmitted` 关联已入图请求。

两个入图方法也接受 `gpu::TextureRegion2D`，可选择单采样颜色纹理的局部矩形或体纹理切片。预留计划须与该区域的宽高深和格式匹配。只指定 mip 和数组层时，预留计划须匹配整个子资源。`Poll` 按块行去掉设备行距填充，返回紧凑字节。

队列独占暂存缓冲。构图至执行期间须保持队列有效，销毁前等待所有已提交读回的 GPU 工作完成，例如调用 `FrameScheduler::WaitIdle()`。取消与提交均推进批次代次。同一请求 ID 可在取消后重用，旧图再调用 Prepare 会失败。放弃的图应停止录制与提交。

`ReserveBuffer` 按字节数预留，`EnqueueReservedBuffer` 在逻辑缓冲的指定偏移登记拷贝，`EnqueueBuffer` 合并两步。源须声明 CopySource 并满足拷贝偏移对齐。Prepare 解析实际源和目标切片，动态源过短时跳过拷贝，屏障仍会使用暂存缓冲。结果的 Texture.Width 为零，PackedByteSize 为请求字节数。

| `Poll` 状态 | 含义及请求生命周期 |
|---|---|
| Pending | 请求尚未提交或目标 Fence 未完成。 |
| Ready | 空目标可反复查询格式和紧凑字节数。有足够空间时，先 Invalidate，再按块行复制并回收请求。 |
| SourceOutOfRange | 动态源实际切片过短。达到该批 Fence 后报告并回收请求及暂存缓冲。 |
| DestinationTooSmall | 目标容量不足，保留请求供重试。 |
| InvalidRequest | ID 不存在，或缓存操作被拒绝。 |
| DeviceLost | 设备已丢失，或 Invalidate 首次报告丢失。 |

`SetGpuProfiling` 在 BeginFrame 后设置当前槽的采样档位和 UserSequence，成功提交时固化。帧槽清理会丢弃未提交设置，复用槽时默认为 Off 和序号 0。FrameOnly 记录图首尾，FrameAndPasses 额外记录各 GPU Pass。查询在同一命令缓冲内解析至 ReadBack 并转换至 HostRead。

每次成功提交只触发一次 `OnFrameSlotRetired(slot, snapshot)`。快照携带 UserSequence 和 Valid、Off、Unavailable、AllocationFailed 或 DeviceLost。健康设备依据时间戳有效位数及 tick 周期换算纳秒，丢失后跳过读取。宿主可在下次 Prepare 前通过 `GetCompletedGpuTiming` 读取有效快照。

## Scene 绘制输入

| 类型 | 头文件 | 当前内容 |
|---|---|---|
| `Scene` | `scene.h` | 私有 Primitive/Batch/Material SparseSet、关联账本、材质反向索引与 Pass 缓存 |
| `PrimitiveSceneInfo` | `mesh_batch.h` | 本地到世界变换、`Aabb LocalBounds`、MeshBatch 句柄列表 |
| `MeshBatch` | `mesh_batch.h` | 一个材质句柄、一个顶点输入引用、拓扑与内联 Elements |
| `MeshBatchElement` | `mesh_batch.h` | 借用的索引缓冲、索引范围和默认实例范围 |
| `VertexInput` | `vertex_input.h` | 顶点布局、属性和借用的顶点缓冲流 |
| `MeshSection` | `mesh_batch.h` | 几何资源的 submesh 分段：材质槽和索引范围；Scene 不存储它 |
| `MaterialRenderProxy` | `material.h` | 持有 MaterialAsset 句柄，拥有已解析的 CPU 参数快照；见下文材质核心 |

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
| `MaterialRenderProxy` | `MaterialProxyHandle = SceneHandle<MaterialRenderProxy>` |

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

支持有索引实例绘制与无索引实例绘制（`MeshBatchElement::IndexBuffer == nullptr`）：
- 有索引绘制：`IndexCount` 是索引数量，三角形列表的一个三角形对应三个索引；`IndexBufferOffset` 是字节偏移，`IndexStride` 为 2 或 4；`FirstIndex` 相对此偏移，单位是索引；`VertexOffset` 是有符号的基础顶点偏移。
- 无索引绘制：`IndexBuffer` 为空指针，`IndexBufferOffset` 与 `VertexOffset` 必须为 0，`IndexCount` / `FirstIndex` 分别表示 `VertexCount` / `FirstVertex`；同一 `MeshBatch` / `MeshDrawCommand` 内禁止混用有索引与无索引元素。`ValidateMeshBatchElements`（由 `Scene::ValidateBatch` 与 Debug 下的动态 `BuildMeshDrawCommands` 共用）按 `VertexInput` 校验所有 `Vertex` 流容量以及未延迟覆盖的 `Instance` 流容量；最终提交与校验另根据驻留的 `StreamLayouts` 校验生效顶点流与实例流（含 `MeshPassSubmitArgs::InstanceStream` 和可见实例区间）不越界。
每个 Batch 最多包含 32 个 Element 条目（包含空条目）；超过上限的输入被拒绝。Element 与可见命令的 `FirstInstance` 默认为 0、`InstanceCount` 默认为 1；可见命令可覆盖该实例范围。逐实例数据由应用通过实例流提供；CPU 自动合批间接绘制（MDI）见下文[校验与录制](#校验与录制)，LOD 选择与 GPU 计数的 `DrawIndirectCount` 尚未实现。

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

例如，已由外部准备好 `VertexInput cubeVertices` 和 `gpu::Buffer& cubeIndices` 后，
可用下面的数据组织一个顶面金属、其余五面木头的立方体。假定索引缓冲按木头 30 个索引、金属 6 个索引排列，
索引宽度为 4 字节，立方体本地坐标范围为 [-0.5, 0.5]。木头和金属实例由调用方准备：

```cpp
#include <radray/runtime/scene.h>
#include <radray/runtime/mesh_batch.h>

// scene must stop referencing cubeVertices and cubeIndices before they are destroyed.
void AddCube(Scene& scene, const AssetManager& assets, const VertexInput& cubeVertices, gpu::Buffer& cubeIndices,
             const MaterialInstance& wood, const MaterialInstance& metal) {
    MaterialRenderProxy woodProxy, metalProxy;
    if (!woodProxy.Update(assets, wood)) return;
    if (!metalProxy.Update(assets, metal)) return;
    const array<MaterialProxyHandle, 2> materials{
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

## Shader 资产、布局与缓存

runtime 消费 shader 模块的拥有型布局与变体接口；描述、验证和序列化契约见
[Shader 布局](shader-pipeline.md#shader-布局)。runtime 负责 GPU 资产、布局注册表与材质绑定。

### ShaderAsset 与离线加载

`ShaderAsset` 拥有不可变 family 描述、变体接口、入口名及当前设备后端的 Shader owner。
`Create(device, ShaderAssetDesc)` 接受内存描述与 stage 字节码；`Load(device, indexPath)` 读取 reflect CLI
生成的版本 1 `.index.json`，以索引目录为基准加载 `.layout.bin` 和当前后端的独立 stage 文件。
另一个后端的 stage 文件不必部署。非法索引、重复 stage、缺少当前后端必需 stage 或创建 Shader 失败时不发布资产。
离线加载不调用 DXC。内存根源码通过 `ShaderAsset::Compile` 和调用方提供的 Client 编译为设备后端的 Shader。离线产物入口见 [家族反射](shader-pipeline.md#家族反射)。

ShaderAsset 不引用布局注册表，也不保存原生 PipelineLayout/BindingGroupLayout。
`GetLayoutDesc()` 返回共享的 `shader::ShaderPipelineLayoutDesc`。`GetVariantInterface(variant)` 按变体索引和当前后端
返回 `shader::ShaderVariantTargetInterfaceDesc` 的可空只读指针。`GetStage` 返回包含 Shader 指针和入口视图的 `gpu::ShaderEntry`。
`FindBinding`、`FindPushConstant`、`FindBufferMember` 查询 CPU 描述；前两者分别返回可选的 `radray::ShaderBindingLocation`
和 `radray::ShaderPushConstantLocation`，其 `ActiveOnDevice` 表示当前后端是否有映射。
这两个查询结果由 `radray/runtime/shader_asset.h` 提供。迁移原 `shader::` 查询结果时使用 runtime 类型；
绑定查询与材质资源目标共用 `shader::ShaderBindingReference`，通过 `Reference.GroupSlot` 和
`Reference.BindingId` 读取组槽和组内 ID。
借用的指针和入口字符串不得跨所属资产的搬移或销毁保存。

`ShaderAsset::ClassifyReload` 拒绝跨 Device 替换。仅字节码或顶点输入变化时返回 `Compatible`。
共享布局、反射类型或成员、变体数量或顺序、名称、后端可用性、活跃绑定、阶段或入口变化时返回 `Breaking`。
持有 PSO 的渲染方按当前 Shader 和材质版本重新准备管线。样例校验所选变体的活跃绑定、顶点输入和 Pass 常量 ABI，包括 ViewProjection、LocalScale 和 Color 的类型、大小及字节偏移。

### PsoCache

`PsoCache` 在构造时借用固定的 AssetManager、设备、布局注册表与可选原生缓存。图形和计算键包含 shader handle、变体与可选共享布局，图形键还保存顶点布局和渲染状态。
一次资产修订内相同键返回同一个 PSO。新修订可立即创建新 PSO，旧版本由 `RetireInvalidated(slot)` 交给帧槽，在最后一次 GPU 使用完成后先释放 PSO，再释放配套布局。

### PipelineLayoutRegistry

`PipelineLayoutRegistry` 显式借用一台 Device，单线程串行调用，无全局状态。
创建 PSO 的调用方使用 `registry.Acquire(shader.GetLayoutDesc())` 取得 `GpuPipelineLayout` 的共享句柄，
通过 `GetLayout()` 借用原生布局，通过 `GetGroupLayout(groupSlot)` 独立持有组布局并创建参数集；Shader 加载不触发该操作。

组 key 按 BindingId 排序，只包含当前后端映射的 BindingDescriptor，不含名称、类型树或块成员信息。
完整布局 key 为排序后的 `(GroupSlot, 组布局对象)` 与 push constant 描述。
两级缓存均先按内容哈希分桶，再比较全部字段；同内容跨 family 复用同一组对象和完整布局。
Stages 属于 key，不同阶段可见性会生成不同条目。共享组的 CPU 参数 ABI 兼容性由调用方保证。
GroupSlot 独立于 space/set；过滤后空组不创建。

注册表的两个索引只持有弱引用。完整布局持有各组的强引用，最后一个使用者释放时销毁相应原生对象。
完整布局先销毁原生 pipeline layout，再释放组。查找时清理所在桶的过期项，每级累计到阈值时清扫所有桶，
阈值初始为 64，清扫后取 64 与存活项数两倍中的较大值。
句柄可以晚于注册表销毁。调用方须保证布局覆盖借用它的 PSO、参数集、已录制命令及 GPU 在途使用，
提交完成由 fence 确认。Device 必须晚于注册表和所有句柄销毁。

### 资产登记与生命周期

`AssetManager` 在 owner 线程管理已经构造好的资产。离线 Shader 先由 `ShaderAsset::Load` 读取索引，再用 `Emplace` 登记。
`MakeAssetId` 对逻辑名或规范化的绝对 URI 做 64 位哈希，零 ID 表示匿名。`EmplaceWithHandle` 在调用工厂前预留槽位和 ID，
工厂可用传入的句柄建立资产自身标识。此时同名登记会失败，预留槽位在成功发布后才能查询。工厂失败会撤销预留并推进代次。

弱句柄用两个 32 位字段保存索引和代次，仅在所属 manager 中解析。`Find<T>(id)` 和 `AsHandle<T>(handle)` 校验存活、代次和类型，
`Acquire` 增加强引用。`AssetRef<T>` 在 64 位平台占 16 字节，保活逻辑资产。`Get` 借用当前版本，提交后须重新获取。
`GetRevision` 自首次发布的 1 开始，失效句柄返回 0。`GetDependents` 返回直接依赖此槽位的父句柄，修改仓库后须重新查询该视图。

#### 事务提交

`AssetTransaction` 绑定一个 manager。`StageReplace` 暂存已准备的值，`StageBuild` 暂存构建函数和直接依赖。
`StageMaterial` 复制材质描述，在提交时根据最终 Shader 同时解析程序与参数映射。暂存失败会保留首条诊断并使整笔事务失败。
`Commit` 校验目标仍有引用、依赖仍存活，并在拟提交的依赖图上验环，再按子先父后运行构建函数。
构建函数通过 `tx.Get` 读取已声明依赖的最终值，未声明的依赖或尚未构建的候选返回空。
构建与 `ClassifyReload` 回调期间只能读取仓库，变更引用计数、修改事务或重入提交会触发 `RADRAY_ABORT`。

`Compatible` 允许单独提交。`Breaking` 要求当前全部存活的直接父资产同批暂存，可通过 `GetDependents` 和 `AsHandle<T>` 找到它们。
`Rejected`、缺失父资产、构建失败或依赖环都会返回失败诊断并保留线上版本。成功时仓库按旧图父先子后将旧值挂入 `AssetGarbageBatch`，
更新反向依赖并递增各槽位版本，再按新图子先父后返回 `AssetReloadEvent`。
依赖持有子槽位，父资产可以保存句柄和反射值拷贝。若父资产借用子资产内部的原生对象，替换该对象的子资产须分类为 `Breaking`。

#### 回收与帧同步

引用归零的槽位进入回收链，回收前仍可用 `Acquire` 复活。`CollectGarbage(batch)` 先撤销槽位身份，再将当前版本移入调用方的批次。
清理批次时先析构各资产，再释放它持有的子槽位引用。无参 `CollectGarbage()` 同步排空由此产生的级联回收。
manager 的寿命须覆盖外部强引用和非空退役批次，析构时会检查两者。Device 的寿命须覆盖其 GPU 对象。

单队列样例在 `BeginFrame()` 返回后、场景提取前提交。当有事务或 `HasPendingGarbage()` 为真时，先在当前 `FrameSlot`
登记 `AssetGarbageBatch`，再回收、提交，最后登记换下的 PSO 或视图。复用帧槽时先等待 GPU 并重置命令存储，
再逆序销毁派生对象与退役资产。跨帧使用 `TextureView` 的缓存同时持有底层资产的强引用。发生替换时，旧视图须在同帧退役。
新视图创建失败期间跳过受影响的绘制，按线上版本重试。

样例为每套 PSO 记录 Shader 和材质已应用的版本，以及现有管线连续收到 `Compatible` 事件的最新版本。
重建失败时，只有这两个兼容版本都追平线上版本才能继续使用旧 PSO。遇到 `Breaking` 或漏掉事件时退役旧管线，
暂停相关 draw，后续帧持续按版本重试。每次暂停或成功换入都在 `Extract` 前标脏关联材质代理并同步驻留命令，
使 `MeshDrawCommand::Pipeline` 与当前 PSO 一致。

### 顶点接口解析

VertexInput 保存几何 layout、semantic、格式、步长与偏移，不保存 shader Location。
`ValidateVertexInputLayout` 校验纯几何，不要求 GPU streams 已上传。
`ResolveVertexInputState(meshInput, shaderInputs, output)` 按 Semantic/SemanticIndex 匹配，
要求浮点/整数类别及分量数相等，归一化格式属于浮点类别；只输出变体所需属性，使用其后端签名的 Location。
合法重叠属性允许存在。结果经过 RHI 顶点状态校验，失败保留输出；semantic 视图借用 mesh 字符串，
直到 PSO 创建完成 mesh 都须保持存活且不搬移。完整 ValidateVertexInput 继续验证 stream 范围、用途和延迟实例流。
`PrepareMaterialVertexInput(interface, meshInput, output)` 只接受带 vertex 阶段的反射接口，零顶点输入合法。

## 材质核心

`MaterialAsset` 合并材质性质、具名程序、参数声明与默认值，并拥有每个所引用 Shader family 的参数映射。
实例和渲染快照使用资产句柄，无独立共享 Schema 身份。

| 类型 | 职责 |
|---|---|
| `MaterialAsset` | 不可变材质性质、Shader 程序、参数定义与预解析映射 |
| `MaterialInstance` | MaterialAsset 句柄和按声明索引保存的可选覆盖，值对象 |
| `MaterialRenderProxy` | MaterialAsset 句柄和显式同步的完整参数快照 |

### 定义、实例与同步

`CreateMaterial(assets, desc, id)` 校验非空且唯一的参数/程序名、Shader 句柄、变体和入口。
每个被程序引用的 ShaderAsset 必须恰好提供一份 `MaterialFamilyParameters`，显式指定常量块名与参数名子集。
子集允许为空；未知/重复参数、额外或缺失 family、与 shader 类型不兼容的映射拒绝创建。
不同 family 可使用不同子集，未参与某个 family 的参数可以继续用于 CPU 逻辑。

`MaterialParameterValue` 支持纯值类型常量（`float`、`int32_t`、`uint32_t`、`array<float, 2>`、`array<float, 3>`、`array<float, 4>`、`array<int32_t, 4>`、`array<uint32_t, 4>`、`array<float, 16>`）以及资源快照（可空 `TextureView*` 和可空 `Sampler*`）。
其中 `array<float, 16>` 在 CPU 侧统一约定按列主序（Column-Major，`m[column * 4 + row]`，与 `Eigen::Matrix4f::data()` 及 HLSL 默认 `column_major` 一致；历史计划稿中曾拟定的行主序约定已统一调整为列主序）存储。
参数名称区分大小写；索引属于该资产声明顺序。资源参数只复制指针，不增加引用计数；
外部 owner 必须覆盖相关 CPU/GPU 使用期间。

`MaterialInstance` 通过 `GetParameter(assets, index)` 读取覆盖或默认值；
`SetParameter(assets, index, value)` 拒绝越界和类型改变，`ResetParameter(assets, index)` 恢复默认值。
实例没有父链、静态开关或运行时 shader 编译。

默认 Proxy 没有有效资产。`Update(assets, handle)` 复制默认值，`Update(assets, instance)` 复制解析后的参数；
失败保留原快照。实例后续变化必须再次显式同步。材质热替换保持参数数量、顺序、名称和类型，保留实例按索引保存的覆盖。
仅默认值或绑定映射变化属于 `Compatible`，程序、标签或渲染状态变化属于 `Breaking`。
样例记录各代理已应用的材质版本，发现版本变化时用原实例重新 `Update`，再调用 `Scene::UpdateMaterial`，漏收事件也能更新继承的默认值。
Proxy 不保留实例地址，其参数引用不能跨 Update、对象移动或 Scene 的 SparseSet 搬移保存。
`Scene::UpdateMaterial(MaterialProxyHandle, proxy)` 标脏引用该快照的所有 Batch；
同步和读取由调用方串行组织，没有自动 GPU 上传。
需要材质性质的 Processor 借用 `const AssetManager&`，通过 Proxy 的句柄解析资产。

### 材质参数映射

`MaterialParameterBindingMap` 在创建资产时预解析，只保存数值位置、期望 variant 索引、矩阵转置标记和材质句柄身份，不借用 ShaderAsset、
布局注册表或原生布局。通过 `MaterialAsset::GetBindingMap(shaderHandle)` 查询。
常量子集只映射到指定 CBuffer/PushConstant；参数名作为成员路径，可定位单个数组元素。
标量与向量要求种类（`Scalar` / `Vector`）、基础类型（`Float` / `SignedInteger` / `UnsignedInteger`）、行列维度与字节大小精确匹配且无内部 padding；`array<float, 16>` 要求 `Matrix`、`Float`、`4x4`、`MatrixStride == 16`、`Size == 64` 且无内部 padding，并按 `member->RowMajor` 记录 `TransposeMatrix4x4`。
写入 `array<float, 16>` 时，目标着色器为默认 `column_major` 则直接拷贝 64 字节，目标为显式 `row_major` 则按 `(column, row) -> row * 16 + column * 4` 自动转置写入，使同一 `MaterialInstance` / `MaterialRenderProxy` 可供不同矩阵主序的 Shader family 消费。
TextureView 只映射 Texture/RWTexture，Sampler 只映射动态 Sampler，数量必须为 1；资源数组和 immutable sampler 拒绝。
资源目标在当前后端不存在时标记 inactive 并跳过；包含常量参数但常量块在当前后端不可用时创建失败。

WriteConstants / BindResources 在 Debug 下断言 Proxy 的 8 字节材质句柄一致；Release 由调用方保证身份。
WriteConstants 先检查全部类型、块大小和范围，再写入映射字节，保留其他字节。
BindResources 先检查该组全部适用参数和非空资源，再调用 Set；参数集所属组由调用方保证。
底层 Set 失败可能已经写入部分资源，调用方应重置或丢弃该集。
映射不执行 Reset、Flush、提交或 GPU 等待，family 超集中其他必需绑定仍由调用方填写。

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
| `Blend` | 空，关闭混合；有值时使用 `gpu::BlendState` |
| `ColorWriteMask` | `ColorWrite::All`；单组材质默认值，具体颜色附件由 Pass 决定 |
| `DepthTestEnable` / `DepthWriteEnable` | 均为 true |
| `DepthCompare` / `DepthBias` | `LessEqual` / 零偏移 |
| `Stencil` | 空，关闭模板测试；有值时使用 `gpu::StencilState` |
| `StencilReference` | `uint8_t`，默认 0，正反面共用；通过 RHI 动态设置 |

没有直接保存整个 RHI `DepthStencilState`，因为其中的附件格式属于本次 Pass。
附件格式、采样数、清除值、几何拓扑和资源同步仍由渲染流程提供。
这些属性随 MaterialAsset 创建后不可修改；MaterialInstance 目前只覆盖参数。

`MaterialShaderProgram` 保存程序名、ShaderAsset 句柄、VariantIndex 和拥有的 VS/PS 入口名称，PS 空字符串表示纯顶点程序。
创建材质时校验当前后端接口和入口，不保存裸 Shader 或 PipelineLayout 指针。
具名程序列表只保存当前可用程序，不表示材质自动参加同名 Pass。

PSO 还需要顶点输入、附件格式和经 Pass 确定的最终状态。runtime 材质不持有 PSO，不安排 Pass，
可使用独立布局和参数映射明确参数的 GPU 位置，但不自动创建参数集或执行上传。
`MeshDrawCommand` 保存 PSO、索引几何、模板参考值与绑定池句柄；顶点流、静态参数组和命令级 push constants 由列表的绑定池持有。
修改材质参数不要求重新创建 MaterialAsset/PSO；样例通过 `Scene::UpdateMaterial` 标脏关联缓存，
Processor 重建时读取新 Proxy，将 `BaseColor * Brightness` 的 RGB 和原 alpha 复制进命令的 16 字节颜色常量。
相机与局部缩放使用 Pass 级参数组，物体变换使用 Pass 级延迟实例流，均在提交时提供。
这些每帧数据变化无需重建命令；材质的稳定 GPU uniform buffer 更新路径尚未实现。

样例的 M1～M5 注释串起资源准备、定义/实例、Proxy 同步、PSO 准备和参数适配。
八个实例使用三份参数声明相同的 MaterialAsset：普通不透明、双面描边不透明和 alpha 混合透明材质；
共享加载的 shader，由样例准备各 Pass 的六个 PSO。具体场景、筛选矩阵和状态见
[教学样例](../guide/build-test.md#mesh-drawing-pipeline-教学样例)。
Depth Pass 沿用材质剔除与深度偏移，显式选择 Less、开启深度测试/写入、关闭模板；
Color Pass 沿用材质颜色、深度和模板状态。双面材质使用 Always/Replace 写入模板参考值 7。
Outline Pass 筛选描边标签，以放大几何和 NotEqual 7 绘制金色轮廓，不改写深度/模板；
Transparent Pass 筛选透明标签，使用材质 alpha 混合、深度测试和关闭深度写入的状态。
附件采用 D32_FLOAT_S8_UINT，参考值由持久 `MeshDrawCommand` 保存并在 Draw 前录制；生命周期见 [RHI 模板参考值](render-rhi.md#模板参考值)。
颜色默认值/覆盖和 Brightness 都由 Processor 从 Proxy 获取，准备阶段解析参数名称，构建命令时使用索引。
Processor 同时检查各自要求的材质标签与样例 `BatchPassPolicy`。没有材质图、编译/静态变体、fallback、纹理绑定演示、
uniform expression 求值或 GPU 参数缓存。

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
表及集合的读写由调用方串行组织；MaterialAsset 发布后不修改标签。
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
| `MeshVertexBufferBindings` | 全部 VB 与 `StreamLayouts`（`Binding`、`ArrayStride`、`StepMode`）均按 Binding 升序正规化，连同可选 InstanceStreamBinding 按内容驻留 |
| `MeshDrawCommandPayload` | 参数组、完整动态 offset 与自持有 PushConstants 按内容驻留 |
| `MeshDrawCommandElements` | ExtraElements、ElementInstances、入库预验的 ValidatedExtraMask / StructureValid 及预计算合并信息；逐命令独占 |

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

默认状态键按 32 位位域组合：`PipelineLayout` 4 位（高 4 位，空为 0）、`Pipeline` 9 位（空为 0）、`Payload` 9 位（空为 0）、
`Geometry`（`VertexBuffers` 与 `IndexBuffer` 组合哈希）8 位（均空为 0）与 `StencilReference` 2 位（0 映射 0，非零折叠至 1～3）。
各字段均由对象指针或驻留内容哈希直接派生，不依赖所属 `MeshDrawCommandBindingStorage` 的局部编号，因此缓存与动态命令混排时相同状态产出一致的 `StateSortKey`，
同 `PipelineLayout` 的 PSO 优先聚簇，且桶折叠不污染邻接位域。桶碰撞只影响聚类，不影响正确性。`CustomStateSortKey` 可显式替换默认状态键，包括指定 0。

缓存路径通过 `VisibleMeshBatch::VisibilitySortKey` 提供当帧键；动态路径使用 `MeshBatchContext::VisibilitySortKey`。
两者使用相同合成函数。深度键由调用方编码，按无符号升序排序；DepthFirst 不自动计算距离或反转深度。
`MeshPassRegistration::SortMode` 定义 Scene Pass 模式，Scheduler 在 Extract 前设置当前槽的对应列表。
手动混合缓存/动态命令时须先设置 MeshPassFrameDrawList 的模式，再生成命令；已有输出不能中途更改模式。

`GatherVisibleMeshDrawCommands` 检查 Batch 代次与独立脏位，直接读取 Pass 热表及命令数组，不访问冷 Batch。
Gather 的脏项检查不能检测所有漏同步：缺失 pass cache 会缺少输出，processor/registration 变化也不一定使已有 batch 变脏；调用方仍须使用当前 registrations 显式同步。
`ElementMask & ValidElementMask` 非零时仅追加一条可见项；零掩码、空元素位、不存在的位均不输出。
无效或脏 Batch 回滚本次追加；未注册、撤销或被 Processor 筛选跳过的 Pass 合法产生空输出。
`GatherVisiblePrimitives` 与 `GatherVisibleMeshDrawCommands` 在外层单次提取 Pass 缓存后共用内层单 Batch 收集路径（先验句柄存活与脏标记，再检查槽表边界跳过未注册 Pass 或尾部被过滤 Batch）；剔除与 LOD 由应用负责。
`FirstInstanceOverride` 与 `InstanceCountOverride` 独立覆盖所选全部元素；未覆盖的部分保留元素默认范围。

可见项保存排序键、const 命令指针、实例标量、元素掩码和两个 UseElement 来源标记。
标记为 true 且存在 ElementInstances 时读取逐元素值，否则读取可见项标量；手写可见项默认使用标量。
缓存指针只在帧内 Scene 借用期间有效；Scene 变化后必须重新 Gather。
同键保持输入顺序，Batch 内按源下标递增绘制；需要独立材质或排序位置的 Element 应拆成不同 Batch。

`MeshPassFrameDrawList::SortVisibleCommands` 对不超过 64 项使用原地稳定插入排序；更大数组先单趟同时计算 `differing` 差异字节掩码与非递减有序性，若已有序则直接返回（零 `_scratchCommands` 扩容与零基数搬运）；否则复用常驻 scratch 执行八位一轮的稳定 LSD 基数排序，跳过全数组无差异的字节。当活跃差异字节为奇数趟时，先将 `_scratchCommands` 容量对齐至至少 `VisibleCommands.capacity()` 后在末尾与 `VisibleCommands` 做 O(1) 缓冲交换（消除整表回拷并保留预留容量，排序前持有的 `VisibleCommands` 底层指针/span 视图在排序后须重新获取）；偶数趟结果直接落回 `VisibleCommands`，`_scratchCommands` 仅按当帧实际元素数分配而不复制大预留容量。容量足够时排序不分配临时堆缓冲。
Sort 和 Submit 都先完成动态指针发布。动态 Context 在 AddCommand 时追加元数据，
`FinalizeVisibleCommands` 在全部生成后填入稳定命令指针并冻结列表；之后不能增删或改变命令存储。
下一帧清空 VisibleCommands、`IndirectDraws` 并 Clear Context，保留容量。动态池由 unique_ptr 保持移动时身份稳定；
列表移动后同步移动 `IndirectDraws` 并重新绑定 Context 的可见数组，已发布指针与间接批次均保持有效。

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

局部状态缓存在每次 Submit 内去重 PSO、VB、IB、StencilReference、参数组与 PushConstants。
VB 先比较池地址和 VertexBuffersIndex，相同则跳过组解析；变化后按槽位 0～31 定长表比较完整 View，
更高槽位直接绑定。连续脏槽位使用原数组 subspan 下发，遇到未变槽位、间隙或被 Pass 实例流覆盖时截断。
Payload 同样比较池地址和 PayloadIndex，相等时跳过解析、参数组比较与 PushConstants 写入；变化后仅下发变更的参数组，且 PushConstants 按 `(PushConstantId, Offset, Data)` 去重（无 PushConstants 的命令不清空已写入缓存）。
PSO 切换会清空 VB 缓存并重绑实例流与命令 VB；若新旧 PSO 的 `PipelineState::GetLayout()` 返回相同非空 `PipelineLayout`，则保留 Pass/命令参数组与 PushConstants 缓存，否则清空并重绑；IB 和模板参考值始终保留。
零实例 Draw 跳过；零掩码项不绑定状态。无索引元素（`IndexBuffer.Target == nullptr`）不调用 `BindIndexBuffer`（保留当前 IB 缓存状态），在完成连续单实例合并后调用 `encoder.Draw`。空命令指针或 SetPushConstants 失败返回 false，调用方须放弃录制。

CPU 自动合批间接绘制（MDI）提供 `CountMeshPassIndirectDraws`、`BuildMeshPassIndirectDraws` 与 `SubmitMeshPassIndirectDraws`，配合 `MeshPassFrameDrawList::IndirectDraws`（`PreparedMeshPassIndirectDraws`）与两路 `BufferUse::Indirect` 动态缓冲使用：

- `CountMeshPassIndirectDraws` 与 `BuildMeshPassIndirectDraws` 共用与 `SubmitMeshDrawCommands` 一致的元素展开、实例数覆盖与连续单实例合并内核；解引用前通过 `MeshDrawCommandElements` 入库预验的 `ValidatedExtraMask` / `StructureValid` 执行 O(1) 掩码与结构一致性校验，失败返回 false（合法 0 条绘制返回 true 且计数为 0）；单元素命令（`ExtraElementsIndex == kInvalidBindingIndex`）与全掩码无合并多元素命令走展开/绘制快轨（`SubmitMeshDrawCommands` 仍完整复用 `IndexBuffer` 状态缓存）。
- `BuildMeshPassIndirectDraws` 入口无条件清空输出，仅合并相邻且 `Pipeline`、`StencilReference`、`(BindingStorage, VertexBuffersIndex)`、`(BindingStorage, PayloadIndex)` 及绘制类型/实际 `IndexBufferView` 完全一致的展开元素为同一 `MeshPassIndirectBatch`，并累加 `HasNonZeroFirstInstance`；比较顶点槽和载荷槽时，各自的双侧无效槽均忽略 `BindingStorage`；任一有效槽都要求索引及存储对象一致。
- `SubmitMeshPassIndirectDraws` 在首次绑定或绘制前检查 `encoder.IsValid()`、所有批次的命令引用、非零 `ArgumentCount` 和 CPU 参数数组范围。Debug 还校验全部可见命令与 `submitArgs`（含无索引顶点流与实例流容量）。
  索引与非索引参数分别取实际走间接绘制的批次中最大的 `FirstArgument + ArgumentCount`，据此校验各自的切片容量。预检还要求切片偏移按 4 字节对齐、整个切片位于缓冲范围内。每个底层参数缓冲至多查询一次有效性，并通过一次 `GetDesc()` 确认 `BufferUse::Indirect` 和大小，两路切片共用缓冲时复用查询结果。
  任一预检失败返回 false，编码器收到 0 条绑定与 0 条绘制。录制中途 `SetPushConstants` 失败立即返回 false，调用方须放弃录制。
- 设备不支持间接绘制或 `MaxDrawIndirectCount == 0` 时，构图阶段可省略两路间接参数缓冲而直接调用 `SubmitMeshDrawCommands`；直接调用 `SubmitMeshPassIndirectDraws` 时按 `deviceCanDrawIndirect && (!batch.HasNonZeroFirstInstance || capabilities.Features.IndirectFirstInstance)` 逐批决策：满足条件时按 `MaxDrawIndirectCount` 分块并累加 `slice.Offset + (FirstArgument + consumed) * stride` 调用 `DrawIndexedIndirect` / `DrawIndirect`，否则该批次逐条回退直接调用 `DrawIndexed` / `Draw`。

RHI 公共接口不暴露 PSO 布局和已写参数资源，runtime 无法检查布局兼容、必需参数、动态 offset 设备对齐或读取索引内容。
这些条件由应用与 RHI 校验层保证；Pass 开闭、viewport/scissor、附件、barrier、提交和等待由调用方组织。
测试位于 `test_mesh_draw_command.cpp`、`test_mesh_pass_processor.cpp`、`test_frame_slot.cpp`、`test_scene.cpp`、
`test_frame_scheduler.cpp`、`test_rdg.cpp`、`test_mdi_gpu.cpp` 与 `test_runtime_allocations.cpp`（关闭 mimalloc 时独立 exe），观测状态仅由 fixture/fake 持有。

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

DeviceContext 也提供接收 `shared_ptr<gpu::Device>` 与非空 Direct 队列指针的构造函数。
调用方须保证队列属于传入设备，且底层环境寿命满足设备要求。
DeviceContext 持有共享 Device owner，队列指针从设备借用；`GetDevice` / `GetQueue` 返回借用引用。
构造函数统一尝试通过 `_device->CreatePipelineCache()` 创建可选的设备级 `gpu::PipelineCache`（后端返回空则保持空值），并通过 `GetPipelineCache()` 与 `GetPipelineCacheData()` 暴露供轻量管线复用与持久化。
`SupportsIndirect` 实时读取 RHI 的 `IndirectDraw` 和 `IndirectFirstInstance` 能力，不依赖后端内部头。

## 生命周期与失败

Environment 必须晚于 DeviceContext 及所有依赖它的 GPU 资源销毁。应用应先完成 GPU 工作、释放资源，
再销毁 DeviceContext，最后销毁 Environment。DeviceContext 析构先等待其 Direct 队列，随后显式释放内部 `_pipelineCache`，最后释放 Device owner（避免独占最后一个 Device 引用时管线缓存析构访问已销毁设备）。
如果外部仍持有同一 Device 的共享引用，Environment 也必须继续存活。

环境销毁前，调用方须结束所有使用它的线程。`IsOwnerThread()` 可供宿主识别创建线程。独立资源创建与 Fence 等待遵守 [RHI 并发契约](render-rhi.md)。
应用可使用上述多槽帧循环，或直接通过 RHI 组织 Pass、录制、同步和提交；帧资源释放由 Slot 的对象清理链承担，
共享旧资源可将 owner 移入当前 Slot 的 `Emplace` 保活；跨 Scheduler 或跨队列的通用退役仍由应用组织。

可失败工厂返回 `Nullable<unique_ptr<T>>`，失败处记录日志，调用方检查返回值；不通过接口传递错误字符串。
Vulkan instance 和 D3D12 factory 的底层环境约束仍由 RHI 管理。
