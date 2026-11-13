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
| `gpu_resource.h` | `GpuBuffer`、`GpuTexture`、资源描述与 `ImportPersistent` 重载 | 有 |
| `upload_arena.h` | `UploadArena`、按槽分页映射、对齐分配和聚合 Flush | 有 |
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
`BeginFrame() -> FrameSlot&` 开启下一槽，在场景更新和上传准备之前完成等待、allocator Reset、帧数据清理和退役清理。
开启后重复调用直接返回同一槽，不重复等待、析构帧对象或释放准备期登记的退役对象。
`GetCurrentSlot()` 仅在 BeginFrame 至 Submit 返回之间返回该槽，其他时间为空。
准备过程未调用 RunFrame 时，下一次 BeginFrame 保留本轮准备；WaitIdle 则清理并取消准备，保留下一槽索引与帧编号。
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

`RunFrame` 返回 `void`，正常返回只表示 Pipeline 已完成本帧录制与提交，不保证异步工作已完成。新槽和已回收的槽不调用 Wait；
在途标记表示存在尚未由 Scheduler 回收的 Submit；即使没有提交或底层已经完成，也会在复用前调用一次 Wait。
`WaitIdle()` 先等待所有在途槽，再对**全部槽**调用 `ResetCommandStorage()`，
解除 command allocator 所有 epoch 引用。随后关闭准备窗口，按帧从旧到新对各槽调用 `ResetFrameData()`，
清空 CPU 数据并析构登记的对象；已开始但尚未提交的当前槽最后清理。
这一顺序使旧帧的绑定和 view 先于移交到较新帧的旧 owner 销毁。同槽依赖仍须遵守下述登记顺序。
不能逐槽完成等待、命令重置和对象清理，否则可能释放仍被其他槽旧录制引用的 Buffer；
它保留当前索引，重复调用不会重复等待已回收提交。Scheduler 析构先执行 WaitIdle，
再销毁全部 Slot，最后销毁 Pipeline，确保 Pipeline 拥有的资源晚于命令存储释放。
Slot 自身不持有 fence 或完成值，Reset 和析构均不等待。
所有调用和析构遵守 [RHI 串行调用与命令存储契约](render-rhi.md#命令录制存储)，不可并发或重入。

### 帧内分配与资源释放

每个 Slot 直接拥有 `std::pmr::monotonic_buffer_resource`、一条对象清理链和一个 `std::pmr::unsynchronized_pool_resource`。
`GetMemoryResource()` 返回线性 resource，供帧对象和 RDG 跨阶段数据使用；
`GetScratchMemoryResource()` 返回临时 pool，供 Compile／Prepare 的局部工作容器使用。
线性存储的初始缓冲为 128 KiB，首次使用时分配；回收保留初始缓冲，归还额外块。
二者只供本槽串行 CPU 阶段使用，不提供并发保证，也不跨槽共享。默认直接使用槽内 `HeapMemoryResource`；
`FrameScheduler::Create` 可传入自定义 memoryUpstream，借用的上游必须晚于调度器销毁。

调用方在当前槽 BeginFrame 后至 Submit 返回前，用 `slot.Emplace<T>(args...)` 创建帧内对象。
构造参数原样转发，对象地址与对齐保持有效直到槽回收，调用方不得手动析构或 delete 返回的对象。
平凡可析构对象只分配本体；非平凡对象和清理节点合并分配，构造成功后才挂入清理链，按构造完成的逆序析构。
分配和构造异常原样传播，失败对象不登记，其存储留到槽回收；成功的嵌套对象仍由该槽清理。
对象析构必须 noexcept；构造和清理期间不得重入槽回收或调度器。

转移 `unique_ptr` 所有权即可延迟释放 GPU 资源，例如已成功创建的 `unique_ptr<render::Buffer> buffer`：

```cpp
auto* owner = slot.Emplace<unique_ptr<render::Buffer>>(std::move(buffer));
auto* borrowedBuffer = owner->get();
```

后续录制可借用 borrowedBuffer，owner 在槽回收时析构。槽内 owner 默认只供该槽使用；
若其他槽也引用它，必须在 owner 槽再次复用前调用上述 WaitIdle，或由外部 owner／各使用槽的共享引用保活。
shared_ptr 可同样保活共享资源；自定义清理通过 Emplace 移入 MakeScopeGuard 的结果。
有依赖的资源应先登记被依赖者、后登记引用者，或归入同一个 RAII 对象。

回收先 Reset command allocator，解除全部旧录制引用，再清空各 Pass 的可见/动态命令并统一析构登记对象。
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
`MeshPassFrameDrawList` 包含 `VisibleCommands` 和自动绑定到该列表的 `DynamicContext`，不拥有缓存命令副本。
列表搬移时会重新绑定 DynamicContext，避免 vector 扩容后继续向旧列表写入。
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

`gpu_resource.h/.cpp` 集中定义 `GpuBuffer` 和 `GpuTexture`，两者分别继承非原子的 `RefCountedBase<T>`。
初始引用数为 0；工厂分配并构造对象，
使用显式 `IntrusivePtr` 构造取得首个强引用。计数基类统一检查溢出、下溢和带引用析构，并在归零时立即销毁对象。
引用计数只表达 CPU 所有权，不等待 GPU；调用方须保活资源至 GPU 使用完成且相关命令存储引用解除。
跨帧持有资源的资源表、待上传请求和拥有参数集的对象保存强引用；需要延迟释放时，把 owner 交给当前槽的
`FrameSlot::Emplace`。几何子分配和材质绑定对象本身的所有权保持各自契约。
runtime 不登记全部资源、不绑定资源域，也不在最后一个引用释放时自动退役。

### 提交边界状态

`GpuBuffer` / `GpuTexture` 的 `GetSubmittedState()` 返回已提交工作按单队列顺序执行至末尾时的整资源交接状态，
包含图末收尾屏障，不一定等于最后一个 Pass 的使用状态。首次相关提交前保存创建初态：buffer 为 Common，texture 为 Undefined。
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

`GpuBuffer::Create(device, descriptor)` 创建固定 Device 内存的缓冲。`GpuBufferDescriptor` 包含 Size 和 Usage，
Usage 默认 Resource，原生用途自动增加 CopyDestination；支持 vertex、index、constant、shader resource、UAV、indirect 和 copy 用途。
零容量、超出设备已知 MaxBufferSize、映射用途或未知用途位返回空值；原生创建失败也返回空值。
`GetDesc` 返回创建时的描述，`GetBuffer` 借用原生缓冲；设备须晚于缓冲销毁。

`ImportPersistentBorrowed(graph, buffer)` 借用 wrapper；`ImportPersistentOwned(graph, owner)` 立即去重持有强引用。
两者读取 `GetSubmittedState()` 初态并以 Common 收尾。同一图中的持久导入都使用这些入口，
具体 Pass 声明 CopySource、Vertex、Index、ShaderRead 等实际用途，RDG 负责图内转换并恢复 Common。
上传只初始化指定区间，调用方须先初始化随后读取的内容；该包装不提供映射、自动扩容或默认 shader 参数绑定。

### 纹理与持久状态

`GpuTexture::Create(device, descriptor)` 借用设备，验证格式和设备能力，再依次创建纹理与覆盖全部 mip／数组层的默认采样 view。
描述包含 Dim、Width、Height、ArrayLayers、MipLevels、Format；仅支持 RHI 区域上传允许的非压缩 color 2D／2DArray。
内存固定 Device、单采样、用途固定 CopyDestination | Resource，不提供通用 RT／UAV 纹理。
全部创建成功后才交付对象；销毁时 view 先于 texture，设备须晚于纹理销毁。`GetTexture`、`GetDefaultView`、`GetDesc` 提供只读访问入口。

`ImportPersistentBorrowed` 与 `ImportPersistentOwned` 对纹理采用相同的状态关联与所有权规则。
初态来自 `GetSubmittedState()`（新纹理为 Undefined），图末恢复 ShaderRead。
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

源 CPU 所有权与目标 GPU owner 分别表达。Build/Compile 不申请、Map 或 Flush 上传页，
常规 Borrow/Owned/Generate 不复制 payload；Snapshot 是调用方显式选择的付费例外。
被裁剪来源不计算布局、不写映射、不产生 Copy。CPU 消费结束、命令提交、GPU 完成是三个独立时间点。

`UploadBuffer(handle, offset, source)` 声明 Device 目标的 GPU Copy 或 Upload 目标的 HostWrite。
`UploadTexture(handle, region, RDGTextureUploadSource)` 支持非压缩颜色 2D/2DArray 的 mip、array layer 和局部区域。
region 直接使用 `render::TextureRegion2D`；Prepare 确定 staging offset/pitch 后，Record 将它与目标区域组合成 RHI 复制描述。
CPU 源 pitch 与 staging pitch 独立；Borrow/Owned 逐行只复制有效像素，Generated 的 WriteContext 提供最终 RowPitch、RowBytes、Rows。
源 pitch 不需要满足设备对齐，staging 的 pitch/placement 按设备约束与像素大小的公倍数对齐。
上传默认不是图根；必须完成的初始化显式 MarkOutput。裁剪不会将请求自动排回下一帧。

`CreateBuffer(RDGDynamicBufferDescriptor{MaxSize, Usage, Alignment}, source)` 声明动态 Upload 切片。
PrepareLayout 在可见命令固定并排序后执行，返回实际字节数；可建立索引，但不得创建 RHI 资源或生成完整 payload。
Write 接收最终映射区间，不能改变尺寸、图拓扑或登记新上传；生成器之间没有隐式执行依赖。
声明上界、逻辑有效大小和对齐保留大小分别处理；RHI BufferDescriptor::Size 始终是物理 buffer 容量。
实际大小为零时不分配、不 Flush、不 Copy，ResolveBuffer 返回空 slice，消费者跳过绑定及 draw/dispatch；已存活的 Raster Clear 仍执行。

### 每槽 UploadArena

Pipeline 每槽独占 UploadArena，页首次存活分配时才创建并持久 Map；页大小默认 1 MiB，可由 Pipeline 构造参数指定。
页直接持有 `render::Buffer`，由 Arena 创建 Upload 内存并设置 MapWrite、PersistentMap，保存映射地址和容量，集中调用 FlushMappedRange。
Reset 保留映射；Clear 和析构先 Unmap 再释放页。新增页创建或映射失败时，已有页及返回的切片保持有效。
一次分配不跨页，空间不足追加页，本帧已使用的 buffer、offset、写指针不搬移，不复制旧页。
页同时考虑 copy、CBuffer、storage/texel、index 对齐；CBuffer 的物理保留范围覆盖后端描述符 padding。
GPU 只读共享页，Prepare 汇总实际读取用途并集，集中 Flush 后按物理页生成同步，不逐逻辑切片反复转换。
页不支持 UAV 或 CopyDestination。已提交页状态同样只在 Submit 后发布。

槽完成 Wait、命令存储 Reset、帧内对象清理后才能 Reset 页游标。稳定参数组可继续引用缓存页，
页面或范围改变时在 Prepare 更新；引用帧内 view 的参数组须在 view 销毁前解除绑定或销毁。
图持有的 PassData 先销毁，再释放帧内 view，最后释放底层 owner。
UploadArena 保留峰值页；GetCapacity 表示保留容量，不是有效数据量。Clear 要求 GPU、命令和参数组引用均已解除，不自动缩页。

CPU 验证在 `test_rdg.cpp`、`test_upload_arena.cpp`；GPU 验证在 `test_rdg_upload_gpu.cpp`，
覆盖上传时序、部分更新、源 pitch、普通 CopyPass 纹理初始化、切片偏移、参数组复用和槽轮转。

## 几何子分配池与持久映射上传

`geometry_buffer_pool.h/.cpp` 提供对称的 VertexBufferPool 与 IndexBufferPool。Create 借用设备并复制布局、属性和调试名前缀；
Allocate 才按需创建 GpuBuffer Device 页，池持有页面强引用，空闲区间由 [BestFitAllocator](core-facilities.md#bestfitallocator) 管理。
池与设备必须晚于所有子分配句柄销毁；释放区间前须完成 GPU 使用并重置相关 command allocator，可将旧 mesh owner 移入 FrameSlot::Emplace。
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
两类句柄均提供 `GetGpuBuffer()`，返回所属整页的强引用，用于 RDG 的 ImportPersistentOwned 与 UploadBuffer；
GetBuffer 仍返回绘制所需的原生缓冲。页面强引用不保留子分配区间，不能据此提前释放或复用子分配；
子分配句柄仍须活到相关 GPU 使用与命令引用结束，池仍须晚于子分配句柄销毁。

两池的 AddPassReadAccess 遍历保留页，以 ImportPersistentOwned 保活并关联 Common 边界状态，声明读取，默认状态分别为 Vertex 和 Index。
可传 Vertex | ShaderRead 等合法只读组合（须通过 AdditionalUsage 增加相应用途）；多次导入与上传声明共享同一物理句柄。
依赖以整页表达，不需要逐 Primitive 收集几何资源；未使用的页也会参加存活 Pass 的整页状态转换。

几何上传通过 RDG 的显式来源入口。目标在仅上传帧也恢复 Common，后续帧可直接由两池 AddPassReadAccess 声明读取。
`test_geometry_upload_gpu.cpp` 在 D3D12／Vulkan 上通过 BorrowedBytes 声明索引与顶点上传，覆盖跨页增长、同帧绘制和后续帧绘制。

## RDG 与 RenderGraphFramePipeline

`RenderGraphFramePipeline` 固定 Extract、Prepare、Record、Submit；每槽保存瞬态池、UploadArena、编译图和命令缓冲引用。
构造参数为 DeviceContext、正槽数及可选页大小；设备和槽数须与 Scheduler 一致。
Extract 调用 ExtractFrameData → SetupGraph → Compile → ExtractMeshPass，仅提取存活且去重的 MeshPass。
ExtractFrameData 保存稳定输入、候选集合和描述信息，不要求生成完整 payload。
Prepare 固定/排序可见命令 → 来源实际布局 → 物化资源 → 分配与填充 → 存活 Pass Prepare → Flush/屏障准备 → 获取命令缓冲。
Record 只 Begin、编码已准备命令和 End。Submit 调用派生的 SubmitFrame，返回后发布图状态；派生类仍实现 Wait。
未实现 ExtractMeshPass 却声明了存活 mesh pass 时带诊断终止。阶段失败不可恢复，不提供重试或部分提交。

### 构图、访问与裁剪

`RDGBuilder(slot, constraints, options)` 借用已 BeginFrame 的槽保存图；constraints 可由 DeviceContext::GetRDGConstraints() 提供，
只包含不可变设备约束，不持有设备 owner。Compile 单次冻结声明并返回 CompiledRDG；失败返回空值。
FullValidation 控制访问用途/Pass 兼容性完整诊断，句柄、依赖、范围、溢出及调度必需的结构检查始终保留；RHI validation 独立配置。

RDGData 与 CompiledRDG 通过 slot.Emplace 创建并登记清理；声明表、嵌套访问/依赖列表、导入表、
调度结果、物理状态表、切片及屏障计划使用 std::pmr 容器，显式传入 slot.GetMemoryResource() 的线性 resource。
RDGExecution 同样从该 resource 分配，由 CompiledRDG 的独占 owner 持有。
Compile 的局部索引数组、ready 集合，以及 Prepare 的尺寸/用途数组和上传区间 map 使用独立 scratch pool，
作用域结束即归还单元；其内容不被 Record 借用。嵌在图记录中的 Readers、依赖列表与导入表仍随图回收。
Build/Compile 使用槽的 CPU 存储，不创建原生 GPU 资源；Prepare 必须使用构图时的同一个槽。

槽回收先销毁 CompiledRDG 的 PassData，随后销毁执行数据与帧内 view，再销毁 RDGData，释放来源和 GPU owner。
未 Compile、Compile 失败和裁剪后的图也持续由槽保活至清理，不依赖 Builder 的栈生命周期。
图容器每帧重建，先析构再统一重置线性存储；临时 pool 复用阶段局部容器归还的单元。
PMR 容器的普通复制构造会选择默认资源，跨域复制必须显式传入目标资源；当前 RDG 以原域移动和显式构造维持归属。

自动 Upload/HostWrite 使用独立的连续声明表，每条记录内嵌一个目标访问；图节点通过索引关联声明，保持独立裁剪和时序。
用户 Pass 的附件、回调和访问列表放在另一张表中。来源以 Borrow、Owned、Generated 互斥存储，不为 Borrow 额外分配 owner。
Generated 的 Context 使用带类型擦除 deleter 的独占 owner，保留具体上下文的析构方式；上下文仍单独堆分配，消费后释放，裁剪后的来源随帧清理释放。
实际大小、staging 和上传屏障记录仅在 Prepare 为存活上传分配，不参与 Build 的节点初始化与搬移。

`AddRasterPass(name, data, record, prepare)` 返回声明 Builder，Compute/Copy 同形。
PassData 显式持有，Record 是 `bool(const T&, Context&) noexcept` 函数指针；可选 Prepare 是 `bool(T&, RDGPrepareContext&) noexcept`。
不接受捕获执行闭包。PrepareContext 可以访问槽以准备绑定；RecordContext 只提供 encoder/command、准备好的资源与视图、
frame/slot id 和只读可见命令，不提供可变 GetSlot。
借用输入保持到消费完成，PassData 析构不得访问已结束的外部借用。

资源分为导入的 `render::Texture*` / `Buffer*` 与按完整描述符声明的瞬态资源。导入需要真实 `initialState`，可选 `finalState`；
同一物理对象重复导入会合并句柄，初态和显式终态冲突时编译失败。GPU 对象的 owner 仍由调用方持有，必须活到引用它的命令存储重置；
外部附件 View 可通过 `WriteColor` / `SetDepthStencil` 的 `explicitView` 传入，目标必须与导入资源一致。
状态与依赖按整个资源追踪，Texture barrier 使用 `SubresourceRange::AllSub()`，Pass 访问接口不接受子资源范围。

`AddRasterPass`、`AddComputePass`、`AddCopyPass` 分别提供 Encoder 或 CommandBuffer Context。
调用方只可查询当前 Pass 已声明的资源；`ResolveView` 在状态与 View 用途不匹配时返回空。
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
HostWrite 保留逻辑依赖但不产生 GPU 组。连续就绪 Upload 合批，用户 CopyPass 保持独立。
就绪上传使用有序集合，不逐上传遍历全图；Raster / Compute 按以下规则合并：

- Raster 的颜色槽、附件句柄、显式 View 指针和深度状态必须相同；上一子 Pass 的附件 Store 必须为 Store，下一子 Pass 的 Load 必须为 Load。深度/模板只检查纹理实际具有的 aspect。
- 组内共享附件的同状态访问按绘制顺序保序，省略中间附件屏障。其他资源若已被组内任一前序子 Pass 访问，两次访问必须都是相同状态的只读，否则切组。
- Compute 使用相同的非附件规则：共享只读可以合并，写后读、读后写、连续写或状态变化必须切组；检查覆盖整组，不能只比较最后一个子 Pass。
- 后续子 Pass 首次使用的资源可以把转换屏障前移到组入口；其生产者已在组外完成，组内前序子 Pass 不访问它。每组全部首次访问的屏障批量下发一次，然后开启 Encoder，Encoder 内不下发屏障。
- Raster 使用首 Pass 的 Load / Clear 和末 Pass 的 Store，`AllowUavWrites` 为全组 UAV 写访问的逻辑或；每个逻辑子 Pass 回调前重置 `SetStencilReference(0)`。
- Raster / Compute 每组只开关一次 Encoder，但每个逻辑子 Pass 保留独立 debug group，并且 Context 只允许解析当前子 Pass 声明的资源。回调失败时立即停止后续回调并关闭 Encoder。

被裁剪的 MeshPass 不触发当帧提取或排序；其独占资源不向池借出，其上传不执行 Map 或 Copy。
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

CompiledRDG::Prepare(slot, pool, uploads)、Record(command)、CommitSubmittedStates 各执行一次。
Prepare 和 Record 返回 false 时不可继续录制或提交，提交状态保持不变；Record 要求命令缓冲已 Begin，调用者负责 End。
Prepare 中的内部 `detail::RDGStateTracker` 借用物理资源，按初态/池 LastState 推导并冻结屏障和预期终态；Record 只编码。
tracker 是 RDGExecution 的成员，物理状态表使用稠密资源索引和图的 PMR resource，不接管资源 owner，也不维护全局注册表。
它集中组织 HostWrite / 上传页的开始屏障、组入口与上传成员间屏障、结束转换和外部 release；
共享上传页继续由 UploadArena 汇总只读用途并提供页屏障，逻辑切片不各自转换该物理页。
组内规划沿用访问列表和组标记，不为每组遍历全部资源；已知上传区间优化不改变整资源依赖模型。
初态为可写状态时保守设置 PendingWrite，同状态 UAV、写后读、附件与外部转移继续生成必要同步。
实际队列提交正常返回后调用 CommitSubmittedStates，统一发布持久 wrapper、瞬态池和上传页终态，不等待 fence。
持久 wrapper 通过公开 CommitSubmittedState 接口更新；tracker 自身只保存图内预期状态，不提前修改持久状态。
直接使用 CompiledRDG 的调用者同样负责这一边界；被裁剪或未提交的图不发布状态。

`ImportTexture` / `ImportBuffer` 的可选 `RDGExternalOwnership{Acquire, Release}` 声明外部所有权边界。
重复导入同一资源时所有权声明必须一致。执行按资源维护待 Acquire 与待 Release 标记：
首次组访问合并状态转换并发出 Acquire；没有执行组但被 MarkOutput 保活时，图末也消费 Acquire。
即使 Before 与 After 相同，所有权屏障仍发出；被裁剪的资源不转移所有权。
图末完成终态转换后，在独立 barrier 调用中 Release，未指定终态则保持当前状态。
这些声明只生成 RHI ExternalTransfer 屏障；外部队列等待/信号、跨进程同步与资源支持性仍由调用方负责。

### 每槽瞬态资源池

`RDGTransientResourcePool` 独占 Texture、Buffer 及其 View owner。进入槽前必须已完成 Wait 和 allocator Reset，随后 `BeginFrame()` 仅重置借出标记，
按描述符全部字段匹配复用，保留真实 `LastState`；同一执行内相同描述符的多次资源申请仍获得不同物理对象，不做别名复用。
新 Texture / Device Buffer 初态为 Undefined，新 Upload Buffer 为 HostWrite。实际 Submit 后记录真实终态，不能在 Fence 完成后假定纹理自动回到 Undefined。
池保留历史峰值对象，`Clear()` / 析构前所有引用它们的命令存储必须重置，View 先于资源销毁。
瞬态 View 缓存在池内；导入资源的自动 View 由本帧编译图准备结果持有，避免池把外部资源借用延长到后续帧。
直接使用池的 `AcquireTextureView` 时，目标资源必须活到该缓存 View 释放。

当前不提供子资源状态拆分、异步队列、资源内存别名、跨槽共享池、显式资源版本节点或自动 PSO/参数组构建。

教学样例的 `MeshDrawingFramePipeline` 使用同一流程，默认双槽连续六帧。
Pipeline 按槽持有可复用参数组、读回缓冲和独立 fence；动态实例/常量由 RDG Create 分配最终 Upload 切片。
唯一实例生成器在 PrepareLayout 遍历存活 Pass 的可见并集，建立 candidate → 紧凑实例映射并改写帧内 FirstInstance，
Write 每个可见实例只计算一次矩阵；持久 MeshDrawCommand 不变。常量直接生成，两份显式来源分别供普通 Pass 与描边共享。
空实例不分配和 draw，已存活附件 Clear 和 readback 保留。颜色/深度附件由每槽池持有；几何、shader、布局与 PSO 由外部共享。
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
应用可使用上述多槽帧循环，或直接通过 RHI 组织 Pass、录制、同步和提交；帧资源释放由 Slot 的对象清理链承担，
共享旧资源可将 owner 移入当前 Slot 的 `Emplace` 保活；跨 Scheduler 或跨队列的通用退役仍由应用组织。

可失败工厂返回 `Nullable<unique_ptr<T>>`，失败处记录日志，调用方检查返回值；不通过接口传递错误字符串。
Vulkan instance 和 D3D12 factory 的底层环境约束仍由 RHI 管理。
