> - 适用: 理解与实施 RadRay RHI 上传内存自动选型（VMA AUTO）与 Runtime UploadArena 双池职责解耦的通用化重构计划
> - 权威: 经用户要求保存的设计与实施计划，不作为当前契约；当前已实现行为见 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `docs/architecture/render-rhi.md`, `docs/architecture/runtime.md`, `modules/gpu/include/radray/gpu/types.h`, `modules/gpu_impl/include/radray/gpu_impl/backend/vulkan_helper.h`, `modules/gpu_impl/src/vk/vulkan_helper.cpp`, `modules/gpu_impl/src/vk/vulkan_impl.cpp`, `modules/gpu_impl/src/rhi_validation.cpp`, `modules/runtime/include/radray/runtime/upload_arena.h`, `modules/runtime/src/upload_arena.cpp`, `modules/runtime/src/rdg.cpp`, `modules/runtime/tests/test_upload_arena.cpp`, `modules/runtime/tests/test_rdg.cpp`, `modules/runtime/tests/test_rdg_upload_gpu.cpp`, `modules/runtime/tests/test_geometry_upload_gpu.cpp`, `benchmarks/bench_rdg_upload/bench_rdg_upload_gpu.cpp`

# RHI 上传内存自动选型与 UploadArena 双池通用化重构计划

## 状态、基线与确认决策

- 记录日期：`2026-11-24`
- 基线：`codex/system-hotspots-20261124` 分支 `6be7657`（审阅最近三轮提交 `8c6f918`、`4d07b51`、`6be7657`；其中 `8c6f918` 在 [`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp) 与 [`render-rhi.md`](../architecture/render-rhi.md) 中新增的 D3D12 原生光栅 pass GPU 计时区间保持不动，本计划仅重构上传内存选型与暂存分页链路）
- 状态：设计取舍已由用户确认，待按 TDD 实施，并在实施同一提交中同步 [`render-rhi.md`](../architecture/render-rhi.md) 与 [`runtime.md`](../architecture/runtime.md)。
- 已确认核心决策：
  1. **RHI 层移除 `ResourceHint::PreferHostStaging`，交由 VMA 3 `VMA_MEMORY_USAGE_AUTO` 按真实用途自动选型（原问题 1 选项 B）**：
     - 回退本分支刚引入的 `gpu::ResourceHint::PreferHostStaging` 枚举位及其 `rhi_validation.cpp` 校验分支；
     - Vulkan 后端从废弃的 `VMA_MEMORY_USAGE_GPU_ONLY` / `CPU_TO_GPU` / `GPU_TO_CPU` 迁移至 `VMA_MEMORY_USAGE_AUTO`，按 `MemoryType` 绑定主机访问方向（`Upload` → `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`，`ReadBack` → `VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT`），由真实的 `BufferDescriptor::Usage`（VMA `ContainsDeviceAccess()`）自动区分纯传输暂存（优先非 `DEVICE_LOCAL` 主机内存）与管线直读动态缓冲（优先 `DEVICE_LOCAL | HOST_VISIBLE` BAR/ReBAR，耗尽时由 VMA 内置循环自动回退主机内存）。
  2. **Runtime `UploadArena` 在全后端统一拆分 `Staging` 与 `Dynamic` 两类物理页（原问题 2 选项 A）**：
     - 移除 `UploadArena::Allocate` 的 `bool stagingCopy` 补丁参数与构造函数中的 `_separateHostStaging = (device.GetBackend() == GpuBackend::Vulkan)` 后端特判；
     - 在所有后端统一维护 `Staging` 页（物理缓冲仅挂 `BufferUse::MapWrite | BufferUse::CopySource`，`Reads` 固定 `BufferState::CopySource`，无 `CBuffer` 容量对齐膨胀）与 `Dynamic` 页（物理缓冲挂 `MapWrite | CopySource | Vertex | Index | CBuffer | Resource | Indirect`，供管线直读切片共享），保证 D3D12 与 Vulkan 页内布局一致并为未来 D3D12 `GPU_UPLOAD` 堆预留对称结构。
  3. **RDG 上传路由统一化与白盒测试占位归位**：
     - `UploadTexture`、`UploadBuffer` 以及仅声明 `BufferUse::CopySource`（不含任何管线直读位）的 `RDGDynamicBufferDescriptor` 统一分配到 `Staging` 页；含 `Vertex / Index / CBuffer / Resource / Indirect` 任一用途的动态缓冲分配到 `Dynamic` 页；
     - 将 [`test_rdg_upload_gpu.cpp`](../../modules/runtime/tests/test_rdg_upload_gpu.cpp) 与 [`test_geometry_upload_gpu.cpp`](../../modules/runtime/tests/test_geometry_upload_gpu.cpp) 中仅用于推高 Dynamic 页游标的占位动态缓冲从 `BufferUse::CopySource` 改为同池的 `BufferUse::CBuffer` / `BufferUse::Vertex`。

---

## 一、问题诊断：为什么 `8c6f918..6be7657` 属于补丁式修改

### 1.1 Vulkan 内存选型绕过了 VMA 3 原生机制且缓存策略自相矛盾

1. **旧映射使用 VMA v2 废弃策略导致全量抢占 BAR**：
   - [`vulkan_helper.cpp`](../../modules/gpu_impl/src/vk/vulkan_helper.cpp) 原先将 `MemoryType::Upload` 映射为 `VMA_MEMORY_USAGE_CPU_TO_GPU`。在项目集成的 VMA 3.x（`FindMemoryPreferences` / `CalcAllocationParams`）中，`VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` 仅对 `VMA_MEMORY_USAGE_AUTO*` 生效，`CPU_TO_GPU` 会忽略该标志并无条件追加 `preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`，致使纯 `CopySource` 暂存缓冲也占用独显稀缺的 BAR/ReBAR（非 ReBAR 设备仅 256 MiB，且 CPU 写入走 PCIe MMIO）。
2. **手写 `memoryTypes` 过滤与二次 `vmaCreateBuffer` 存在多重缺陷**：
   - `8c6f918` 在 [`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp) 中手动遍历 `VkPhysicalDeviceMemoryProperties` 过滤 `!(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)`、改设 `vmaInfo.usage = VMA_MEMORY_USAGE_UNKNOWN` 与 `preferredFlags = HOST_COHERENT | HOST_CACHED`，失败后再清空 `memoryTypeBits` 二次调用 `vmaCreateBuffer`：
     - **重复造轮子并多一次对象创建开销**：VMA `AllocateMemory` 已内置按 `FindMemoryTypeIndex` 代价排序并在分配失败时剔除当前 `memTypeIndex` 循环回退的机制；
     - **忽略 UMA / 集显拓扑**：VMA `FindMemoryPreferences` 会检查 `IsIntegratedGpu()`，而手动排除 `DEVICE_LOCAL` 在同时暴露 local 与 non-local host 内存类型的集成或混合架构上会错误避开统一内存；
     - **缓存倾向与顺序写语义冲突**：上方已挂 `SEQUENTIAL_WRITE_BIT`（倾向 write-combined 以规避大块上传时的 CPU cache RFO 污染，并与 D3D12 `D3D12_HEAP_TYPE_UPLOAD` 的 `WRITE_COMBINE` 行为一致），下方却在 `UNKNOWN` 模式下强行偏好 `HOST_CACHED_BIT`。

### 1.2 `UploadArena` 向底层谎报了暂存页的 `BufferUse`

1. **纯暂存页仍携带全套管线绑定标志**：
   - 在 [`upload_arena.cpp`](../../modules/runtime/src/upload_arena.cpp) 中，即便 `hostStaging == true` 且入口已限制 `reads == BufferState::CopySource`，新建页仍然固定传入 `CopySource | Vertex | Index | CBuffer | Resource | Indirect | MapWrite`。
   - 这不仅使暂存页在 D3D12 与 Vulkan 上承受不必要的 `CBufferAlignment` 物理容量向上取整，还让 VMA 的 `VmaBufferImageUsage::ContainsDeviceAccess()` 把纯拷贝暂存页误判为需要 GPU 管线直读的缓冲；同时迫使 [`rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp) 采用宽松的 `HasFlag(MapWrite | CopySource)` 校验，允许挂有 `Vertex | CBuffer` 的直读缓冲同时携带 `PreferHostStaging`。

### 1.3 为迁就测试占位写法引入 `stagingCopy` 导致动态缓冲分池语义分裂

1. **测试失败源于白盒游标占位，而非真实业务需求**：
   - [`test_rdg_upload_gpu.cpp`](../../modules/runtime/tests/test_rdg_upload_gpu.cpp) 第 373 行的 `prefix` 与 [`test_geometry_upload_gpu.cpp`](../../modules/runtime/tests/test_geometry_upload_gpu.cpp) 第 87、90 行的 `prefix` / `spacer` 均仅有 `MarkOutput` 而无任何读取 Pass，原意只是占满前缀字节以验证后续 `CBuffer` / `Vertex` / `Index` 切片落在非零页内偏移或新页。
   - `6be7657` 为保持这两处测试字面不变，在 `UploadArena::Allocate` 上增加 `bool stagingCopy = false`，仅由 RDG 内置 `UploadBuffer` / `UploadTexture` 传 `true`。结果用户通过 `CreateBuffer(RDGDynamicBufferDescriptor{..., BufferUse::CopySource})` 创建并交由自定义 `AddCopyPass` 消费的纯传输源（如 `test_rdg_upload_gpu.cpp` 第 464 行的自定义纹理区域初始化）反而无法进入主机暂存池，继续挤占 BAR。

---

## 二、分层实施方案（`U1`～`U4`）

### U1：RHI 契约收敛与 Vulkan `VMA_MEMORY_USAGE_AUTO` 迁移

涉及文件：
- [`modules/gpu/include/radray/gpu/types.h`](../../modules/gpu/include/radray/gpu/types.h)
- [`modules/gpu_impl/src/rhi_validation.cpp`](../../modules/gpu_impl/src/rhi_validation.cpp)
- [`modules/gpu_impl/src/vk/vulkan_helper.cpp`](../../modules/gpu_impl/src/vk/vulkan_helper.cpp)
- [`modules/gpu_impl/src/vk/vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)

具体改动：

1. **移除 `ResourceHint::PreferHostStaging` 及校验补丁**：
   - 在 `types.h` 的 `enum class ResourceHint : uint32_t` 中移除本分支刚引入的 `PreferHostStaging`；
   - 在 `rhi_validation.cpp` 的 `DeviceValidation::CreateBuffer` 中移除 `PreferHostStaging` 允许位与 `invalidStaging` 判断，恢复 `constexpr auto bufferHints = ResourceHint::Dedicated | ResourceHint::PersistentMap;`。

2. **统一映射 VMA 3 `VMA_MEMORY_USAGE_AUTO` 并按 `MemoryType` 绑定主机访问方向**：
   - 在 `vulkan_helper.cpp` 中更新 `MapType(MemoryType)`：
     ```cpp
     VmaMemoryUsage MapType(MemoryType v) noexcept {
         switch (v) {
             case MemoryType::Device: return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
             case MemoryType::Upload: return VMA_MEMORY_USAGE_AUTO;
             case MemoryType::ReadBack: return VMA_MEMORY_USAGE_AUTO;
         }
         Unreachable();
     }
     ```
   - 在 `vulkan_impl.cpp` 的 `DeviceVulkan::CreateBuffer` 中：
     - 按 `desc.Memory`（而非仅按可选 `MapWrite` / `MapRead` 位）设置互斥的 VMA 主机访问标志，确保关闭 RHI 校验或调用方创建未附带 `MapWrite` 的 `Upload` 缓冲时，既不会触发 VMA 在 `VMA_MEMORY_USAGE_AUTO` + `VMA_ALLOCATION_CREATE_MAPPED_BIT` 下要求必含 `HOST_ACCESS_*` 的断言，也不会因同时出现 `MapWrite | MapRead` 触发互斥断言，更不会误落入不可映射的纯设备内存：
       ```cpp
       if (desc.Memory == MemoryType::Upload) {
           vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
       } else if (desc.Memory == MemoryType::ReadBack) {
           vmaInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
       }
       ```
     - 删除 `PreferHostStaging` 的手动 `vmaGetMemoryProperties` 循环、`VMA_MEMORY_USAGE_UNKNOWN` 覆盖与二次 `vmaCreateBuffer` 重试，保持单次调用：
       ```cpp
       if (auto vr = vmaCreateBuffer(_vma->_vma, &bufInfo, &vmaInfo, &vkBuf, &vmaAlloc, &vmaAllocInfo);
           vr != VK_SUCCESS) {
           if (!MarkDeviceLost(vr, "CreateBuffer")) RADRAY_ERR_LOG("vmaCreateBuffer failed: {}", vr);
           return nullptr;
       }
       ```
   - **选型效果核验（对照 VMA `FindMemoryPreferences`）**：
     - `MemoryType::Upload` + `Usage = MapWrite | CopySource`（`VkBufferCreateInfo::usage` 仅含 `TRANSFER_SRC_BIT | TRANSFER_DST_BIT`，`ContainsDeviceAccess() == false`）：`requiredFlags = HOST_VISIBLE`，`notPreferredFlags = DEVICE_LOCAL | HOST_CACHED`，独显优先落入非 `DEVICE_LOCAL` 的主机 write-combined 内存，UMA 与堆满场景由 VMA 自动回退；
     - `MemoryType::Upload` + 任意管线直读位（`Vertex / Index / CBuffer / Resource / Indirect`，`ContainsDeviceAccess() == true`）：`requiredFlags = HOST_VISIBLE`，`preferredFlags = DEVICE_LOCAL`，`notPreferredFlags = HOST_CACHED`，独显优先使用 BAR/ReBAR，256 MiB BAR 耗尽时由 VMA `AllocateMemory` 自动回退至主机内存；
     - `MemoryType::ReadBack`：`requiredFlags = HOST_VISIBLE`，`preferredFlags = HOST_CACHED`；
     - `MemoryType::Device`（Buffer 与 Texture）：无 `HOST_ACCESS_*` 标志，`preferredFlags = DEVICE_LOCAL`。

---

### U2：`UploadArena` 全后端统一双池与精确 `BufferUse`

涉及文件：
- [`modules/runtime/include/radray/runtime/upload_arena.h`](../../modules/runtime/include/radray/runtime/upload_arena.h)
- [`modules/runtime/src/upload_arena.cpp`](../../modules/runtime/src/upload_arena.cpp)

具体改动：

1. **公开接口设计（声明于 `.h`，非内联实现置于 `.cpp`）**：
   - 移除 `Allocate` 的 `bool stagingCopy` 默认参数与 `_separateHostStaging` 成员变量；
   - 提供职责对称的两个分配入口：
     ```cpp
     // 分配供 GPU 管线直接读取（Vertex / Index / CBuffer / ShaderRead / Indirect，或混合 CopySource）的动态上传切片。
     // 有效大小与物理保留范围分开；本帧已返回的分配不搬移，零大小返回空切片。
     std::optional<UploadAllocation> Allocate(uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads);

     // 分配仅供传输命令读取（CopySource）的暂存切片；底层页仅声明 MapWrite | CopySource 用途。
     // 本帧已返回的分配不搬移，零大小返回空切片。
     std::optional<UploadAllocation> AllocateStaging(uint64_t size, uint64_t alignment);
     ```

2. **内部 `PageKind` 双池模型与建页契约**：
   - 在 `UploadArena` 私有作用域定义：
     ```cpp
     enum class PageKind : uint8_t {
         Dynamic,
         Staging,
     };
     ```
     并将 `Page::HostStaging` 替换为 `PageKind Kind{PageKind::Dynamic};`。
   - 抽取私有实现 `AllocatePageSlice(PageKind kind, uint64_t size, uint64_t alignment, uint64_t reservedSize, gpu::BufferStates reads)`：
     - `if (!size) return UploadAllocation{};`
     - 校验 `alignment != 0`、`reservedSize >= size`、`reservedSize <= std::numeric_limits<size_t>::max()`、`reads` 非空且仅含允许的只读状态位（`Staging` 固定由 `AllocateStaging` 传入 `reservedSize = size`、`reads = BufferState::CopySource`）；
     - 遍历 `_pages`，**在所有后端统一仅匹配 `page.Kind == kind` 的已有页**尝试游标对齐切分；
     - 无可用页时，校验 `MaxBufferSize` 并计算新页 `capacity = std::max(maximum ? std::min(_pageSize, maximum) : _pageSize, reservedSize)`，按 `kind` 精确决定 `BufferDescriptor::Usage`：
       ```cpp
       constexpr auto kStagingUsage = gpu::BufferUse::MapWrite | gpu::BufferUse::CopySource;
       constexpr auto kDynamicUsage = kStagingUsage | gpu::BufferUse::Vertex | gpu::BufferUse::Index |
                                      gpu::BufferUse::CBuffer | gpu::BufferUse::Resource | gpu::BufferUse::Indirect;
       const auto usage = kind == PageKind::Staging ? kStagingUsage : kDynamicUsage;
       auto buffer = _device.CreateBuffer({
           .Size = capacity,
           .Memory = gpu::MemoryType::Upload,
           .Usage = usage,
           .Hints = gpu::ResourceHint::PersistentMap,
       });
       ```
     - 创建或持久 `Map(0, capacity)` 失败时返回 `std::nullopt`，不污染已有页与已返回切片；
     - `Reset()`、`Clear()`、`Flush()`、`AppendReadBarriers()`、`CommitSubmittedStates()` 与 `GetCapacity()` 对两类页统一生效，保持既有帧槽生命周期与屏障汇总契约不变。

---

### U3：RDG 暂存与动态缓冲按真实用途路由

涉及文件：
- [`modules/runtime/src/rdg.cpp`](../../modules/runtime/src/rdg.cpp)

具体改动：

1. **拆分暂存分配辅助函数，完整保留 `RDGDeviceConstraints` 校验**：
   - 移除 `AllocateBuffer` 的 `bool stagingCopy` 参数；
   - 新增文件内辅助函数 `AllocateStagingBuffer`（保留测试注入自定义 `RDGDeviceConstraints` 时的对齐与上限拦截能力）：
     ```cpp
     std::optional<UploadAllocation> AllocateStagingBuffer(
         UploadArena& arena, const RDGDeviceConstraints& c, uint64_t size, uint64_t alignment) {
         const auto merged = MergeAlignment(alignment, c.BufferCopyAlignment);
         if (!merged || (c.MaxBufferSize && size > c.MaxBufferSize)) return std::nullopt;
         return arena.AllocateStaging(size, *merged);
     }
     ```

2. **统一三类上传切片路由**：
   - **纹理上传暂存（`CompiledRDG::Prepare` 中 `access.Texture` 分支）**：
     - 补充 `if (constraints.MaxBufferSize && mappedSize > constraints.MaxBufferSize) return false;`，调用 `uploads.AllocateStaging(mappedSize, *placement)`（`*placement` 已合并 `TexturePlacementAlignment` 与纹素块字节数）；
   - **缓冲上传暂存（`pass.Kind == RDGPassKind::Upload && !access.Texture` 分支）**：
     - 调用 `AllocateStagingBuffer(uploads, constraints, mappedSize, 1)`；
   - **动态 Upload 缓冲物化（`!resource.Imported && resource.Desc.Memory == gpu::MemoryType::Upload` 分支）**：
     - 判断该动态缓冲是否仅声明了传输源用途：
       ```cpp
       constexpr auto kCopyOnlyUsage = gpu::BufferUse::CopySource | gpu::BufferUse::MapWrite;
       const bool stagingOnly = (resource.Desc.Usage & ~kCopyOnlyUsage) == gpu::BufferUse::UNKNOWN;
       const auto resolvedReads = reads[i] ? reads[i] : BufferReads(resource.Desc.Usage);
       auto allocation = stagingOnly
                             ? AllocateStagingBuffer(uploads, constraints, sizes[i], resource.Alignment)
                             : AllocateBuffer(uploads, constraints, sizes[i], resource.Desc.Usage, resource.Alignment, resolvedReads);
       if (!allocation) return false;
       direct[i] = *allocation;
       execution.Slices[i] = {allocation->Buffer, allocation->Offset, sizes[i]};
       execution.States.BindBuffer(static_cast<uint32_t>(i), allocation->Buffer.Get(), resolvedReads);
       ```
     - 当 `stagingOnly == true` 时，RDG 访问校验已保证任何读取 Pass 只能以 `BufferState::CopySource` 访问该句柄（无读取 Pass 仅 `MarkOutput` 时 `BufferReads(resource.Desc.Usage)` 亦为 `BufferState::CopySource`），因此用户自定义 `AddCopyPass` 消费的纯 `CopySource` 动态缓冲与内置 `UploadBuffer` / `UploadTexture` 自动共享同一类 `Staging` 页。

---

### U4：测试、基准测量与文档同步

涉及文件：
- [`modules/gpu_impl/tests/test_buffer.cpp`](../../modules/gpu_impl/tests/test_buffer.cpp)
- [`modules/runtime/tests/test_upload_arena.cpp`](../../modules/runtime/tests/test_upload_arena.cpp)
- [`modules/runtime/tests/test_rdg.cpp`](../../modules/runtime/tests/test_rdg.cpp)
- [`modules/runtime/tests/test_rdg_upload_gpu.cpp`](../../modules/runtime/tests/test_rdg_upload_gpu.cpp)
- [`modules/runtime/tests/test_geometry_upload_gpu.cpp`](../../modules/runtime/tests/test_geometry_upload_gpu.cpp)
- [`docs/architecture/render-rhi.md`](../architecture/render-rhi.md)
- [`docs/architecture/runtime.md`](../architecture/runtime.md)

具体步骤（按 TDD 先更新与补齐测试确认失败点，再落地 `U1`～`U3`）：

1. **单元测试与双后端 GPU 测试**：
   - **`test_upload_arena.cpp`**：
     - 将 `VulkanCopyOnlyPagesRemainSeparateAcrossReset` 升级为后端无关的 `StagingAndDynamicPagesRemainSeparateWithExactBufferUsage`：断言 `AllocateStaging` 页的 `BufferDescriptor::Usage == (BufferUse::MapWrite | BufferUse::CopySource)`、`Hints == ResourceHint::PersistentMap`，`Allocate` 页包含管线直读 `BufferUse` 位，两类页跨 `Reset()` 各自独立复用、互不混用，且 `AppendReadBarriers` 分别输出 `CopySource` 与合并后的管线只读状态；
     - 保留 `SharedPageUnifiesReadStatesAndPaddingDoesNotOverlap` 验证同一 Dynamic 页内 `CBuffer | Vertex | CopySource` 的状态并集与偏移计算；补充 `AllocateStaging` 的零大小早退、非法 `alignment == 0` 拒绝及建页失败不破坏既有切片用例。
   - **`test_rdg.cpp`**：
     - 新增 CPU 用例验证同一图中 `UploadBuffer`、`UploadTexture` 与纯 `BufferUse::CopySource` 的 `RDGDynamicBufferDescriptor` 共享 `Usage == (MapWrite | CopySource)` 的 Staging 页，而 `BufferUse::CBuffer` / `BufferUse::Vertex` 动态缓冲分配在独立的 Dynamic 页，且自定义 `RDGDeviceConstraints::BufferCopyAlignment` 与 `MaxBufferSize` 对 Staging 路径同样生效。
   - **`test_rdg_upload_gpu.cpp`**：
     - 在 `ConstantsUseNonzeroOffsetsAcrossPagesAndReuseParameterSets` 中，将仅用于占满 Dynamic 页前 256 字节的 `prefix` 描述符从 `BufferUse::CopySource` 改为 `BufferUse::CBuffer`，维持双后端 `constants[0].Offset == 256`、`constants[1].Offset == 0` 断言；
     - 在动态切片拷贝测试中额外核验纯 `BufferUse::CopySource` 动态切片底层缓冲的 `GetDesc().Usage == (BufferUse::MapWrite | BufferUse::CopySource)`。
   - **`test_geometry_upload_gpu.cpp`**：
     - 在动态几何分支中，将推高 Dynamic 页游标的 `prefix`（8 B）与 `spacer`（32 B）从 `BufferUse::CopySource` 改为 `BufferUse::Vertex`，维持双后端 `v.Offset == 8`、`i.Offset == 32` 与跨页 `v.Buffer != i.Buffer` 断言。
   - **`modules/gpu_impl/tests/test_buffer.cpp`**：
     - 覆盖双后端 `MemoryType::Upload` 在纯传输用途（`MapWrite | CopySource`）与带管线用途（`MapWrite | CopySource | CBuffer | Vertex`）下的创建、持久映射写入、`FlushMappedRange` 与 GPU 读回验证，确认 RHI 校验层与原生验证层零报错。

2. **基准验证**：
   - 构建完成后串行运行 CTest 相关套件（`GpuContract`、`BufferTest`、`UploadArena`、`RDG*`、`BufferUploadGpuTest`、`TextureUploadGpuTest`、`GeometryUploadGpuTest`）；
   - 在 Release 配置下运行 `bench_rdg_upload` 与双后端 `bench_rdg_upload_gpu`（D3D12 与 Vulkan），对照测量 `PrepareUs`、`RecordUs`、`GpuUs`，核实 Vulkan 暂存走主机内存后的实际传输与准备耗时。

3. **长期文档同步（与代码实现同一提交完成）**：
   - 在 [`docs/architecture/render-rhi.md`](../architecture/render-rhi.md) 中补充 Vulkan 缓冲/纹理内存分配规则：`CreateBuffer` / `CreateTexture` 使用 VMA `VMA_MEMORY_USAGE_AUTO*`，按 `MemoryType` 设置顺序写或随机读主机访问标志，纯传输 `Upload` 缓冲优先使用非 `DEVICE_LOCAL` 主机内存，含管线直读用途的 `Upload` 缓冲优先使用 `DEVICE_LOCAL` 可映射内存并在耗尽时自动回退；
   - 在 [`docs/architecture/runtime.md`](../architecture/runtime.md) 的「每槽 UploadArena」与「RDG 上传来源」小节中，将原 Vulkan 特判描述更新为全后端统一的 `Staging` 页（`MapWrite | CopySource`）与 `Dynamic` 页双池契约，以及 RDG 上传节点与纯 `CopySource` 动态缓冲统一使用 `Staging` 页的事实说明。
