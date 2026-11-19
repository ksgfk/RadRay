> - 适用: 理解与实施 RadRay 阶段二下游实机接入暴露的 RDG/瞬态池/着色器反射开销治理及阶段三常驻世界（稳态零排序、无索引绘制、MDI 自动合批、值类型材质常量扩展与设备级管线缓存）底座计划
> - 权威: 经用户要求保存的设计与实施计划。当前已实现行为见 [RHI 与后端](../architecture/render-rhi.md)、[Shader pipeline](../architecture/shader-pipeline.md) 与 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `shaderlib/core/platform.hlsli`, `modules/gpu/include/radray/gpu/types.h`, `modules/gpu_impl/src/d3d12/d3d12_impl.cpp`, `modules/gpu_impl/src/vk/vulkan_impl.cpp`, `modules/gpu_impl/src/rhi_validation.cpp`, `modules/shader/include/radray/shader/shader_reflector.h`, `modules/shader/src/shader_reflector.cpp`, `modules/shader/src/shader_layout.cpp`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/src/rdg.cpp`, `modules/runtime/include/radray/runtime/transient_parameter_pool.h`, `modules/runtime/src/transient_parameter_pool.cpp`, `modules/runtime/include/radray/runtime/gpu_device.h`, `modules/runtime/src/gpu_device.cpp`, `modules/runtime/include/radray/runtime/frame_slot.h`, `modules/runtime/src/frame_slot.cpp`, `modules/runtime/include/radray/runtime/scene.h`, `modules/runtime/src/scene.cpp`, `modules/runtime/include/radray/runtime/mesh_pass_processor.h`, `modules/runtime/src/mesh_pass_processor.cpp`, `modules/runtime/include/radray/runtime/mesh_draw_command.h`, `modules/runtime/src/mesh_draw_command.cpp`, `modules/runtime/include/radray/runtime/material.h`, `modules/runtime/src/material.cpp`, `modules/runtime/include/radray/runtime/material_parameter_binding_map.h`, `modules/runtime/src/material_parameter_binding_map.cpp`

# Runtime 与 RHI 阶段二实机接入反馈优化与阶段三 MDI 场景底座实施计划

## 状态、基线与范围

- 记录日期：`2026-11-21`
- 基线：`2026-11-21` 阶段二复盘清理落地后的 `develop` 分支状态（`2bc3375`，承接 [Runtime 与 RHI 阶段二实施复盘、架构成因与过度防御清理计划](2026-11-21-runtime-phase2-review-and-cleanup-plan.md)）
- 状态：根据下游高频即时命令流与大视距体素世界的实机反馈修订，待按 TDD 分步实施。
- 架构边界收敛决定（剔除四项跨层或阶段错位设计）：
  1. **不引入以裸指针和 `string_view` 为键的 `RawPsoCache`**：`PsoCache` 继续严格以带代数的 `AssetHandle<ShaderAsset>` 为身份；对不走 `AssetManager` 的轻量管线，仅在 `DeviceContext` 默认创建并暴露驱动级 `gpu::PipelineCache*`；
  2. **不向 `MaterialParameterValue` 塞入 `AssetHandle<TextureAsset>`**：保持 `MaterialAsset` 仅依赖 `ShaderAsset` 的单向生命周期边界，纹理视图继续通过 `Nullable<gpu::TextureView*>` 快照传入；`MaterialParameterValue` 仅扩充零生命周期负担的纯值类型常量（整型、向量与 `4x4` 浮点矩阵，并在写入时按反射 `RowMajor` 自动适配行列主序）；
  3. **不在 `RDGBuilder` 构图期包装需要 `Prepare` 期才能判定 `UploadArena` 切片对齐的 `AddBufferToTextureCopy` 高层中转器**：RDG 动态缓冲的实际 `sizes[i]`（`PrepareLayout`）与 `UploadArena` 页内物理偏移 `slice.Offset` 均在 `CompiledRDG::Prepare` 阶段才确定，在构图期（`Compile` 前）静态分支“1 节点直接拷贝 vs 2 节点中转拷贝”违背了 RDG 的两阶段生命周期（CPU 到纹理上传已由 `RDGBuilder::UploadTexture` 在 `Prepare` 期统一处理，纹理到读回已由 `AsyncReadbackQueue` 处理，下游固定 `GpuBuffer` 的逐行中转由调用方在单个 `AddCopyPass` 回调内循环录制 `CopyBufferToBuffer` 即可）。`O4` 收敛为：**引入 `RDGAttachmentSlice` 单级附件范围、让 `RDGBuilder(slot)` 默认继承真实设备 `RDGDeviceConstraints`、以及在 `AllocateBuffer` 中补齐 `BufferUse::Indirect` 的 4 字节对齐**；
  4. **不在阶段三提前引入 RHI `Draw[Indexed]IndirectCount`（GPU Count Buffer）**：阶段三 CPU 合批器已知确切的 `ArgumentCount`，直接消费现有 `Draw[Indexed]Indirect`；GPU 计数缓冲留归阶段四 GPU Compute Culling 落地。
- 影响范围：
  - **第一部分（阶段二实机接入暴露的 RDG / 瞬态池 / 反射开销与接口摩擦治理，`O1`～`O5`）**：
    1. `O1`（`RDG` 构图期零全局堆分配）：将 `RDGUserPass::Name` 与 `Add*Pass` 回调包装对象迁入所属 `FrameSlot` 的 PMR 线性内存，将 `RDGUploadSource::Generated` 改为 `sizeof(Generator) <= 64`（且满足对齐与 `nothrow` 移动）的内联小缓冲（SBO），其余情况回退堆分配以保持 `RDGUploadSource` 的 `noexcept` 移动契约；
    2. `O2`（`TransientParameterSetPool` 开放寻址平坦去重表）：用 `Group` 内部跨帧保留 `capacity()` 的 `vector<TransientParameterBinding> KeyBindings` + `vector<Entry> Entries` + `vector<uint32_t> Buckets` 替换 `std::unordered_map`（消除 MSVC `std::unordered_map::clear()` 每帧逐节点 `deallocate` 与 `emplace` 逐节点 `allocate` 的堆抖动）；
    3. `O3`（`RDG` 默认 `ShaderRead` 全 mip 视图对齐与 `RDGTransientResourcePool` 滑动窗口淘汰）：保留 `CompiledRDG::Prepare` 默认视图预建循环，将 `DefaultViewRequest` 修正为 `usage == gpu::TextureViewUsage::Resource ? gpu::SubresourceRange::All : 1u`（仅 `Resource` 取全 mip 以 $O(1)$ 命中 `GpuTexture::GetDefaultView()`，`UnorderedAccess` / `RenderTarget` / `Depth*` 严格保持单 mip `1u`）；为 `RDGTransientResourcePool` 补齐 8 帧滑动窗口淘汰与瞬态 Buffer 容量分桶复用；
    4. `O4`（`RDGAttachmentSlice` 单级附件范围、`RDGBuilder` 默认真实设备约束与 `BufferUse::Indirect` 4 字节切片对齐）：引入 `RDGAttachmentSlice` 消除附件子资源签名的无效自由度；修复 `RDGBuilder(FrameSlot&)` 默认约束全为 `1` 与 `AllocateBuffer` 遗漏 `BufferUse::Indirect` 4 字节对齐的问题；
    5. `O5`（`RadRay::shader` 跨后端 Push Constants 策略提升与 D3D12 Target-Only Root Constants）：支持在 `ReadLane` 中将指定 DXIL `cbuffer` 直接移入 `lane.Pushes` / `lane.ActivePushes`（保留对 `uint4 raw[N]` 等一维定长数组成员的支持），并支持 `AllowDxilOnly` 辅助根常量。
  - **第二部分（阶段三常驻世界收编底座补齐，`M1`～`M3`）**：
    1. `M1`（`DeviceContext` 默认 `gpu::PipelineCache`）：在 `DeviceContext` 内建可选的 `gpu::PipelineCache` 实例与序列化导出接口；
    2. `M2`（`SortVisibleCommands` 稳态零排序短路与全链路无索引 `Draw` 支持）：在 `differing` 扫描中合一完成有序性预检（稳态帧 $O(N)$ 零扩容直接返回），并从 `Scene::ValidateBatch`、`MeshPassProcessor::BuildMeshDrawCommands` 到 `SubmitMeshDrawCommands` 全链路放开 `IndexBuffer == nullptr` 的无索引绘制；
    3. `M3`（`MeshPassFrameDrawList` MDI 自动合批提交器与值类型 `MaterialParameterValue` 扩展）：在构图时为仅支持直接绘制的设备选择直接提交。支持间接绘制时，按切片范围与设备能力分批提交或逐批回退，并扩展支持行列主序自动适配的值类型材质常量。

---

## 一、第一部分：阶段二实机反馈优化与接口收敛方案（`O1`～`O5`）

### O1：`RDG` 构图期零全局堆分配（Pass 回调/名称 PMR 化与 `Generated` 64B SBO）

1. **`RDGUserPass` 名称与回调对象迁入 `FrameSlot` 线性存储（[`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) / [`rdg.cpp`](../../modules/runtime/src/rdg.cpp)）**
   - 将 `detail::RDGUserPass::Name` 由 `string` 改为 `std::pmr::string`（构造时绑定 `&_data.MemoryResource`）；
   - 移除 `AddRasterPass`、`AddComputePass`、`AddCopyPass` 中的 `make_shared<Callback>`，改为在 `_slot.GetMemoryResource()` 上通过 `std::pmr::polymorphic_allocator<>` 分配移动独占的 RAII 类型擦除回调对象：
     ```cpp
     struct ErasedPassCallback {
         void* Instance{nullptr};
         void (*Destroy)(void*) noexcept {nullptr};
         ErasedPassCallback() noexcept = default;
         ErasedPassCallback(void* instance, void (*destroy)(void*) noexcept) noexcept : Instance(instance), Destroy(destroy) {}
         ErasedPassCallback(ErasedPassCallback&& other) noexcept : Instance(std::exchange(other.Instance, nullptr)), Destroy(std::exchange(other.Destroy, nullptr)) {}
         ErasedPassCallback& operator=(ErasedPassCallback&& other) noexcept {
             if (this != &other) {
                 Reset();
                 Instance = std::exchange(other.Instance, nullptr);
                 Destroy = std::exchange(other.Destroy, nullptr);
             }
             return *this;
         }
         ErasedPassCallback(const ErasedPassCallback&) = delete;
         ErasedPassCallback& operator=(const ErasedPassCallback&) = delete;
         ~ErasedPassCallback() noexcept { Reset(); }
         void* get() const noexcept { return Instance; }
         void Reset() noexcept {
             if (Instance && Destroy) Destroy(Instance);
             Instance = nullptr;
             Destroy = nullptr;
         }
     };
     ```
     - 若 `std::is_trivially_destructible_v<Callback>`，`Destroy = nullptr`；否则 `Destroy = +[](void* ptr) noexcept { std::destroy_at(static_cast<Callback*>(ptr)); }`；
     - **回调构造完成到 `AddPass` 登记成功之间的异常安全析构保障**：`AddRasterPass` / `AddComputePass` / `AddCopyPass` 在 placement-new 构造 `Callback` 后**立即交由局部 RAII `ErasedPassCallback`（或 `MakeScopeGuard`）持有析构责任**，再传入 `Add*PassImpl(name, std::move(callback), ...)`。若 `AddPass` 内部在分配长 `Name` 或扩容 `UserPasses` / `Passes` 时触发上游 PMR 分配失败，栈展开会立即通过 `~ErasedPassCallback()` 调用 `Destroy` 销毁 `Callback`（保证 `T data` 持有的唯一资源绝不因未登记进 `UserPasses` 而泄漏）；
     - 登记成功后，在 `CompiledRDG::~CompiledRDG()` 与 `RDGData::~RDGData()` 中统一调用 `pass.Callback.Reset()`（已调用过则幂等跳过），确保无论构图成功执行还是 `Compile()` 失败提前丢弃，回调持有的资源与参数集引用均在帧内视图销毁前按既定顺序精确析构一次，且全程 **0 全局堆分配与 0 原子引用计数开销**。

2. **`RDGUploadSource::Generated` 内联小对象缓冲（SBO）与堆回退条件**
   - 在 `RDGUploadSource::GeneratedSource` 中使用 `alignas(std::max_align_t) std::byte Storage[64]` 配合操作函数表：
     - 定义内联条件常量：
       ```cpp
       constexpr bool kInlineGenerator = sizeof(Generator) <= 64 &&
                                         alignof(Generator) <= alignof(std::max_align_t) &&
                                         std::is_nothrow_move_constructible_v<Generator>;
       ```
     - 当 `kInlineGenerator == true` 时，直接在 `Storage` 内原位 placement-new 构造 `Generator`，`RDGUploadSource` 的移动构造、移动赋值、`ReleaseSource()` 与析构均通过函数表在 `Storage` 上原位转移并精确调用一次 `std::destroy_at`；
     - **当 `kInlineGenerator == false`（即 `sizeof(Generator) > 64`、或者 `alignof(Generator) > alignof(std::max_align_t)`、或者 `Generator` 的移动构造非 `noexcept`）时，统一回退至堆分配（在 `Storage` 中仅保存堆指针）**，从而对任意合法的 `T` 严格保持 `RDGUploadSource(RDGUploadSource&&) noexcept` 与 `operator=(RDGUploadSource&&) noexcept` 契约不变。

---

### O2：`TransientParameterSetPool` 开放寻址平坦去重表（消除 `std::unordered_map` 节点分配抖动）

- 在 [`transient_parameter_pool.h`](../../modules/runtime/include/radray/runtime/transient_parameter_pool.h) / [`transient_parameter_pool.cpp`](../../modules/runtime/src/transient_parameter_pool.cpp) 中：
  - **问题根因**：`radray::unordered_map` 是 `std::unordered_map` 的别名；在 Windows MSVC STL 中，`std::unordered_map::clear()` 会遍历并 `deallocate` 所有链表节点（仅保留桶指针数组），导致即便键本身不分配 vector，每帧 `BeginFrame()` 仍会触发 $N$ 次节点释放、下一帧 `AcquireParameterSet` 仍会触发 $N$ 次节点分配。
  - **解决方案**：将 `Group::Filled` 替换为完全基于跨帧复用 `radray::vector` 的开放寻址平坦哈希表（不与 `FrameSlot` 的单帧 arena 产生任何跨层引用）：
    ```cpp
    struct FilledEntry {
        uint32_t Offset{0};
        uint32_t Count{0};
        size_t Hash{0};
        size_t Bucket{0};  // 此条目在线性探测后实际占用的桶
        gpu::ShaderParameterSet* Set{nullptr};
    };
    struct Group {
        shared_ptr<gpu::BindingGroupLayout> Layout;
        vector<unique_ptr<gpu::ShaderParameterSet>> Sets;
        vector<TransientParameterBinding> KeyBindings;
        vector<FilledEntry> Entries;
        vector<uint32_t> Buckets;  // 非空时 size() 为 2 的幂，空桶值为 kEmptyBucket = UINT32_MAX
        array<size_t, 8> Peaks{};
        size_t Used{0};
    };
    ```
    1. `AcquireParameterSet` 入口将 `bindings` 拷入 `_scratch`（若尚未按 `(BindingId, ArrayElement)` 严格升序则 `std::sort`），单趟检查无重复槽位并算出 `hash`；
    2. 若 `!group.Buckets.empty()`，按 `mask = group.Buckets.size() - 1` 从 `slot = hash & mask` 起线性探测：`uint32_t entryIdx = group.Buckets[slot]`；若 `entryIdx != kEmptyBucket`，比较 `entry.Hash == hash && entry.Count == _scratch.size()` 及 `std::equal(_scratch.begin(), _scratch.end(), group.KeyBindings.data() + entry.Offset)`，命中直接返回 `entry.Set`；
    3. 未命中时先取得参数集并完成写入与 `FlushWrites()`。以加入新键后的条目数决定是否扩容，保持装载率至多 `1/2`。扩容时初始化至少 16 个、数量为 2 的幂的空桶，用缓存的 `Hash` 重新探测并更新每个既有条目的 `Bucket`。追加新键和条目时，令 `entry.Bucket` 和 `group.Buckets[entry.Bucket]` 分别记录最终桶位和条目下标。参数集写入失败时沿用现有的解绑与 `Used` 回滚路径。
    4. `BeginFrame()` 只清理上一帧实际占用的桶。即使一个持续使用的布局曾出现大量不同绑定，清桶次数也等于上一帧的唯一键数。
       ```cpp
       for (const auto& entry : group.Entries) {
           group.Buckets[entry.Bucket] = kEmptyBucket;
       }
       group.Entries.clear();
       group.KeyBindings.clear();
       ```
       保留三个 `vector` 的容量。沿用 `_scratch.clear()`、已用参数集的 `ResetBindingsForReuse()`、8 帧 `Peaks`、`Used` 归零、`Sets` 按窗口峰值收缩、冷组淘汰及 `_windowIndex` 推进。布局和负载稳定、容量已预热时，去重表和参数池每帧的堆分配与释放均为零。[现有帧回收流程](../../modules/runtime/src/transient_parameter_pool.cpp)

---

### O3：`RDG` 默认 `ShaderRead` 全 mip 视图对齐与 `RDGTransientResourcePool` 滑动窗口淘汰

1. **`DefaultViewRequest` 仅对 `TextureViewUsage::Resource` 使用全 mip 范围（[`rdg.cpp`](../../modules/runtime/src/rdg.cpp)）**
   - **保留 `CompiledRDG::Prepare`（第 1619–1628 行）对所有 `ShaderRead` 与 `UnorderedAccess` 访问预创建默认视图的循环**，确保无 `Prepare` 回调的 Pass 在 `Record` 中直取默认视图的契约不回归；
   - 将 `DefaultViewRequest(gpu::TextureViewUsage usage)` 精确修改为：
     ```cpp
     RDGTextureViewDescriptor DefaultViewRequest(gpu::TextureViewUsage usage) noexcept {
         const uint32_t mipCount = usage == gpu::TextureViewUsage::Resource ? gpu::SubresourceRange::All : 1u;
         return {.Range = {0, gpu::SubresourceRange::All, 0, mipCount}, .Usage = usage};
     }
     ```
     - 当 `usage == gpu::TextureViewUsage::Resource` 时，`mipCount = All`，对于多级 mip 的持久 `GpuTexture` 直接在 `GpuTexture::GetOrCreateView`（[`gpu_resource.cpp:85-86`](../../modules/runtime/src/gpu_resource.cpp)）中 **$O(1)$ 命中 `_defaultView`**；
     - 当 `usage` 为 `UnorderedAccess`、`RenderTarget` 或 `DepthRead` / `DepthWrite` / `DepthReadStencilWrite` / `DepthWriteStencilRead` 时，`mipCount` 严格保持 `1u`，保证 `Record` 阶段按 `ctx.ResolveView(handle, TextureViewUsage::RenderTarget)` 等查询默认 mip 0 附件视图时与 `ResolveAttachmentView` 缓存的单级 mip 0 视图精确匹配。

2. **`RDGTransientResourcePool` 8 帧滑动窗口淘汰与瞬态 Buffer 容量向上对齐复用**
   - 在 `RDGTransientResourcePool::Impl` 中为 `TextureEntry` 与 `BufferEntry` 增加 `uint32_t UnusedFrames{0}`：
     - **瞬态 Buffer 容量分桶复用与精确尺寸回退**：`AcquireBuffer(desc)` 计算请求大小的向上分桶容量 `bucketSize`（`<= 4096 B` 按 `256 B` 向上取整，`> 4096 B` 按 `std::bit_ceil(desc.Size)` 向上取整；**若向上取整算术溢出，或 `MaxBufferSize != 0 && bucketSize > MaxBufferSize`（沿用 [`rdg.cpp:733`](../../modules/runtime/src/rdg.cpp) `MaxBufferSize == 0` 表示无上限的语义，兼容默认 `FakeDevice`）则取 `desc.Size`**）。池内查找条件放宽为：`!entry.Used && entry.Desc.Memory == desc.Memory && entry.Desc.Usage == desc.Usage && entry.Desc.Hints == desc.Hints && entry.Desc.Size >= desc.Size && entry.Desc.Size <= bucketSize`（优先选择满足条件的最小 `entry.Desc.Size`）；未命中时先尝试以 `bucketSize` 调用 `_impl->Device.CreateBuffer(bucketDesc)`，**若 `bucketSize > desc.Size` 且分桶创建失败（如 `513 MiB` 向上取整为 `1 GiB` 超出当前可用显存），则立即按原始请求尺寸 `desc.Size` 重试一次**，并在 `Impl::BufferEntry::Desc` 中记录实际创建成功的 `BufferDescriptor` 容量；同时 `CompiledRDG::Prepare` 中的逻辑 `execution.Slices[i].Size` 仍严格保持调用方请求的 `sizes[i]`；
     - **8 帧未命中淘汰**：每次 `RDGTransientResourcePool::BeginFrame()` 对上一轮 `Used == false` 的条目递增 `++UnusedFrames`，对 `Used == true` 的条目重置 `UnusedFrames = 0` 与 `Used = false`；当 `UnusedFrames >= 8` 时，先从 `_impl->Views` 中擦除所有 `Desc.Target == entry.Resource.get()` 的瞬态视图，再销毁该 `TextureEntry` / `BufferEntry`。

---

### O4：`RDGAttachmentSlice` 单级附件范围、`RDGBuilder` 默认设备约束与 `BufferUse::Indirect` 4 字节对齐

1. **单级附件子资源结构 `RDGAttachmentSlice`（[`rdg.h`](../../modules/runtime/include/radray/runtime/rdg.h) / [`rdg.cpp`](../../modules/runtime/src/rdg.cpp)）**
   - 新增专用于光栅附件与清屏的轻量范围结构（消除在附件上恒为 1 的 `MipLevelCount` 与恒等于格式全集的 `Aspects`）：
     ```cpp
     struct RDGAttachmentSlice {
         uint32_t MipLevel{0};
         uint32_t BaseArrayLayer{0};
         uint32_t ArrayLayerCount{gpu::SubresourceRange::All};
         constexpr gpu::SubresourceRange ToSubresourceRange(gpu::TextureAspects aspects = {}) const noexcept {
             return {BaseArrayLayer, ArrayLayerCount, MipLevel, 1, aspects};
         }
         friend constexpr bool operator==(const RDGAttachmentSlice&, const RDGAttachmentSlice&) noexcept = default;
     };
     ```
   - 在 `RDGRasterPassBuilder::WriteColor`、`SetDepthStencil` 以及 `RDGBuilder::AddClearColor`、`AddClearDepthStencil` 新增接收 `RDGAttachmentSlice` 的重载（保留既有 `gpu::SubresourceRange` 重载兼容现有调用）。

2. **`RDGBuilder` 默认填充真实设备 `RDGDeviceConstraints`**
   - 提取内部辅助函数 `RDGDeviceConstraints QueryRDGDeviceConstraints(const gpu::Device& device) noexcept`（统一读取 `detail.BufferCopyOffsetAlignment`、`TextureDataPlacementAlignment`、`TextureDataPitchAlignment`、`CBufferAlignment` 与 `limits` 的各项对齐/上限，供 `RDGBuilder` 与 `CompiledRDG::Prepare` 共用）；
   - 将 `RDGBuilder(FrameSlot& slot)` 的默认构造行为改为：当调用方未显式传入 `RDGDeviceConstraints` 时，默认以 `QueryRDGDeviceConstraints(slot.GetDevice())` 初始化 `_data.Constraints`（保留显式传入自定义 `RDGDeviceConstraints` 的构造函数重载供单元测试注入特定约束）。

3. **`AllocateBuffer` 补齐 `BufferUse::Indirect` 的 4 字节偏移对齐（[`rdg.cpp:721-739`](../../modules/runtime/src/rdg.cpp)）**
   - RHI 校验层（[`rhi_validation.cpp:867`](../../modules/gpu_impl/src/rhi_validation.cpp)）与双后端硬件均要求 `DrawIndirect` / `DrawIndexedIndirect` 的 `argumentOffset % 4 == 0`；
   - 在 `AllocateBuffer`（`rdg.cpp:730`）中紧随 `BufferUse::Index` 之后补齐：
     ```cpp
     if (usage.HasFlag(gpu::BufferUse::Index) && !merge(4)) return std::nullopt;
     if (usage.HasFlag(gpu::BufferUse::Indirect) && !merge(4)) return std::nullopt;
     ```
     `UploadArena` 按 `reservedSize` 推进同页游标。验收使用实际占 1 B 的 `Vertex` 切片作为前序分配，以单独验证 `Indirect` 的 4 B 对齐。[上传页分配](../../modules/runtime/src/upload_arena.cpp)

---

### O5：`RadRay::shader` 跨后端 Push Constants 策略提升与 D3D12 Target-Only Root Constants

- 在 [`shader_reflector.h`](../../modules/shader/include/radray/shader/shader_reflector.h) / [`shader_reflector.cpp`](../../modules/shader/src/shader_reflector.cpp) 及离线反射工具 [`shader_reflect.cpp`](../../tools/shader_reflect/shader_reflect.cpp) 中扩展 `ShaderPushConstantOverride`：
  ```cpp
  struct ShaderPushConstantOverride {
      string Name;
      uint32_t PushId{0};
      bool PromoteDxilCBufferToRootConstants{false};
      bool AllowDxilOnly{false};
  };
  ```
  1. **`ReadLane` 中 DXIL `cbuffer` 免手写 `[RootSignature]` 提升并同步迁移活跃引用**：
     - 在 `ReadLane` 遍历 `artifact.Bindings()` 时，若 `dxil == true` 且当前 `wire` 为 `BindingKind::CBuffer`（`wire.Count == 1`），检查其名称是否命中 `policy.PushConstants` 中 `PromoteDxilCBufferToRootConstants == true` 的规则：
       - 若命中：复用 `shader_artifact.cpp` 既有的根结构体类型树校验（`wire.TypeIndex != kShaderNoType`，允许结构体内部包含标量、向量、矩阵、嵌套结构体及 `uint4 raw[N]` 等一维定长数组成员；**调用契约要求提升为 D3D12 Root Constants 的数组成员在 HLSL 中必须使用静态或字面量下标访问**，符合 D3D12 资源绑定规范），取反射根类型的**精确有效跨度 `uint32_t actualSize = artifact.Types()[wire.TypeIndex].Size`**（如两个 32 位字段为 `8 B`，而非 CBuffer 256B 硬件对齐容量 `wire.BufferByteSize`），校验 `actualSize > 0 && actualSize % 4 == 0 && actualSize <= 64 * 4`；
       - 构造 `ShaderPushConstantDesc`（`.Name = name`、`.Push.Size = actualSize`、`.Push.Stages = gpu::ShaderStages{wire.StageMask}`、`.Push.D3D12 = gpu::PushConstantDescriptor::D3D12Location{wire.Binding, wire.Group}`、`.TypeIndex = remap[wire.TypeIndex]`），**将其插入 `lane.Pushes` 并在 `wire.StageMask != 0` 时推入 `lane.ActivePushes`（绝不放入 `lane.Bindings` 与 `lane.ActiveBindings`）**，确保后续第 394–395 行组装 `Interface.ActiveBindings` 与 `Interface.ActivePushConstants` 时引用表完全一致。
  2. **`AllowDxilOnly` 辅助根常量放行（覆盖双 lane 与纯 SPIR-V 在线编译）**：
     - **双 lane 名称比对放行**：在 `ReflectShaderFamily` 检查 DXIL 与 SPIR-V 两端 `Pushes` 名称一致性时（[`shader_reflector.cpp:297`](../../modules/shader/src/shader_reflector.cpp)），若某 Push Constant 仅存在于 `variant.DXIL->Pushes` 而不在 `variant.SPIRV->Pushes` 中，且该名称在 `policy.PushConstants` 中显式标记了 `AllowDxilOnly == true`，即使 `policy.AllowTargetOnlyBindings == false` 也予以放行；
     - **纯 SPIR-V 在线编译 override 校验放行**：`ShaderAsset::Compile`（[`shader_asset.cpp:115-135`](../../modules/runtime/src/shader_asset.cpp)）在 Vulkan 设备上仅编译 SPIR-V lane 并把完整的跨后端 `desc.ReflectionPolicy` 传入 `ReflectShaderFamily`。因此在校验 `policy.PushConstants` 时（[`shader_reflector.cpp:370-372`](../../modules/shader/src/shader_reflector.cpp)），**始终先执行 `!pushLocations.emplace(rule.Name, rule.PushId).second || !pushIds.insert(rule.PushId).second` 检查（严格保留重复名称与重复 `PushId` 的拒绝语义并占位 `rule.PushId`）；随后当 `!pushes.contains(rule.Name)` 时，若 `rule.AllowDxilOnly == true` 且本次输入的所有变体均不含 `DXIL` lane（纯 SPIR-V 反射），允许该 DXIL 专属名称缺席**；反之若任一变体含 `DXIL` lane 而 `pushes` 中仍不存在 `rule.Name`，则照常报错拒绝；
     - `AllowDxilOnly` 按变体允许该名称在 SPIR-V 中缺席。构建 family 共享布局时，沿用 `MergePush` 合并所有变体实际提供的映射，并校验大小、类型和同后端位置的一致性。任一 SPIR-V 变体声明该名称，共享 `ShaderPushConstantDesc` 即保留其 Vulkan 映射。只有全部 SPIR-V 变体都缺席时，`Vulkan` 才为空。每个变体、每个后端的 `Interface.ActivePushConstants` 取自本 lane 的活跃引用。[反射合并](../../modules/shader/src/shader_reflector.cpp)
     - D3D12 映射计入 64 DWORD 根签名总预算，超限时 `DeviceD3D12::CreatePipelineLayout` 返回 `nullptr`。Vulkan 管线布局只纳入具有 Vulkan 映射的条目。[管线布局注册](../../modules/runtime/src/pipeline_layout_registry.cpp)
  3. **离线反射 CLI `radray_shader_reflect` 策略同步（[`tools/shader_reflect/shader_reflect.cpp:78-86`](../../tools/shader_reflect/shader_reflect.cpp)）**：
     - 在 `ReadPolicy` 解析 `"pushConstants"` 数组时，除必填的 `"name"` 与 `"pushId"` 外，同步读取可选布尔字段 `"promoteDxilCBufferToRootConstants"` 与 `"allowDxilOnly"`（若存在且非布尔类型则返回 `false`），保证离线 manifest CLI 反射与在线 `ReflectShaderFamily` 行为完全一致。

---

## 二、第二部分：阶段三常驻世界收编底座方案（`M1`～`M3`）

### M1：`DeviceContext` 默认持有 `gpu::PipelineCache` 与安全析构顺序

- 在 [`gpu_device.h`](../../modules/runtime/include/radray/runtime/gpu_device.h) / [`gpu_device.cpp`](../../modules/runtime/src/gpu_device.cpp) 中：
  - 为 `DeviceContext` 增加可选成员 `unique_ptr<gpu::PipelineCache> _pipelineCache`：**统一在公开构造函数 `DeviceContext::DeviceContext(shared_ptr<gpu::Device> device, gpu::CommandQueue* directQueue) noexcept`（[`gpu_device.cpp:18-23`](../../modules/runtime/src/gpu_device.cpp)）中调用现有的无参 `_device->CreatePipelineCache()` 初始化**（若后端返回空则保持 `nullptr`），确保无论是通过 `DeviceContext::Create(environment)` 工厂创建还是真设备测试直接调用 `DeviceContext(device, queue)` 构造，`GetPipelineCache()` 行为完全一致；
  - 提供 `Nullable<gpu::PipelineCache*> GetPipelineCache() const noexcept;` 与 `vector<byte> GetPipelineCacheData() const noexcept;`；
  - **严格规定析构顺序**：`DeviceContext::~DeviceContext()`（[`gpu_device.cpp:24-28`](../../modules/runtime/src/gpu_device.cpp)）在函数体内显式执行 `_device.reset()`，而 `PipelineCacheVulkan` 持有 `DeviceVulkan&` 引用并在析构时调用 `vkDestroyPipelineCache`（[`vulkan_impl.cpp:2032`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）。因此必须在 `_directQueue->Wait()` 之后、`_device.reset()` 之前**显式调用 `_pipelineCache.reset()`**，防止 `DeviceContext` 独占最后一个 `Device` 引用时发生悬垂设备访问。

---

### M2：`SortVisibleCommands` 稳态零排序短路与全链路无索引 `Draw` 支持

1. **`MeshPassFrameDrawList::SortVisibleCommands()` 稳态有序预检（[`frame_slot.cpp`](../../modules/runtime/src/frame_slot.cpp)）**
   - 在 `SortVisibleCommands()`（`VisibleCommands.size() > 64` 分支）中，将 `_scratchCommands.resize(VisibleCommands.size())` 移至单趟预检之后：
     ```cpp
     uint64_t differing = 0;
     bool sorted = true;
     for (size_t i = 1; i < VisibleCommands.size(); ++i) {
         differing |= VisibleCommands[i].SortKey ^ VisibleCommands[0].SortKey;
         sorted &= VisibleCommands[i - 1].SortKey <= VisibleCommands[i].SortKey;
     }
     if (sorted) return;
     _scratchCommands.resize(VisibleCommands.size());
     ```
   - 当可见命令序列已按 `SortKey` 非递减排列时，$O(N)$ 只读扫描后立即返回，零 `_scratchCommands` 扩容、零基数分桶搬运。

2. **从 `Scene`、`MeshPassProcessor` 到 `SubmitMeshDrawCommands` 全链路支持无索引 `Draw`**
   - **数据约定**：当 `MeshBatchElement::IndexBuffer == nullptr`（对应 `MeshDrawCommand::IndexBuffer.Target == nullptr`）时，表示**无索引绘制**：
     - `IndexBufferOffset` 与 `VertexOffset` 必须为 `0`（`MeshDrawCommand::IndexBuffer.Offset == 0`、`Stride == 0`、`VertexOffset == 0`）；
     - `IndexCount` 字段承载顶点个数 `VertexCount`（活跃元素必须 `> 0`），`FirstIndex` 字段承载起始顶点 `FirstVertex`；
     - 同一 `MeshBatch` / `MeshDrawCommand` 内的所有活跃元素必须**同为有索引**或**同为无索引**（禁止在同一 Batch 内混用有索引与无索引元素）；
     - 对所有 `VertexStepMode::Vertex` 的顶点输入流 `stream`（步长 `stride = layout.ArrayStride`），校验顶点范围不越界：当 `stride > 0` 时 `(uint64_t{element.FirstIndex} + element.IndexCount) * stride <= stream.Size`（当 `stride == 0` 时要求 `stream.Size > 0`）；
     - **对所有 `VertexStepMode::Instance` 的实例输入流（步长 `instanceStride = layout.ArrayStride`），同步校验实例范围不越界**（与 RHI [`rhi_validation.cpp:984-988`](../../modules/gpu_impl/src/rhi_validation.cpp) 的 `GraphicsEncoderValidation::Draw` 规则严格对齐，且在 MDI `DrawIndirect` 下由 Runtime 提交器前置拦截）：
       - 固定绑定在 `batch.VertexInput->Streams` 上的实例流（且未开启 `allowDeferredInstanceStream`）：在 `Batch` 入口即校验 `(uint64_t{element.FirstInstance} + element.InstanceCount) * instanceStride <= stream.Size`；
       - 延迟注入或由 `VisibleMeshDrawCommand` / `MeshPassSubmitArgs::InstanceStream` 覆盖的实例流：在冷数据绑定结构 `MeshDrawCommandBindings` 与 `MeshVertexBufferBindings` 中按绑定槽保存 `ArrayStride` 与 `StepMode`（不增加 64B `MeshDrawCommand` 热结构尺寸），**并将每个绑定的 `ArrayStride` 与 `StepMode` 同步纳入 `HashBinding(const MeshVertexBufferBindings&)` 与 `SameBinding(const MeshVertexBufferBindings&, const MeshVertexBufferBindings&)`（[`mesh_draw_command.cpp:115-132`](../../modules/runtime/src/mesh_draw_command.cpp)）的驻留去重比较**，防止共享同一底层 `VertexBufferView` 但步长不同的两条命令被错误合并为同一 `VertexBuffersIndex`；在最终实例绑定与 `FirstInstance` 重映射确定后的 `ValidateMeshDrawCommand`、`SubmitMeshDrawCommands` 与 `SubmitMeshPassIndirectDraws` 中，对所有无索引活跃元素校验 `(uint64_t{firstInstance} + instanceCount) * instanceStride <= activeInstanceView.Size`（以及 `(uint64_t{firstVertex} + vertexCount) * vertexStride <= activeVertexView.Size`）。
   - **四处代码同步贯通（共享 `MeshBatch` 元素、顶点流与实例流容量校验）**：
     1. 提取共用校验函数 `bool ValidateMeshBatchElements(const MeshBatch& batch, bool allowDeferredInstanceStream) noexcept`：识别首条活跃元素是否有 `IndexBuffer`；若为有索引 Batch，按既有规则校验 `IndexBuffer` 范围与对齐；若为无索引 Batch，校验全部活跃元素 `IndexBuffer == nullptr && IndexBufferOffset == 0 && VertexOffset == 0`、实例范围不溢出 `uint32_t`，对照 `batch.VertexInput` 中各 `VertexStepMode::Vertex` 流校验 `(FirstIndex + IndexCount) * ArrayStride <= stream.Size`，并对已绑定且非延迟覆盖的 `VertexStepMode::Instance` 流校验 `(FirstInstance + InstanceCount) * ArrayStride <= stream.Size`。[`scene.cpp:11-25`](../../modules/runtime/src/scene.cpp) 的 `Scene::ValidateBatch` 直接调用该函数；
     2. [`mesh_pass_processor.cpp:96-165`](../../modules/runtime/src/mesh_pass_processor.cpp) `MeshPassProcessor::BuildMeshDrawCommands`：在仍持有 `batch.VertexInput` 时，若 `!output.HasValidatedElements(batch)`（如经 `DynamicMeshPassDrawListContext` 提交的动态 Batch），**调用 `ValidateMeshBatchElements(batch, sharedState.AllowDeferredInstanceStream)`**，同时将 `batch.VertexInput->Layouts` 中各绑定的 `{Binding, ArrayStride, StepMode}` 存入 `MeshDrawCommandBindings`（随 `MeshVertexBufferBindings` 排序并参与 `InternBinding` 哈希/相等驻留）；无索引元素构造 `gpu::IndexBufferView{nullptr, 0, 0}`；**连续元素合并直接复用现有的完整合并判据**（第 133–151 行：`sharedState.AllowElementMerging && primitiveSize != 0`，要求 `a.IndexCount % primitiveSize == 0 && b.IndexCount % primitiveSize == 0`、`a.InstanceCount == 1 && b.InstanceCount == 1 && a.FirstInstance == b.FirstInstance`、`a.IndexBuffer == b.IndexBuffer && a.IndexBufferOffset == b.IndexBufferOffset && a.IndexStride == b.IndexStride && a.VertexOffset == b.VertexOffset` 且 `uint64_t{a.FirstIndex} + a.IndexCount == b.FirstIndex`），确保条带拓扑（`primitiveSize == 0`）、非完整图元或不同 `FirstInstance` 绝不被错误合并；
     3. [`mesh_draw_command.cpp:374-443`](../../modules/runtime/src/mesh_draw_command.cpp) `ValidateMeshDrawCommandImpl` 与 `ValidateElements`：当 `command.IndexBuffer.Target == nullptr` 时，校验 `Offset == 0 && Stride == 0 && VertexOffset == 0 && IndexCount > 0`，所有 `ExtraElements` 同样满足 `IndexBuffer.Target == nullptr && Offset == 0 && Stride == 0 && VertexOffset == 0 && IndexCount > 0`，并利用 `MeshVertexBufferBindings` 中保存的步长对最终生效的顶点流与实例流（含 `submitArgs.InstanceStream` 及 `visible` 实例区间）校验 `(firstVertex + vertexCount) * vertexStride <= vertexView.Size` 与 `(firstInstance + instanceCount) * instanceStride <= instanceView.Size`；
     4. [`mesh_draw_command.cpp:576-591`](../../modules/runtime/src/mesh_draw_command.cpp) `SubmitMeshDrawCommands`：当 `element.IndexBuffer.Target == nullptr` 时，不调用 `encoder.BindIndexBuffer`（保持 `stateCache.IndexBuffer` 原值不变），在完成连续元素合并后调用 `encoder.Draw(indexCount, instanceCount, element.FirstIndex, firstInstance)`。

---

### M3：`MeshPassFrameDrawList` MDI 自动合批提交器与值类型 `MaterialParameterValue` 扩展

1. **`MeshPassFrameDrawList` 内建 MDI 间接参数构建与切片偏移/设备能力回退提交器（[`mesh_draw_command.h`](../../modules/runtime/include/radray/runtime/mesh_draw_command.h) / [`mesh_draw_command.cpp`](../../modules/runtime/src/mesh_draw_command.cpp) / [`frame_slot.h`](../../modules/runtime/include/radray/runtime/frame_slot.h) / [`frame_slot.cpp`](../../modules/runtime/src/frame_slot.cpp)）**
   - 新增批量间接绘制准备结构，并**由 `MeshPassFrameDrawList` 直接持有当前帧本 Pass 的 `PreparedMeshPassIndirectDraws IndirectDraws`（在 `MeshPassFrameDrawList::Clear()` 中随 `FrameSlot::ResetFrameData()` 自动清空，并在 [`frame_slot.cpp:19-32`](../../modules/runtime/src/frame_slot.cpp) 手写的 `MeshPassFrameDrawList` 移动构造与移动赋值运算符中同步 `std::move(other.IndirectDraws)`，保证 `FrameSlot::_passDrawLists` 扩容搬移或外部移动赋值时不丢失、不错配已构建批次）**：
     ```cpp
     struct MeshPassIndirectDrawCounts {
         uint32_t IndexedArgumentCount{0};
         uint32_t NonIndexedArgumentCount{0};
     };

     struct MeshPassIndirectBatch {
         uint32_t CommandIndex{0};       // 指向代表本批次 PSO / VB / IB / Payload 状态的首条 VisibleMeshDrawCommand
         bool Indexed{true};
         gpu::IndexBufferView IndexBuffer{};
         uint32_t FirstArgument{0};      // 相对于 IndexedArguments 或 NonIndexedArguments 的起始下标
         uint32_t ArgumentCount{0};
         bool HasNonZeroFirstInstance{false};
     };

     struct PreparedMeshPassIndirectDraws {
         vector<MeshPassIndirectBatch> Batches;
         vector<gpu::DrawIndexedIndirectArguments> IndexedArguments;
         vector<gpu::DrawIndirectArguments> NonIndexedArguments;
         void Clear() noexcept;
     };
     ```
   - **`CountMeshPassIndirectDraws`、`BuildMeshPassIndirectDraws` 与 `SubmitMeshDrawCommands` 共用元素展开逻辑与跨池状态相等规则**：
     - **共用元素展开内核与空命令/掩码载荷结构前置检查**：提供 `bool CountMeshPassIndirectDraws(std::span<const VisibleMeshDrawCommand> visibleCommands, MeshPassIndirectDrawCounts& out) noexcept` 与 `bool BuildMeshPassIndirectDraws(std::span<const VisibleMeshDrawCommand> visibleCommands, PreparedMeshPassIndirectDraws& out)`，二者共用同一内部遍历函数并严格对齐 [`mesh_draw_command.cpp:414-443, 504-591`](../../modules/runtime/src/mesh_draw_command.cpp) 的检查顺序与展开规则：
       - 若 `visible.InstanceCount == 0 && !visible.UseElementInstanceCount` 则跳过整条命令；
       - **在解引用读取 `ValidElementMask` 与展开元素之前，首先检查 `const auto commandPtr = visible.CachedCommand; if (!commandPtr) return false;`，并完整复用 `ValidateElements` 的结构一致性检查**（校验 `command.ValidElementMask != 0`、`extraElements.size() + 1 == std::popcount(command.ValidElementMask)`、`ElementIndex` 与掩码位序严格对应，以及 `instances.empty() || instances.size() == extraElements.size() + 1`；对齐 [`test_mesh_draw_command.cpp:342, 478`](../../modules/runtime/tests/test_mesh_draw_command.cpp)），防止 `ValidElementMask = 0b0101` 但缺失 `ExtraElements` 时在 `Count` / `Build` 阶段按 `ordinal == 1` 越界访问，同时将计数失败（返回 `false`）与合法的 0 条绘制（返回 `true` 且计数为 `0`）明确区分；
       - 若 `(visible.ElementMask & command.ValidElementMask) == 0` 则跳过整条命令；
       - 逐个有效元素位解析 `firstInstance`（当 `elements && !elements->ElementInstances.empty() && visible.UseElementFirstInstance` 时取 `elements->ElementInstances[ordinal].FirstInstance`，否则取 `visible.FirstInstance`）与 `instanceCount`（当 `elements && !elements->ElementInstances.empty() && visible.UseElementInstanceCount` 时取 `elements->ElementInstances[ordinal].InstanceCount`，否则取 `visible.InstanceCount`）；
       - 若解析后的 `instanceCount == 0` 则跳过该元素；**仅在解析后的 `instanceCount == 1 && elements != nullptr` 时应用 `elements->ContiguousMergeMask` 合并连续元素**（当 `visible.InstanceCount` 被覆盖为 `> 1` 时绝不误合并，保证 `CountMeshPassIndirectDraws` 计数的字节数与 `BuildMeshPassIndirectDraws` 实际产出的字节数在任何实例数覆盖下 100% 一致）。
     - **恢复并收紧跨命令 MDI 合批的状态相等规则（包含 `BindingStorage` 池指针身份）**：
       - `BuildMeshPassIndirectDraws` 入口无条件先执行 `out.Clear()`；
       - 按 `visibleCommands` 顺序仅合并**相邻**且满足以下全部条件的元素记录进同一 `MeshPassIndirectBatch`：
         1. `command.Pipeline.Get() == batchCommand.Pipeline.Get()` 且 `command.StencilReference == batchCommand.StencilReference`；
         2. **顶点绑定完全相同**：若使用驻留索引比较，必须满足 `(command.VertexBuffersIndex == kInvalidBindingIndex && batchCommand.VertexBuffersIndex == kInvalidBindingIndex)` 或 **`(command.BindingStorage.Get() == batchCommand.BindingStorage.Get() && command.VertexBuffersIndex == batchCommand.VertexBuffersIndex)`**（严格纳入 `BindingStorage` 指针身份，防止来自 `Scene` 缓存池与 `DynamicContext` 动态池、局部索引恰好相同的不同 VB/Payload 被错误合批，对齐 [`test_mesh_draw_command.cpp:371`](../../modules/runtime/tests/test_mesh_draw_command.cpp)）；
         3. **参数与 PushConstant 负载完全相同**：`(command.PayloadIndex == kInvalidBindingIndex && batchCommand.PayloadIndex == kInvalidBindingIndex)` 或 **`(command.BindingStorage.Get() == batchCommand.BindingStorage.Get() && command.PayloadIndex == batchCommand.PayloadIndex)`**；
         4. **绘制类型与实际元素 `IndexBufferView` 完全相同**：同为有索引（`Indexed == true` 且当前元素的 `element.IndexBuffer` 与 `batch.IndexBuffer` 的 `Target`、`Offset`、`Stride` 完全相等）或同为无索引（`Indexed == false`）；
       - 满足上述条件时归入当前 `MeshPassIndirectBatch`，追加一条 `DrawIndexedIndirectArguments`（或 `DrawIndirectArguments`），并对批内每条最终 `firstInstance` 累加 **`batch.HasNonZeroFirstInstance |= (firstInstance != 0)`**；否则开启新 `MeshPassIndirectBatch`。
   - **`SetupGraph` 按设备能力选择提交路径。** 使用 `const bool deviceCanDrawIndirect = capabilities.Features.IndirectDraw && capabilities.Limits.MaxDrawIndirectCount >= 1;`。两条路径均在消费光栅 Pass 上登记 `UseMeshPass(passId)`、附件和实际使用的几何、实例及材质资源，以便帧管线正常提取和排序可见命令。[帧管线提取与准备](../../modules/runtime/src/render_graph_pipeline.cpp)
     - `deviceCanDrawIndirect == false` 时，省去两路间接参数缓冲及其生成器。`Record` 直接调用 `SubmitMeshDrawCommands(ctx.GetEncoder(), ctx.GetVisibleCommands(passId), submitArgs)` 并传回结果，使用最终实例范围和 M2 的直接提交校验。
     - `deviceCanDrawIndirect == true` 时，按以下四步准备并提交。`IndirectFirstInstance` 的判定留到每个批次的最终 `FirstInstance` 确定之后。

     1. **`SetupGraph`** 按候选元素数分别计算 `maxIndexedBytes` 和 `maxNonIndexedBytes`。先用 `graph.CreateBuffer(RDGDynamicBufferDescriptor{maxBytes, gpu::BufferUse::Indirect}, RDGUploadSource::Generated(maxBytes, ...))` 创建两路缓冲，各生成器保存自身的 `(FrameSlot* Slot, MeshPassId Pass, uint64_t MaxBytes)`，回调按 ID 重新取列表。再创建消费 Pass，并在 `UseMeshPass(passId)` 后分别对 `indexedArgsHandle`、`nonIndexedArgsHandle` 声明 `ReadBuffer(..., gpu::BufferState::Indirect)`，使两路生成器都参与调度。
     2. **`PrepareUploads`（`PrepareLayout` 回调，[`rdg.cpp:1555-1579`](../../modules/runtime/src/rdg.cpp)）**：在回调中通过 `data.Slot->GetPassDrawList(data.Pass)` 重新取得当前列表，**首先调用 `drawList.IndirectDraws.Clear()`**（确保即便本帧 `ActualSize == 0` 导致 RDG 在 [`rdg.cpp:1644`](../../modules/runtime/src/rdg.cpp) 跳过 `Write`，同槽上一帧残留的 `Batches` 也已被清空），再调用 `CountMeshPassIndirectDraws(drawList.VisibleCommands, counts)` 返回对应路（`IndexedArgumentCount * sizeof(DrawIndexedIndirectArguments)` 或 `NonIndexedArgumentCount * sizeof(DrawIndirectArguments)`）的精确字节数；**若 `CountMeshPassIndirectDraws` 因空命令或掩码结构违约返回 `false`，直接返回 `data.MaxBytes + 1` 使 `CompiledRDG::Prepare` 在第 1561 行立即失败返回 `false` 且不再调用 `Write`**；
     3. **`FillUploads`（`Write` 回调，[`rdg.cpp:1638-1694`](../../modules/runtime/src/rdg.cpp)）**：此时所有 `PrepareLayout`（含实例缓冲生成器的 `FirstInstance` 紧凑重映射）均已完成；通过 `data.Slot->GetPassDrawList(data.Pass)` 取得列表，若 `drawList.IndirectDraws.Batches.empty()`（有索引与无索引两路缓冲共用同一次构建结果，首个执行的 `Write` 构建一次），调用 `BuildMeshPassIndirectDraws(drawList.VisibleCommands, drawList.IndirectDraws)` 按最终紧凑 `FirstInstance` 构建，并在 `std::memcpy` 前核对对应路产物字节数与 `writeContext.Size` / `writeContext.Bytes.size()` 完全一致（构建失败或字节数不一致均返回 `false`）；`Write` 结束后 RDG 调用 `ReleaseSource()`（[`rdg.cpp:1691`](../../modules/runtime/src/rdg.cpp)）仅销毁临时生成器，而 `drawList.IndirectDraws` 安全存活在当前 `FrameSlot` 的 `MeshPassFrameDrawList` 上直至 `Record` 结束；
     4. **`PrepareBindings` / `Record`** 在光栅 Pass 的 `Prepare` 或 `Record` 中分别解析 `indexedArgsHandle` 与 `nonIndexedArgsHandle` 得到两个 `RDGBufferSlice`，在 `Record` 中连同当前帧的 `drawList.IndirectDraws` 调用 `SubmitMeshPassIndirectDraws`。
   - **录制期批量提交 `bool SubmitMeshPassIndirectDraws(gpu::GraphicsCommandEncoder& encoder, std::span<const VisibleMeshDrawCommand> visibleCommands, const PreparedMeshPassIndirectDraws& prepared, RDGBufferSlice indexedArgBuffer, RDGBufferSlice nonIndexedArgBuffer, const gpu::GpuDeviceCapabilities& capabilities, const MeshPassSubmitArgs& submitArgs = {}) noexcept`**：
     - **统一间接路径判定条件（预检与录制两处共用同一判定函数）**：
       - 复用构图时的设备能力条件，对每个批次计算 `canUseIndirect = deviceCanDrawIndirect && (!batch.HasNonZeroFirstInstance || capabilities.Features.IndirectFirstInstance)`。直接调用该提交接口时也按传入设备能力计算此值，保证 `MaxDrawIndirectCount == 0` 的设备进入直接绘制路径，间接路径的分块大小至少为 1。
     - **全部前置预检在发出第一条 `encoder` 绑定/绘制前一次性完成（预检失败保证编码器收到 0 条绑定与 0 条 draw）**：
       1. 与 `SubmitMeshDrawCommands` 一样对 `encoder.IsValid()`、`visibleCommands` 及 `submitArgs` 执行完整性预检（含 `M2` 无索引绘制的最终顶点流与实例流容量校验）；
       2. **在进入提交循环前，先遍历 `prepared.Batches` 一次性校验所有批次的 CPU 下标范围与 indirect 批次的 GPU 切片范围**：
          - 对**每一个** `batch`（无论 `canUseIndirect` 为 `true` 或 `false`）：校验 **`batch.CommandIndex < visibleCommands.size()`**、`batch.ArgumentCount > 0`，且 **`uint64_t{batch.FirstArgument} + batch.ArgumentCount <= (batch.Indexed ? prepared.IndexedArguments.size() : prepared.NonIndexedArguments.size())`**（防止外部传入截断的 `visibleCommands` span 或不完整的 CPU 参数数组导致越界）；
          - 若该 `batch` 满足 `canUseIndirect == true`：取目标切片 `const RDGBufferSlice& slice = batch.Indexed ? indexedArgBuffer : nonIndexedArgBuffer;` 与 `stride`，进一步校验 `slice.Buffer != nullptr && slice.Buffer->IsValid()`、`slice.Buffer->GetDesc().Usage.HasFlag(gpu::BufferUse::Indirect)`、`slice.Offset % 4 == 0`、`uint64_t{batch.FirstArgument} + batch.ArgumentCount <= slice.Size / stride` 且 `slice.Offset <= slice.Buffer->GetDesc().Size && slice.Size <= slice.Buffer->GetDesc().Size - slice.Offset`（走直接回退 `canUseIndirect == false` 的批次不要求对应 GPU 切片非空；若任一 CPU 或 GPU 范围预检失败，**在向 `encoder` 下发任何管线/资源绑定或第一条 draw 之前立即返回 `false`**）；
     - **前置预检全部通过后进入批次录制循环（录制期失败遵循现有放弃录制契约）**：
       - 复用 `MeshDrawCommandStateCache` 在每个 `MeshPassIndirectBatch` 入口仅绑定发生变化的 PSO、VertexBuffers、StencilReference、ParameterSets、PushConstants 与（有索引批次的）IndexBuffer；若录制中途可失败的编码器调用（如 `encoder.SetPushConstants`）返回 `false`，立即返回 `false` 并由调用方按 [`docs/architecture/runtime.md:992`](../architecture/runtime.md) 既有契约放弃本次可能仅完成部分录制的命令缓冲；
       - 若 `canUseIndirect == true`：按 `uint32_t consumed = 0` 循环分块：`const uint32_t chunk = std::min(batch.ArgumentCount - consumed, capabilities.Limits.MaxDrawIndirectCount);`，计算包含切片基址的实际字节偏移 **`const uint64_t byteOffset = slice.Offset + (uint64_t{batch.FirstArgument} + consumed) * stride;`**，调用 `encoder.DrawIndexedIndirect(slice.Buffer.Get(), byteOffset, chunk)`（或 `encoder.DrawIndirect(slice.Buffer.Get(), byteOffset, chunk)`），并推进 `consumed += chunk`；
       - 若 `canUseIndirect == false`（如设备不支持 `IndirectDraw`、`MaxDrawIndirectCount == 0`，或本批次含 `firstInstance != 0` 而设备 `!Features.IndirectFirstInstance`）：**该批次直接从 `prepared.IndexedArguments` / `NonIndexedArguments` 逐条调用 `encoder.DrawIndexed` / `encoder.Draw`**。

2. **扩展纯值类型 `MaterialParameterValue` 并按反射 `RowMajor` 自动适配矩阵主序（[`material.h`](../../modules/runtime/include/radray/runtime/material.h) / [`material_parameter_binding_map.h`](../../modules/runtime/include/radray/runtime/material_parameter_binding_map.h) / [`material_parameter_binding_map.cpp`](../../modules/runtime/src/material_parameter_binding_map.cpp)）**
   - 将常量块参数类型扩展至覆盖 32 位有符号/无符号整型、二维/三维/四维向量与 `4x4` 浮点矩阵：
     ```cpp
     using MaterialParameterValue = std::variant<
         float, int32_t, uint32_t,
         array<float, 2>, array<float, 3>, array<float, 4>,
         array<int32_t, 4>, array<uint32_t, 4>,
         array<float, 16>,
         Nullable<gpu::TextureView*>,
         Nullable<gpu::Sampler*>>;
     ```
   - **明确 CPU 矩阵主序约定与跨主序 Shader 自动打包**：
     - 明确约定 `MaterialParameterValue` 中的 `array<float, 16>` 在 CPU 侧统一按**行主序（Row-Major，`m[row * 4 + column]`）**存储；
     - 在 `MaterialParameterConstantTarget` 中增加 `size_t ExpectedVariantIndex{0}` 与 `bool TransposeMatrix4x4{false}`；
     - 在 `MaterialParameterBindingMap::Create` 中：
       - 标量/向量：校验 `member->Kind`（`Scalar` / `Vector`）、`member->ScalarKind`（`Float` / `Int` / `UInt`）、`member->RowCount == 1`、`member->ColumnCount`（`1..4`）、`member->Size == expectedBytes` 且 `!member->HasInternalPadding()`；
       - `array<float, 16>` 矩阵：校验 `member->Kind == ShaderTypeKind::Matrix && member->ScalarKind == ShaderScalarKind::Float && member->RowCount == 4 && member->ColumnCount == 4 && member->MatrixStride == 16 && member->Size == 64 && !member->HasInternalPadding()`，并设置 **`TransposeMatrix4x4 = !member->RowMajor`**；
     - 在 `MaterialParameterBindingMap::WriteConstants` 中：校验 `parameter->index() == target.ExpectedVariantIndex`；对 `array<float, 16>`，当 `!target.TransposeMatrix4x4`（目标 Shader 成员为 `row_major`）时直接 `std::memcpy` 64 字节，当 `target.TransposeMatrix4x4`（目标 Shader 成员为默认 `column_major`）时按 `(row, column) -> column * 16 + row * 4` 转置写入目标常量块，使同一 `MaterialInstance` / `MaterialRenderProxy` 可同时供 `row_major` 与 `column_major` 两个 Shader family 正确消费。

---

## 三、测试与验收门禁

1. **`test_radray_shader` 与离线反射 CLI `radray_shader_reflect`**：
   - 验证 `ShaderPushConstantOverride{.PromoteDxilCBufferToRootConstants = true}` 将未手写 `[RootSignature]` 的 DXIL `ConstantBuffer<TwoDwords> : register(b0, space2)`（含两个 32 位成员）及含一维定长数组 `struct PushArray { uint4 Raw[2]; }` 的 `ConstantBuffer` 提升为 `ShaderPushConstantDesc`：断言 `Push.Size` 分别为 `8` 与 `32`（取根类型有效跨度而非 256B），且该名称出现在 DXIL `Interface.ActivePushConstants` 而不在 `Interface.ActiveBindings`，与 SPIR-V `[[vk::push_constant]]` 在 `ReflectShaderVariant` 下合并成功；
   - **使用同一份包含 `AllowDxilOnly = true` 辅助根常量的 `ShaderReflectionPolicy`，分别覆盖 `DXIL-only`、`SPIR-V-only`（对应 Vulkan 下 `ShaderAsset::Compile` 在线编译路径）与 `双 lane (DXIL + SPIR-V)` 三种输入**：断言在 `AllowTargetOnlyBindings = false` 下三者均成功通过反射，且重复名称或重复 `PushId` 在 `SPIR-V-only` 下仍被拒绝；同时在 `test_radray_gpu_impl` 中覆盖 D3D12 根常量加描述符表超出 64 DWORD 总预算时 `CreatePipelineLayout` 返回 `nullptr` 的失败路径；
   - 在同一 family 中构造两个双 lane 变体，均使用 `AllowDxilOnly = true` 的 `CommonPush`，并设置 `AllowTargetOnlyBindings = false`。A 仅在 DXIL 使用它，B 在两个后端均使用它。断言共享条目含 D3D12 和 Vulkan 映射，A 的 SPIR-V 活跃表无该 `PushId`，B 的 SPIR-V 以及两者的 DXIL 活跃表均引用同一 `PushId`。仅保留 A 再次反射时，断言共享条目的 `Vulkan == std::nullopt`。
   - 验证 `radray_shader_reflect` CLI：使用包含 `"promoteDxilCBufferToRootConstants": true` 与 `"allowDxilOnly": true` 的双 lane manifest 运行离线反射工具，断言生成的 family JSON 与在线 `ReflectShaderFamily` 产物完全一致。
2. **Runtime 正确性与分配测量**（`test_radray_runtime` 覆盖 Fake RHI 与双后端真设备，替换全局分配函数的用例按仓库约定单独成 exe）。
   - **`O1` 帧槽保留容量内零分配、溢出块归还、登记前分配失败异常安全、SBO 回退与析构计数验证**：在帧数据落在 `FrameSlot` 保留的 `128 KiB` 初始块容量内时，统计预热后稳态帧的实际全局堆分配/释放次数及 `MemoryProbe::Allocations` 增长，验证长名称（`> 32` 字符）`AddRasterPass`、`AddComputePass`、`AddCopyPass` 与满足 `kInlineGenerator` 的 `RDGUploadSource::Generated` 均为 **0 次分配**；再用 `MemoryProbe` 覆盖超过 `128 KiB` 初始容量的多 Pass 大图场景，验证无逐 Pass 独立全局堆分配且溢出块在 `ResetFrameData()` 后全数归还（`LiveBytes == retained`）；**用长 Pass 名称在 `Callback` 构造完成后、`AddPass` 登记完成前触发上游 `MemoryProbe` 分配失败，断言回调捕获的唯一所有者（如 `unique_ptr<int>`）已立即析构释放**；覆盖 `> 64 B`、`alignas(64)` 过度对齐以及移动构造带 `noexcept(false)` 的生成器自动走堆回退且 `RDGUploadSource` 移动仍为 `noexcept`；构造持有 `unique_ptr<int>`（或析构计数器）的内联生成器，验证历经移动构造、`Prepare` 与 `ReleaseSource()` / 构图失败销毁后**恰好析构 1 次**；
   - **`O2` 分配探针校准。** 在同一被测配置和线程上，先给 `radray::unordered_map` 预留桶，再计量一次新键 `emplace` 和随后的 `clear()`，分别断言分配、释放计数均有增长。默认 `RADRAY_ENABLE_MIMALLOC=ON` 时，探针需覆盖 `mi_stl_allocator` 实际使用的分配与释放入口。采用全局 `operator new/delete` 计数时，使用单独的 `RADRAY_ENABLE_MIMALLOC=OFF` 构建和独立测试可执行文件。校准通过后重置计数，再开始稳态测量。[RadRay 容器分配器](../../modules/core/include/radray/types.h) · [独立测试目标先例](../../modules/core/tests/CMakeLists.txt)
   - **`O2` 稳态与跨帧复用。** 使用 Fake RHI，在同一 `FrameSlot` 上以固定工作集预热后，测量包含参数池 `BeginFrame()`、`AcquireParameterSet` 命中与未命中、帧槽回收的完整周期，断言分配和释放均为零。另以多组键触发碰撞和扩容，验证扩容后每个键仍命中原参数集。下一帧以不同顺序重新获取这些键，通过 Fake 观察解绑、重新写入和参数集复用，覆盖重排后清理了正确的桶位。
   - **`O3` 默认视图（含附件默认 mip 0 查询）、`MaxBufferSize == 0` 兼容、分桶分配失败回退原尺寸与瞬态池淘汰验证**：验证多级 mip `GpuTexture` 在默认 `ShaderRead` 下 `ctx.ResolveView(handle, TextureViewUsage::Resource)` 直接返回 `texture->GetDefaultView()`（`MipLevelCount == desc.MipLevels`），无 `Prepare` 回调的 Compute Pass 在 `Record` 中直取 `UnorderedAccess` 视图非空，且多级 mip 纹理作为默认 mip 0 光栅附件时在 `Record` 中调用 `ctx.ResolveView(handle, TextureViewUsage::RenderTarget)` / `DepthWrite` 仍能准确取到单级 mip 0 附件视图；验证 `RDGTransientResourcePool` 在连续 8 帧未借用后销毁旧瞬态纹理/缓冲及关联视图，**在默认 `FakeDevice`（`MaxBufferSize == 0` 表示无上限）下 `200 B` 与 `240 B` 瞬态 Buffer 请求正常复用同一 `256 B` 分桶 Buffer**，且在拒绝向上取整大桶（如 `1 GiB`）、接受原请求尺寸（如 `513 MiB`）的 Fake Device 下自动回退原尺寸创建成功并记录实际容量；
   - **`O4` 默认设备约束与 4 B 间接参数对齐。** 给 FakeDevice 设置非零 `MaxBufferSize`。对同一张使用静态缓冲的有效图，分别使用 `RDGBuilder{slot}` 和显式传入同一设备约束的构造函数。请求恰等于上限时两者均编译成功，超出 1 B 时两者均返回空。对齐用例令 FakeDevice 的 `Detail.BufferCopyOffsetAlignment = 1`，从容量足够且为空的 `UploadArena` 依次创建 1 B 的动态 `BufferUse::Vertex` 缓冲和一条记录大小的动态 `BufferUse::Indirect` 缓冲，两者描述符的 `Alignment` 均为 1。由存活光栅 Pass 以 `Vertex` 和 `Indirect` 状态分别读取，保证两者都得到分配。断言两个 `slice.Buffer` 相同、顶点切片偏移为 0、间接切片偏移恰为 4。缺少 `merge(4)` 时第二个偏移为 1。
   - **`M1` / `M2` / `M3` 场景、无索引绘制（含步长去重与实例流越界）、非零切片基址与重映射时点 MDI 合批及非对称矩阵主序验证**：
     - 验证通过 `DeviceContext::Create` 与直接调用公开构造函数 `DeviceContext(device, queue)` 时，`GetPipelineCache()` 在支持的后端上均非空、`GetPipelineCacheData()` 可导出，并覆盖 **`DeviceContext` 独占最后一个 `shared_ptr<gpu::Device>` 引用时的安全析构路径**（确保 `_pipelineCache.reset()` 先于 `_device.reset()` 执行，Vulkan 下无悬垂设备访问）；
     - 验证 `SortVisibleCommands()` 对已有序列表零 `_scratchCommands` 分配并直接返回；
     - 从 `Scene::AddMeshBatch`、`SyncCachedMeshDrawCommands`、`GatherVisibleMeshDrawCommands` 到 `SubmitMeshDrawCommands` 全链路验证无索引 `MeshBatch`（`IndexBuffer == nullptr`）：覆盖 `TriangleList` 单实例连续顶点合并，以及 `TriangleStrip`（`primitiveSize == 0`）、非完整图元（顶点数非 3 倍数）和不同 `FirstInstance` **不合并**的边界；**验证共享同一 64 B `VertexBufferView` 但分别按 `ArrayStride = 16` 与 `ArrayStride = 32` 声明的两条命令获得不同的 `VertexBuffersIndex` 驻留记录，且绘制第 3 个实例时 `16 B` 命令放行、`32 B` 命令拒绝**；同时覆盖**绕过 `Scene::ValidateBatch` 的 `DynamicMeshPassDrawListContext` 动态无索引 Batch 顶点流越界检出**（顶点流仅容纳 3 个顶点而传入 `FirstIndex = 1, IndexCount = 3`），以及**顶点流充足但实例流越界（如实例流仅容纳 1 条记录而 `FirstInstance = 1, InstanceCount = 1`）在直接绘制与 MDI 两条路径下均被前置拒绝**；
     - 在双后端真设备与 Fake RHI 上验证 `CountMeshPassIndirectDraws` + `BuildMeshPassIndirectDraws` + `SubmitMeshPassIndirectDraws`：
       1. **连续元素实例覆盖、空命令与掩码载荷不一致前置拒绝、跨池 `A -> B -> A` 状态隔离、列表扩容移动与 CPU/GPU 前置整体预检测试**：
          - 对照 [`test_mesh_pass_processor.cpp:355`](../../modules/runtime/tests/test_mesh_pass_processor.cpp)：3 个连续元素在 `InstanceCount = 1` 时 `Count` 与 `Build` 均产出 1 条参数（`20 B`），将可见 `InstanceCount` 覆盖为 `2`（`UseElementInstanceCount = false`）时 `Count` 与 `Build` 均展开为 3 条参数（`60 B`）；
          - 对照 [`test_mesh_draw_command.cpp:342, 478`](../../modules/runtime/tests/test_mesh_draw_command.cpp)：传入默认构造的空 `CachedCommand`（`InstanceCount == 1, CachedCommand == nullptr`）或 `ValidElementMask = 0b0101` 但缺失 `ExtraElements` 的命令，断言 `CountMeshPassIndirectDraws` 与 `BuildMeshPassIndirectDraws` 均返回 `false`，且当该输入经过真实生成器时 `PrepareLayout` 返回 `data.MaxBytes + 1` 使 `CompiledRDG::Prepare` 返回 `false` 且不调用 `Write`；而合法的 0 条绘制返回 `true` 且计数为 `0`；
          - 对照 [`test_mesh_draw_command.cpp:371`](../../modules/runtime/tests/test_mesh_draw_command.cpp)：来自两个不同 `MeshDrawCommandBindingStorage` 池、局部 `VertexBuffersIndex` / `PayloadIndex` 相同的 `A -> B -> A` 命令序列，在 MDI 下严格保持 3 个独立 `MeshPassIndirectBatch` 顺序，绝不跨池误合并；
          - 对照 [`test_frame_slot.cpp:241`](../../modules/runtime/tests/test_frame_slot.cpp)：在构建 `IndirectDraws` 后触发 `FrameSlot::GetPassDrawList` 扩容搬移及 `MeshPassFrameDrawList` 移动构造/移动赋值，断言 `IndirectDraws` 随列表完整转移且不与旧批次错配；
          - **CPU 下标与多批次 GPU 切片前置整体预检 vs 录制中途失败**：分别构造传入比构建时短的 `visibleCommands` span（使 `batch.CommandIndex >= visibleCommands.size()`）、回退批次对应的 CPU 参数数组少 1 条记录、以及第 1 批有索引有效而第 2 批无索引 `nonIndexedArgBuffer.Size` 短 1 条记录或缺少 `BufferUse::Indirect` 用途的场景，断言三者均**在前置预检阶段即返回 `false` 且 Fake Encoder 收到 0 条绑定与 0 条 draw**；同时验证预检通过后若第 2 批 `SetPushConstants` 返回 `false`，提交立即返回 `false` 供调用方放弃本次录制；
       2. **完整 `RenderGraphFramePipeline` 混合有索引 + 无索引提取、非零页内偏移、多分块推进与同槽空帧清空测试**：在消费 Pass 同时声明 `UseMeshPass(passId).ReadBuffer(indexedArgs, Indirect).ReadBuffer(nonIndexedArgs, Indirect)`，从正常帧管线 `ExtractMeshPass` 提取 1 条有索引和 1 条无索引命令，断言两路生成器均未被 RDG 裁剪且 `DrawIndexedIndirect` 与 `DrawIndirect` 均实际发出；先在同一帧 `UploadArena` 分配前置缓冲使 `indexedArgBuffer.Offset > 0`，设置 `MaxDrawIndirectCount = 1`（或小于批次记录数以强制切分为至少 2 个分块）提交多条间接记录并读回像素，确保漏加 `slice.Offset` 基址或分块循环漏推进 `consumed * stride` 均能被检出；**在同一 `FrameSlot` 上先跑一帧含混合绘制的 MDI，下一帧跑 0 条可见命令（仅清屏附件，`ActualSize == 0` 跳过 `Write`），断言第二帧 `drawList.IndirectDraws` 已清空、保留清屏且 draw 数为 0**；
       3. **`PrepareLayout` 计数 + `Write` 打包在任意生成器登记顺序下的像素一致性测试**：构造候选实例索引为 `7`、在实例生成器 `PrepareLayout` 中被紧凑重映射为 `0` 的场景，间接参数生成器在 `PrepareLayout` 中清空 `drawList.IndirectDraws` 并调用 `CountMeshPassIndirectDraws` 计数、在 `Write` 中调用 `BuildMeshPassIndirectDraws` 按最终 `FirstInstance` 打包，并由消费 Pass 声明 `UseMeshPass(passId)` 与两路 `ReadBuffer(..., BufferState::Indirect)`；**交换实例生成器与间接参数生成器的 `CreateBuffer` 登记顺序分别执行，断言两种顺序下直接绘制与 MDI 均将候选实例 `7` 绘制为紧凑实例 `0` 且像素完全一致**；
       4. **设备级直接回退。** 在 FakeDevice 的完整 `RenderGraphFramePipeline` 中分别覆盖 `IndirectDraw == false`，以及 `IndirectDraw == true && MaxDrawIndirectCount == 0`。设置 `MaxBufferSize = 64`，让四条独立可见命令各绘制同一小索引缓冲的三个索引，均可直接绘制，若打包则需 `4 × 20 = 80 B`。使用持久几何，断言 `Prepare` 成功、这两路间接参数缓冲的上传分配数为 0，`Record` 收到四次 `DrawIndexed` 和零次间接绘制。另保留单独调用 `SubmitMeshPassIndirectDraws` 的用例，验证 `MaxDrawIndirectCount == 0` 时允许空切片且返回 `true`。
       5. **批次级直接回退。** 在 `IndirectDraw == true`、`MaxDrawIndirectCount == 16`、`IndirectFirstInstance == false` 时准备两个独立批次。最终 `FirstInstance` 全为 0 的批次走间接绘制，含非零值的批次逐条直接绘制。对照全直接提交的像素读回结果。
     - 验证扩展后的 `MaterialParameterValue`：使用**非对称 `4x4` 矩阵**（如含平移分量 `m[0 * 4 + 3] = 5.0f` 的行主序矩阵），验证同一个 `MaterialInstance` 绑定到 `row_major float4x4` 与 `column_major float4x4` 两个 Shader family 时，`WriteConstants` 分别原样写入与转置写入，GPU 计算结果完全一致。
3. **`O2` 稀疏帧性能验收。** 用 Release Google Benchmark 和同一布局预热两种实现，一帧写入 65,536 个唯一键，随后每帧仅写入一个键。分别测量窗口峰值退出后 `BeginFrame()` 的耗时，对照按条目清桶和全表清零，并用持续高负载测量按条目清桶的代价。键生成与参数集首次创建放在计时区间之外，按相同工作集和配置记录结果。[性能测量](../guide/performance.md)
4. **文档与格式门禁**：
   - 实施各子项时同步更新 `docs/architecture/render-rhi.md`、`docs/architecture/shader-pipeline.md` 与 `docs/architecture/runtime.md`；
   - 运行 `python tools/check_docs.py` 与 `git diff --check` 零错误通过。
