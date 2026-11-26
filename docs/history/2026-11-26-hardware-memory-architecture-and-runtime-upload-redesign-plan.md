> - 适用: 理解与实施 RadRay 面向真实 GPU/CPU 硬件体系结构的 RHI 四态物理内存域、实时预算水位与 Runtime 内存及上传子系统全面重构计划
> - 权威: 经用户要求保存的设计与实施计划，不作为当前契约；当前已实现行为见 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)
> - 锚点: `docs/architecture/render-rhi.md`, `docs/architecture/runtime.md`, `modules/gpu/include/radray/gpu/types.h`, `modules/gpu_impl/src/validation.cpp`, `modules/gpu_impl/src/rhi_validation.cpp`, `modules/gpu_impl/src/d3d12/d3d12_helper.cpp`, `modules/gpu_impl/src/d3d12/d3d12_impl.cpp`, `modules/gpu_impl/src/vk/vulkan_helper.cpp`, `modules/gpu_impl/src/vk/vulkan_impl.cpp`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/include/radray/runtime/upload_arena.h`, `modules/runtime/include/radray/runtime/geometry_buffer_pool.h`, `modules/runtime/include/radray/runtime/rdg.h`, `modules/runtime/include/radray/runtime/render_graph_pipeline.h`, `modules/runtime/src/rdg.cpp`

# 面向真实硬件体系结构的 RHI 物理内存域与 Runtime 上传子系统重构计划

## 状态、基线与范围

- 记录日期：`2026-11-26`
- 基线提交：`develop` 分支 `a9e466c9`（含当时工作树上为 Vulkan 纯 `CopySource` 试验 `_preferredStagingMemoryTypeBits` `HOST_CACHED` 的未提交改动）
- 当前状态：**设计方案已通过 `/grill-me` 全部分支对齐定稿，暂未实施代码修改**；实施完成后再将实际契约同步至 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)。
- 核心原则：**完全不保留历史枚举或旧接口兼容包袱，一切设计对齐真实 GPU / CPU / PCIe / OS 虚拟化（WDDM）计算机体系结构**，消除底层按 `BufferUse` 盲猜物理落点的黑盒逻辑与双后端隐式分裂。
- 明确不包含的范围：
  - 纹理仍固定使用 `MemoryType::DeviceLocal`（不在本轮引入 UMA 线性/标准 Swizzle 纹理 CPU 直写扩展）；
  - 跨 API 外部共享资源（`ExternalResourceExtension`）仍限定为 `MemoryType::DeviceLocal`。

---

## 一、重构前基线快照与核心硬件失真诊断（基线 `a9e466c9`）

后续实施将删除 `GpuUploadBuffer`、`UploadArena`（`upload_arena.h/.cpp`）及旧三态 `MemoryType`。删除前的实现细节可通过以下命令回溯：

```powershell
git show a9e466c9:modules/gpu/include/radray/gpu/types.h
git show a9e466c9:modules/gpu_impl/src/d3d12/d3d12_impl.cpp
git show a9e466c9:modules/gpu_impl/src/vk/vulkan_impl.cpp
git show a9e466c9:modules/runtime/include/radray/runtime/gpu_resource.h
git show a9e466c9:modules/runtime/include/radray/runtime/upload_arena.h
git show a9e466c9:modules/runtime/src/upload_arena.cpp
git show a9e466c9:modules/runtime/src/rdg.cpp
```

### 1.1 基线职责、所有权与生命周期快照

1. **RHI 三态内存与非对称映射**：
   - [`types.h`](../../modules/gpu/include/radray/gpu/types.h) 仅定义 `MemoryType::{Device, Upload, ReadBack}`，可映射缓冲在 `CreateBuffer` 创建期持久映射。
   - **Vulkan**（[`vulkan_impl.cpp`](../../modules/gpu_impl/src/vk/vulkan_impl.cpp)）：对 `Usage == (MapWrite | CopySource)` 的 `Upload` 缓冲优先筛选 `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` 且不含 `DEVICE_LOCAL` 的主机缓存内存；对含任意管线用途的 `Upload` 缓冲交由 VMA `VMA_MEMORY_USAGE_AUTO` + `SEQUENTIAL_WRITE` 优先分配 `DEVICE_LOCAL | HOST_VISIBLE`（BAR/ReBAR）并在耗尽时由 VMA 内部静默回退主机内存。
   - **D3D12**（[`d3d12_impl.cpp`](../../modules/gpu_impl/src/d3d12/d3d12_impl.cpp)）：普通 `Upload` 一律落入 `D3D12_HEAP_TYPE_UPLOAD`（L0 系统内存 WC），尚未接入 `D3D12_HEAP_TYPE_GPU_UPLOAD`；带 `UnorderedAccess` 的可映射缓冲绕过 D3D12MA 单独调用 `CreateCommittedResource(CUSTOM, L0, WRITE_COMBINE)`（连 `ReadBack + UnorderedAccess` 也误设为读取极慢的 `WRITE_COMBINE`，且全仓零实际调用）。
2. **Runtime 持久缓冲分裂与单调不缩容 `UploadArena`**：
   - [`gpu_resource.h`](../../modules/runtime/include/radray/runtime/gpu_resource.h) 将持久缓冲硬拆为固定 `Device` 内存的 `GpuBuffer` 与固定 `Upload` 内存并内置 `std::mutex _flushMutex` 的 `GpuUploadBuffer`，两者使用互不兼容的句柄与 RDG 持久导入路径。
   - [`upload_arena.h`](../../modules/runtime/include/radray/runtime/upload_arena.h) 按槽独占维护 `Staging`（`MapWrite | CopySource`）与 `Dynamic`（全管线只读用途并集）两类页，在 `Wait + CommandAllocator::Reset` 后仅将页游标归零，**永久保留历史峰值页，不自动缩容**。
   - [`rdg.cpp`](../../modules/runtime/src/rdg.cpp) 允许 `RDGBuilder::CreateBuffer(gpu::BufferDescriptor)` 传入 `MemoryType::Upload`，并在 `Prepare` 阶段绕开 `RDGTransientResourcePool` 将其暗中改道塞入 `UploadArena` 切片，与 `RDGDynamicBufferDescriptor` 重叠。

### 1.2 对照真实硬件体系结构的六项失真

1. **`BufferUse` 与最佳物理内存段正交，无法无歧义推导**：
   - `BufferUse` 表达管线槽位绑定权限，而落 `L0` 还是 `L1 BAR` 取决于 CPU 写入体积 vs GPU 重读频次与 BAR 预算。大块单次流式直读缓冲（如每帧数 MiB～数十 MiB 的粒子/实例/剔除输入，仅单 Pass 顺序读一次）同样必须声明 `Vertex` / `Resource`，在按 `BufferUse` 推导的模型下会被强制塞进 ReBAR，令 CPU 线程阻塞于跨 PCIe MMIO 写入并挤占显存/小 BAR。
2. **小 BAR 独显在 D3D12 与 Vulkan 下硬件暴露不对称**：
   - 未启用 ReBAR 的 256 MiB 窄窗口独显上，Vulkan 暴露约 246 MiB 的独立 `DEVICE_LOCAL | HOST_VISIBLE` 堆；而 D3D12 驱动直接报告 `D3D12_FEATURE_D3D12_OPTIONS16::GPUUploadHeapSupported == FALSE`（即可直写显存为 `0 B`）。
3. **Full ReBAR 在 Windows WDDM 下不会“堆满报错自动降级”**：
   - 启用 ReBAR 后，`D3D12_HEAP_TYPE_GPU_UPLOAD` 与 Vulkan 大 BAR 堆直接共享整块 L1 显存预算。WDDM 虚拟化内存管理（VidMm）在 L1 超预算时仍会让资源创建成功，转而在后台将常驻纹理或渲染目标静默置换（Evict/Demote）至系统内存引发剧烈卡顿；必须在分配侧引入实时预算水位闸门主动分流。
4. **L0 缓存属性双后端分裂且冲刷 CPU L3 缓存**：
   - 基线 Vulkan 为 `CopySource` 偏好 `HOST_CACHED`（Write-Back），大块顺序 `memcpy` 触发 CPU Cache RFO（Read-For-Ownership）双倍 DRAM 带宽放大并把场景遍历/剔除热数据踢出 L3 Cache，而 D3D12 `UPLOAD` 为 Write-Combined（WC）。
5. **RHI 静默回退掩盖真实驻留属性，破坏零拷贝决策与 D3D12 状态机**：
   - D3D12 `GPU_UPLOAD` 以 `COMMON` 初态创建并参与常规状态转换，而 `UPLOAD` 锁死 `GENERIC_READ` 跳过转换；若 RHI 黑盒降级而不区分真实落点，状态屏障与上层 UMA/ReBAR 零拷贝判断都会失明。
6. **关卡加载突发 Staging 峰值导致锁页内存永久滞留**：
   - `UploadArena` 不缩页使得首帧突发数百 MiB 纹理/网格上传后，多个帧槽的锁页系统内存或 BAR 显存直至管线销毁前永不归还 OS。

---

## 二、已确认的设计决策与未采纳方案对照

### 2.1 已确认核心决策（`D1`～`D6.3`）

| 编号 | 决策主题 | 已确认结论 |
|---|---|---|
| `D1` | 兼容性策略 | 完全不保留旧枚举成员名与旧接口兼容，按真实硬件体系结构全仓一步到位重构。 |
| `D2` | 四态物理内存域与 L0 缓存模型 | `MemoryType` 重定义为四态单向物理域：`DeviceLocal`（L1 无映射）、`DeviceUpload`（L1 BAR WC）、`HostUpload`（双后端统一 L0 WC，移除 Vulkan `HOST_CACHED` Staging 特判）、`HostReadBack`（L0 WB，仅 `MapRead`）；`CacheCoherentUMA` 下后端自动折叠为共享 WB。 |
| `D3` | 跨段回退边界与驻留属性查询 | **RHI 严格物理直通、零静默跨段降级**：独显上 `DeviceUpload` 落不进 L1 BAR 直接返回 `nullptr`，`HostUpload` 严格排斥 `DEVICE_LOCAL`；跨段回退职责显式收归 Runtime 层，同时 `Buffer::GetMemoryProperties()` 如实回传物理属性。 |
| `D4` | 用途合法矩阵与 D3D12 堆状态 | 双后端统一单向契约：`DeviceLocal` 禁 `Map*`、放开全部 GPU 用途；`DeviceUpload` 与 `HostUpload` 必含 `MapWrite` 且仅允许 GPU 只读用途（禁 `MapRead \| CopyDestination \| UnorderedAccess`），保证两者用途 100% 同构；`HostReadBack` 必含 `MapRead` 且仅允许 `MapRead \| CopyDestination`。删除 D3D12 `CUSTOM` 堆分支与后端专属校验特判。 |
| `D5` | 硬件拓扑与实时三视图预算 | 移除 `DeviceDetail` 的 `VramBudget` / `IsUMA`，在 `GpuDeviceCapabilities` 新增静态 `DeviceMemoryInfo`；新增 `Device::QueryMemoryBudget()` 返回 `{DeviceLocal, DeviceUpload, HostShared}` 三组水位，统一覆盖 D3D12 小 BAR（`0 B`）、Vulkan 独立 ~246 MiB BAR 堆与 Full ReBAR 共享 L1 池。 |
| `D6.1` | Runtime 持久缓冲统一 | 废除 `GpuUploadBuffer` 及其内部 mutex，统一为单一 `GpuBuffer`（显式支持 `DeviceLocal / DeviceUploadWithFallback / DeviceUploadStrict / HostUpload`）；RDG 持久导入统一接收 `GpuBuffer`，主机可见缓冲支持图内不重叠区间零拷贝 `UploadBuffer`（`HostWrite`）与外部预写。 |
| `D6.2` | 帧内双池拆分与自动缩容 | 移除 `UploadArena`，拆为独立的 `StagingBufferPool`（纯 `HostUpload` 传输源）与 `DynamicBufferArena`（含 `DeviceUpload` 多遍池与 `HostUpload` 流式/溢出池）；**两套池均按 8 次槽回收滑动窗口自动淘汰闲置物理页**，契约禁止参数集跨帧悬垂持有未激活的动态切片绑定。 |
| `D6.3` | RDG 入口净化与 85% 预算闸门 | 切分 `RDGBuilder::CreateBuffer`（纯 `DeviceLocal` 瞬态独立缓冲）与 `CreateDynamicBuffer`（显式 `PreferDevice / HostOnly` 路由，零魔法尺寸阈值）；新建 `DeviceUpload` 物理缓冲/页的慢路径检查 `QueryMemoryBudget().DeviceUpload` 不超过 **85%** 水位，超限或失败回退 `HostUpload`。 |

### 2.2 已评估并排除的备选方案

1. **自由正交二元组 `(MemoryPool{L0, L1} × CpuCache{None, WC, WB})`（未采纳）**：
   - 排除原因：PCIe 电气协议决定独显硬件上不存在 `L1 + WB`，跨 API 普通堆也不存在有意义的 `L0 + None`，暴露自由笛卡尔积只会制造硬件非法组合与校验负担。
2. **五态或双向 `HostCached`（放开 `L0 + WB` 给 CPU 随机写上传 / 开放可映射缓冲 `UnorderedAccess`）（未采纳）**：
   - 排除原因：CPU 乱序拼装完全可在栈或帧槽 PMR Scratch 内存（CPU L1/L2 Cache）完成后顺序 `memcpy` 刷入 WC 页；GPU 跨 PCIe 随机/原子写 `L0` 会触发严重 Cache Snoop 失效风暴，小标量回读已有 `AsyncReadbackQueue` 合批覆盖，无需为此在 D3D12 额外维护 `CUSTOM` 堆池。
3. **RHI 内置 `DeviceUpload -> HostUpload` 静默降级（未采纳）**：
   - 排除原因：会导致 `GetDesc().Memory == DeviceUpload` 的缓冲暗中驻留在 PCIe 对端 `L0` 且混杂两种 D3D12 原生堆，既污染底层语义又让单测在回退路径上产生假阳性。
4. **Runtime 动态切片按固定字节阈值（如 64 KiB）自动分流 `Auto` 档（未采纳）**：
   - 排除原因：引入 `64 KiB` 与 `64 KiB + 64 B` 跨总线跳变的隐式性能断崖，违背显式可预测原则。

---

## 三、RHI 层详细技术规范（`RadRay::gpu` 与 `RadRay::gpu_impl`）

### 3.1 公共类型与接口契约（`modules/gpu/include/radray/gpu/types.h`）

```cpp
enum class MemoryType : int32_t {
    DeviceLocal,
    DeviceUpload,
    HostUpload,
    HostReadBack,
};

enum class MemoryArchitecture : uint8_t {
    DiscreteNarrowBar,    // 独显窄 BAR：D3D12 无 GPU_UPLOAD(0B) 或 Vulkan 小容量独立 BAR 堆
    DiscreteResizableBar, // 独显大窗口/全量 ReBAR：CPU 可经 BAR 直写主显存池
    IntegratedUMA,        // 统一物理内存池，但 CPU/GPU 缓存非全相干
    CacheCoherentUMA,     // 硬件全相干统一内存池（CPU/GPU 共享缓存一致性）
};

struct DeviceMemoryInfo {
    MemoryArchitecture Architecture{MemoryArchitecture::DiscreteNarrowBar};
    uint64_t DeviceLocalBytes{0};            // L1 物理总容量（UMA 为共享内存配额）
    uint64_t HostVisibleDeviceLocalBytes{0}; // CPU 可直写的 L1 物理容量（D3D12 不支持 GPU_UPLOAD 时为 0）
    uint64_t HostSharedBytes{0};             // L0 共享系统内存容量（UMA 下为 0，避免重复计入）
};

struct MemorySegmentBudget {
    uint64_t BudgetBytes{0}; // OS 当前授予本进程的安全预算水位（未知/不支持时为 0）
    uint64_t UsageBytes{0};  // 本进程当前已用字节
};

struct GpuMemoryBudget {
    MemorySegmentBudget DeviceLocal;  // 主显存段（UMA 为统一内存段）
    MemorySegmentBudget DeviceUpload; // 可直写显存段：D3D12 小 BAR 为 {0,0}；VK 小 BAR 为独立 BAR 堆；ReBAR/UMA 同 DeviceLocal
    MemorySegmentBudget HostShared;   // 主机共享系统内存段（UMA 下为 {0,0}）
};

struct BufferMemoryProperties {
    bool IsDeviceLocal{false};
    bool IsHostVisible{false};
    bool IsHostCoherent{false};
    bool IsHostCached{false};
};
```

- `TextureDescriptor::Memory` 与 `BufferDescriptor::Memory` 默认值统一为 `MemoryType::DeviceLocal`。
- `GpuDeviceCapabilities` 新增 `DeviceMemoryInfo Memory{};`；从 `DeviceDetail` 移除 `VramBudget` 与 `IsUMA`。
- `Device` 新增纯虚接口 `virtual GpuMemoryBudget QueryMemoryBudget() const noexcept = 0;`（只读轻量查询，可在任意线程并发调用，不提交命令、不等待队列；RHI 校验包装层做 `Ready` 检查后直接转发）。
- `Buffer` 新增纯虚接口 `virtual BufferMemoryProperties GetMemoryProperties() const noexcept = 0;`（创建期固化，销毁前不可变）。

### 3.2 描述符校验矩阵（`validation.cpp` 与 `rhi_validation.cpp` 双后端 100% 统一）

`IsBufferMemoryUsageValid(const BufferDescriptor& desc)` 跨后端统一执行以下规则（移除 `rhi_validation.cpp` 中原有的 `GetBackend() == GpuBackend::D3D12` 分支）：

1. `desc.Hints` 仅允许为空或 `ResourceHint::Dedicated`。
2. 按 `desc.Memory` 校验 `desc.Usage`：
   - `MemoryType::DeviceLocal`：禁止 `MapRead | MapWrite`；允许 `Common | CopySource | CopyDestination | Index | Vertex | CBuffer | Resource | UnorderedAccess | Indirect` 的任意非空子集。
   - `MemoryType::DeviceUpload` 与 `MemoryType::HostUpload`：**必须包含 `MapWrite`**，且除 `MapWrite` 外仅允许 `CopySource | Index | Vertex | CBuffer | Resource | Indirect`（严格禁止 `Common | MapRead | CopyDestination | UnorderedAccess`）。
   - `MemoryType::HostReadBack`：**必须包含 `MapRead`**，且仅允许 `MapRead | CopyDestination`。
3. `ValidateTextureDescriptor` 要求 `desc.Memory == MemoryType::DeviceLocal`。

### 3.3 D3D12 后端映射（`d3d12_helper.cpp`、`d3d12_impl.cpp`）

1. **静态拓扑探测**：
   - 读取 `CD3DX12FeatureSupport` 的 `UMA()`、`CacheCoherentUMA()`、`GPUUploadHeapSupported()`（`D3D12_FEATURE_D3D12_OPTIONS16`）与 `DXGI_ADAPTER_DESC1`：
     - `UMA() == TRUE`：`Architecture = CacheCoherentUMA() ? CacheCoherentUMA : IntegratedUMA`；`DeviceLocalBytes = HostVisibleDeviceLocalBytes = DedicatedVideoMemory ? DedicatedVideoMemory : SharedSystemMemory`（若初始 `QueryVideoMemoryInfo(LOCAL)` 可用且无 `DedicatedVideoMemory`，以系统报告容量为准），`HostSharedBytes = 0`。
     - `UMA() == FALSE`：`DeviceLocalBytes = DedicatedVideoMemory`，`HostSharedBytes = SharedSystemMemory`；若 `GPUUploadHeapSupported() == TRUE` 则 `Architecture = DiscreteResizableBar`、`HostVisibleDeviceLocalBytes = DeviceLocalBytes`，否则 `Architecture = DiscreteNarrowBar`、`HostVisibleDeviceLocalBytes = 0`。
2. **缓冲创建与严格直通**：
   - 删除 `d3d12_impl.cpp` 原有的 `D3D12_HEAP_TYPE_CUSTOM` 分支，全部走 `D3D12MA::Allocator::CreateResource`：
     - `DeviceLocal` → `D3D12_HEAP_TYPE_DEFAULT`，初态 `D3D12_RESOURCE_STATE_COMMON`；
     - `DeviceUpload` → 若 `!IsUMA && !GPUUploadHeapSupported()` 直接返回 `nullptr`（不打印误导性设备丢失错误，仅在常规创建失败时按原生错误码处理）；在 UMA 且 `!GPUUploadHeapSupported()` 的旧系统上允许映射至同处共享物理池的 `D3D12_HEAP_TYPE_UPLOAD`（固定态 `GENERIC_READ`），支持 `GPU_UPLOAD` 时统一使用 `D3D12_HEAP_TYPE_GPU_UPLOAD`（初态 `D3D12_RESOURCE_STATE_COMMON`）；
     - `HostUpload` → `D3D12_HEAP_TYPE_UPLOAD`，初态 `D3D12_RESOURCE_STATE_GENERIC_READ`；
     - `HostReadBack` → `D3D12_HEAP_TYPE_READBACK`，初态 `D3D12_RESOURCE_STATE_COPY_DEST`（固定附带 `D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE`）。
   - `BufferD3D12` 保存实际原生 `D3D12_HEAP_TYPE _heapType` 与预计算的 `BufferMemoryProperties`：
     - `IsDeviceLocal`：`DEFAULT`、`GPU_UPLOAD` 或 `UMA` 下为 `true`，独显 `UPLOAD` / `READBACK` 为 `false`；
     - `IsHostVisible`：非 `DEFAULT` 为 `true`；
     - `IsHostCoherent`：所有可映射堆在 D3D12 契约下均为 `true`（`HostCoherentAtomSize == 1`）；
     - `IsHostCached`：`READBACK` 或 `CacheCoherentUMA` 下的可映射堆为 `true`，其余 WC 堆为 `false`。
3. **资源状态屏障（`CmdListD3D12::ResourceBarrier`）**：
   - 按 `buf->_heapType` 判断：当 `_heapType == D3D12_HEAP_TYPE_UPLOAD || _heapType == D3D12_HEAP_TYPE_READBACK` 时因原生状态锁死跳过 transition；当 `_heapType == D3D12_HEAP_TYPE_DEFAULT || _heapType == D3D12_HEAP_TYPE_GPU_UPLOAD` 时正常录制状态转换（其中 `BufferState::HostWrite` 在 `MapType(BufferStates)` 中映射为 `D3D12_RESOURCE_STATE_COMMON`）。
4. **实时预算查询（`DeviceD3D12::QueryMemoryBudget`）**：
   - 调用 `_mainAlloc->GetBudget(&localBudget, &nonLocalBudget)`：
     - `DeviceLocal = {localBudget.BudgetBytes, localBudget.UsageBytes}`；
     - UMA 下：`DeviceUpload = DeviceLocal`，`HostShared = {0, 0}`；
     - 独显下：`DeviceUpload = GPUUploadHeapSupported ? DeviceLocal : MemorySegmentBudget{0, 0}`，`HostShared = {nonLocalBudget.BudgetBytes, nonLocalBudget.UsageBytes}`。

### 3.4 Vulkan 后端映射（`vulkan_helper.cpp`、`vulkan_impl.cpp`）

1. **扩展与静态拓扑探测**：
   - 设备初始化时检测可选设备扩展 `VK_EXT_memory_budget`；若支持则加入 `deviceExts` 并在 `VmaAllocatorCreateInfo::flags` 设置 `VMA_ALLOCATION_CREATE_EXT_MEMORY_BUDGET_BIT`。
   - 遍历 `VkPhysicalDeviceMemoryProperties` 预计算位掩码与堆拓扑：
     - 统计含 `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` 的堆与含 `DEVICE_LOCAL | HOST_VISIBLE` 内存类型所属的堆索引集合；
     - 集成显卡（`INTEGRATED_GPU` 或所有堆均为 `DEVICE_LOCAL`）：若存在 `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` 内存类型则为 `CacheCoherentUMA`，否则 `IntegratedUMA`；
     - 独立显卡：若可直写 `DEVICE_LOCAL` 堆容量大于 `512 MiB` 或与主 `DEVICE_LOCAL` 堆为同一大堆（`>= DeviceLocalBytes / 2`），判定为 `DiscreteResizableBar`，否则为 `DiscreteNarrowBar`（并记录独立小 BAR 堆索引掩码供 `QueryMemoryBudget` 精确分离）。
2. **缓冲创建与严格物理直通（消除黑盒回退与 `HOST_CACHED` Staging 特判）**：
   - 移除旧 `_preferredStagingMemoryTypeBits` 与失败清空 `memoryTypeBits` 二次重试逻辑：
     - `DeviceLocal`：`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`，独显上 `preferredFlags` 倾向不含 `HOST_VISIBLE` 的纯显存类型以保护 256 MiB 小 BAR，`requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`。
     - `DeviceUpload`：`VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`，`requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`（独显上偏好不含 `HOST_CACHED`；`CacheCoherentUMA` 下允许共享缓存页）。不支持或小 BAR 耗尽时 `vmaCreateBuffer` 直接失败并返回 `nullptr`，**绝不回退非 `DEVICE_LOCAL` 内存**。
     - `HostUpload`：`VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`；独显上通过预计算的 `_hostOnlyMemoryTypeBits`（排斥一切含 `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` 的内存类型）锁定 L0 系统内存，并由 VMA `SEQUENTIAL_WRITE` 自然排斥 `HOST_CACHED` 优先选取 WC 内存（仅当平台无非缓存 L0 类型时才兼容同属 L0 的其他主机可见类型）；UMA 下不排斥统一内存池。
     - `HostReadBack`：`VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT`；独显上同样锁定 `_hostOnlyMemoryTypeBits` 排斥 `DEVICE_LOCAL`，优先选取 `HOST_CACHED`（WB）。
   - 根据最终 `vmaAllocInfo.memoryType` 的原生 `VkMemoryPropertyFlags` 直接填充 `BufferMemoryProperties` 四个布尔位。
3. **实时预算查询（`DeviceVulkan::QueryMemoryBudget`）**：
   - 调用 `vmaGetHeapBudgets(_vma->_vma, budgets)`：
     - `DiscreteNarrowBar` 且存在独立 BAR 堆时：主 `DEVICE_LOCAL` 堆汇总入 `DeviceLocal`，含 `DEVICE_LOCAL | HOST_VISIBLE` 的独立小 BAR 堆汇总入 `DeviceUpload`，非 `DEVICE_LOCAL` 堆汇总入 `HostShared`；
     - `DiscreteResizableBar` 下：可直写 `DEVICE_LOCAL` 堆汇总入 `DeviceUpload`（与 `DeviceLocal` 共享 L1 水位），非 `DEVICE_LOCAL` 堆汇总入 `HostShared`；
     - `UMA` 下：统一堆汇总入 `DeviceLocal` 与 `DeviceUpload`，`HostShared = {0, 0}`。

---

## 四、Runtime 层详细重构规范（`RadRay::runtime`）

### 4.1 统一持久缓冲 `GpuBuffer`（`gpu_resource.h/.cpp`）

废除 `GpuUploadBuffer`，单一 `GpuBuffer` 覆盖全部持久缓冲场景：

```cpp
enum class GpuBufferMemory : uint8_t {
    DeviceLocal,              // 纯显存（自动附加 CopyDestination，不可映射）
    DeviceUploadWithFallback, // 优先 DeviceUpload（受 85% 预算水位保护），不满足或失败时自动落 HostUpload
    DeviceUploadStrict,       // 严格要求 DeviceUpload（受 85% 预算水位保护或直接严格分配），落不进 L1 BAR 直接返回空
    HostUpload,               // 固定落 HostUpload（L0 WC）
};

struct GpuBufferDescriptor {
    uint64_t Size{0};
    gpu::BufferUses Usage{gpu::BufferUse::Resource};
    GpuBufferMemory Memory{GpuBufferMemory::DeviceLocal};
};
```

- **分配与预算闸门**：
  - `DeviceLocal`：创建 `gpu::MemoryType::DeviceLocal`，自动补充 `BufferUse::CopyDestination`，初始 `SubmittedState = BufferState::Common`。
  - 主机可见策略（`DeviceUploadWithFallback / DeviceUploadStrict / HostUpload`）：校验 `Usage` 仅含 GPU 只读用途，自动附加 `BufferUse::MapWrite`，初始 `SubmittedState = BufferState::HostWrite`。其中 `DeviceUploadWithFallback` 在慢路径检查 `CanAllocateDeviceUpload(device, Size)`（`HostVisibleDeviceLocalBytes > 0` 且若 `BudgetBytes > 0` 则 `UsageBytes + Size <= BudgetBytes * 85 / 100`），通过则尝试 `MemoryType::DeviceUpload`，未通过或返回 `nullptr` 时自动改建 `MemoryType::HostUpload`；`DeviceUploadStrict` 仅创建 `MemoryType::DeviceUpload`。
- **查询与无锁切片写入**：
  - 提供 `IsHostVisible() const noexcept`、`GetMemoryProperties() const noexcept`、`Nullable<byte*> GetMappedData() const noexcept`、`uint64_t GetSliceAlignment() const noexcept`、`uint64_t AlignSliceSize(uint64_t size) const noexcept` 与 `bool FlushMappedRange(gpu::BufferRange range) noexcept`（无内部 mutex；多线程可并发写互不重叠且满足 `GetSliceAlignment()` 的区间，由提交线程串行调用 `FlushMappedRange`）。
- **几何池联动（`geometry_buffer_pool.h/.cpp`）**：
  - `VertexBufferPoolDescriptor` 与 `IndexBufferPoolDescriptor` 增加可选 `GpuBufferMemory Memory{GpuBufferMemory::DeviceLocal}`，页面底层统一持有 `IntrusivePtr<GpuBuffer>`，为 UMA / 小规模动态几何池直通零拷贝预留无分叉路径。

### 4.2 帧内双分配器拆分与 8 槽窗口自动缩容

删除 `upload_arena.h/.cpp`，新增两对满足同名一一对应规则的组件：

1. **`staging_buffer_pool.h/.cpp`（`StagingBufferPool`）**：
   - 物理页固定使用 `gpu::MemoryType::HostUpload` 与 `BufferUse::MapWrite | BufferUse::CopySource`，无 `CBuffer` 容量膨胀，GPU 读取状态固定为 `BufferState::CopySource`。
   - 接口：`BeginFrame()`（推进 8 槽窗口计数，销毁连续 8 次槽回收 `Cursor == 0` 的闲置物理页）、`Allocate(uint64_t size, uint64_t alignment)`、`Flush()`、`AppendReadBarriers(...)`、`CommitSubmittedStates()`、`Clear()`、`GetCapacity()`。
2. **`dynamic_buffer_arena.h/.cpp`（`DynamicBufferArena`）**：
   - 专职帧内管线直读切片（页用途为 `MapWrite | CopySource | Vertex | Index | CBuffer | Resource | Indirect`），内部维护两类物理页标签：
     - `DevicePage`（`gpu::MemoryType::DeviceUpload`）
     - `HostPage`（`gpu::MemoryType::HostUpload`）
   - 路由规则（显式枚举 `DynamicBufferResidency::{PreferDevice, HostOnly}`）：
     - `HostOnly`：仅在 `HostPage` 线性分配，不足时新建 `HostUpload` 页；
     - `PreferDevice`：优先在已有 `DevicePage` 分配；不足时检查 85% `DeviceUpload` 实时预算水位闸门，通过则尝试新建 `DeviceUpload` 页，未通过或创建失败则自动在 `HostPage` 分配/建页。
   - **8 槽滑动窗口自动缩容**：`BeginFrame()` 同样销毁连续 8 次槽回收未分配任何字节的闲置 `DevicePage` 与 `HostPage`。契约明确：凡绑定 `DynamicBufferArena` 切片的 `ShaderParameterSet` 必须通过 `TransientParameterSetPool` 获取或在每槽 `BeginFrame` 重置/重绑，严禁跨帧悬垂保留未激活页上的绑定（尤其是 Vulkan 生成 `VkBufferView` 的 `TypedBuffer`）。
   - `transient_parameter_pool.h/.cpp` 中的 `AllocateDynamicUniform` 迁移为接收 `DynamicBufferArena&` 并默认使用 `DynamicBufferResidency::PreferDevice`。

### 4.3 RDG 接口净化与零拷贝持久直写（`rdg.h/.cpp`、`render_graph_pipeline.h/.cpp`）

1. **清晰分离两类图内缓冲创建入口**：
   - `RDGTransientBufferDescriptor { uint64_t Size{0}; gpu::BufferUses Usage{gpu::BufferUse::UNKNOWN}; gpu::ResourceHints Hints{}; }`：
     - `RDGBuilder::CreateBuffer(const RDGTransientBufferDescriptor& desc)` **仅创建纯 `DeviceLocal` 瞬态独立缓冲**，100% 由 `RDGTransientResourcePool` 分桶分配与 8 帧淘汰管理，彻底移除图内伪装 `Upload` 缓冲的旁路。
   - `RDGDynamicBufferDescriptor { uint64_t MaxSize{0}; gpu::BufferUses Usage{gpu::BufferUse::CBuffer}; uint64_t Alignment{1}; DynamicBufferResidency Residency{DynamicBufferResidency::PreferDevice}; }`：
     - `RDGBuilder::CreateDynamicBuffer(const RDGDynamicBufferDescriptor& desc, RDGUploadSource source)` **专职当帧 CPU 写入切片**：当 `(desc.Usage & ~BufferUse::MapWrite) == BufferUse::CopySource` 时固定路由至 `StagingBufferPool`；含管线只读用途时按 `desc.Residency` 路由至 `DynamicBufferArena`。
2. **统一持久导入与 `UploadBuffer` 零拷贝自适应**：
   - `ImportPersistentBorrowed` / `ImportPersistentOwned` 统一接收 `GpuBuffer`：
     - 若 `!buffer.IsHostVisible()`（`DeviceLocal`）：初态读取 `GetSubmittedState()`，终态归回 `Common`；对其调用 `RDGBuilder::UploadBuffer` 走 `StagingBufferPool -> CopyBufferToBuffer` 批次路径。
     - 若 `buffer.IsHostVisible()`（`DeviceUpload` / `HostUpload`）：初态与终态均为 `HostWrite`；既支持外部在构图/录制前直接写映射内存（不调 `UploadBuffer`），也支持在图内对其调用 `RDGBuilder::UploadBuffer`（编译期检查全部 `HostWrite` 位于首次 GPU 读取前且区间互不重叠，`Prepare` 期直接向持久映射基址偏移处零拷贝写入并 `FlushMappedRange`，省去 Staging 分配与 GPU Copy 命令）。
3. **管线槽集成**：
   - `CompiledRDG::Prepare(FrameSlot& slot, RDGTransientResourcePool& transientPool, StagingBufferPool& stagingPool, DynamicBufferArena& dynamicArena) noexcept`；
   - `RenderGraphFramePipeline::SlotState` 每槽独立持有 `RDGTransientResourcePool`、`StagingBufferPool` 与 `DynamicBufferArena`，在槽回收重置命令存储后统一调用三者的 `BeginFrame()` 推进 8 槽淘汰窗口。

---

## 五、分阶段 TDD 实施路径与验收标准

### 5.1 实施阶段划分

1. **阶段 `P1`（RHI 物理内存域、拓扑能力与实时预算）**：
   - 更新 `types.h`（四态 `MemoryType`、`DeviceMemoryInfo`、`GpuMemoryBudget`、`BufferMemoryProperties`），改造 `validation.cpp`、`rhi_validation.cpp`、`d3d12_helper.cpp`、`d3d12_impl.cpp`、`vulkan_helper.cpp`、`vulkan_impl.cpp` 与外部资源扩展；
   - 先行编写/更新 `modules/gpu_impl/tests/test_buffer.cpp`、`test_device.cpp` 等单测，覆盖四态合法/拒绝矩阵、`HostUpload` 双后端 WC 属性、`DeviceUpload` 严格直通（不支持时返回空，支持时 `IsDeviceLocal == true`）、D3D12 `GPU_UPLOAD` 状态转换屏障及 `QueryMemoryBudget()` 返回值校验。
2. **阶段 `P2`（Runtime 持久缓冲 `GpuBuffer` 统一）**：
   - 重构 `gpu_resource.h/.cpp` 与 `geometry_buffer_pool.h/.cpp`，删除 `GpuUploadBuffer`，落地 `GpuBufferMemory` 四种策略与 85% 预算闸门辅助函数，补齐 CPU 假设备与双后端 GPU 单测。
3. **阶段 `P3`（`StagingBufferPool` 与 `DynamicBufferArena` 拆分及 8 槽缩容）**：
   - 新增 `staging_buffer_pool.h/.cpp` 与 `dynamic_buffer_arena.h/.cpp`，删除 `upload_arena.h/.cpp`，迁移 `transient_parameter_pool.h/.cpp`；
   - 编写单测验证：`PreferDevice` 与 `HostOnly` 物理池隔离、模拟预算超 85% 水位及 `CreateBuffer(DeviceUpload)` 返回 `nullptr` 时自动回退 `HostPage`、突发多页峰值在连续 8 次 `BeginFrame()` 空闲后精确释放物理页。
4. **阶段 `P4`（RDG 接口切分、样例/基准迁移与当前文档同步）**：
   - 更新 `rdg.h/.cpp`、`render_graph_pipeline.h/.cpp`、`readback_queue.cpp`、全部 `modules/runtime/tests/`、`examples/mesh_drawing_pipeline/`（含常量参数集按槽帧安全重绑）与 `benchmarks/bench_rdg_upload/`；
   - 更新 [RHI 与后端](../architecture/render-rhi.md) 与 [Rendering runtime](../architecture/runtime.md)，运行全量 CTest 与文档检查。

### 5.2 验收红线

1. 全仓构建零错误、零新增警告；生产代码不包含任何仅供测试使用的观测字段（水位与页计数验证均通过公开容量查询或测试侧 `FakeDevice` 探针完成）。
2. D3D12 与 Vulkan 双后端下 RHI 校验层、D3D12 Debug Layer 与 Vulkan Validation / Synchronization Validation 全量用例零报错。
3. `python tools/check_docs.py` 与 `git diff --check` 零报错通过。
