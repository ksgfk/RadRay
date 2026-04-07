根据你现在的数据结构（两种边类型 `RDGResourceDependencyEdge` + `RDGPassDependencyEdge`），重新规划所有校验项：

---

## 1. 节点基础校验

| # | 检查项 | 说明 |
|---|---|---|
| 1.1 | **Handle 有效** | 每个节点 `_id` 在 `[0, _nodes.size())` 范围内 |
| 1.2 | **Id 与索引一致** | `_nodes[i]->_id == i` |
| 1.3 | **Tag 具体化** | Resource 节点的 Tag 必须是 `Buffer` 或 `Texture`（不能是纯 `Resource`）；Pass 节点的 Tag 必须是 `GraphicsPass`/`ComputePass`/`CopyPass`（不能是纯 `Pass`） |

## 2. 边基础校验

| # | 检查项 | 说明 |
|---|---|---|
| 2.1 | **两端非空** | 每条 `RDGEdge` 的 `_from` 和 `_to` 不为 `nullptr` |
| 2.2 | **两端存在** | `_from` 和 `_to` 指向的节点在 `_nodes` 中 |
| 2.3 | **Tag 具体化** | `GetTag()` 必须是 `ResourceDependency` 或 `PassDependency`，不能是 `UNKNOWN` |
| 2.4 | **双向登记一致** | 每条边同时出现在 `_from->_outEdges` 和 `_to->_inEdges` 中；反之亦然 |
| 2.5 | **不自环** | `_from != _to` |

## 3. ResourceDependencyEdge 连接规则

| # | 检查项 | 说明 |
|---|---|---|
| 3.1 | **二分图约束** | `ResourceDependencyEdge` 两端必须是「一个 Resource 节点 + 一个 Pass 节点」，不允许 Pass↔Pass 或 Resource↔Resource |
| 3.2 | **读方向：Resource→Pass** | 若 Access 仅含读标志（`VertexRead`/`IndexRead`/`ConstantRead`/`ShaderRead`/`ColorAttachmentRead`/`DepthStencilRead`/`TransferRead`/`HostRead`/`IndirectRead`），方向必须是 Resource→Pass |
| 3.3 | **写方向：Pass→Resource** | 若 Access 含写标志（`ShaderWrite`/`ColorAttachmentWrite`/`DepthStencilWrite`/`TransferWrite`/`HostWrite`），方向必须是 Pass→Resource |
| 3.4 | **Buffer 边匹配 Buffer 节点** | 若边有 `_bufferRange`（非全默认值）且 `_textureLayout == UNKNOWN`，关联的 Resource 端必须是 `RDGBufferNode` |
| 3.5 | **Texture 边匹配 Texture 节点** | 若边有 `_textureLayout != UNKNOWN` 或 `_textureRange` 非默认值，关联的 Resource 端必须是 `RDGTextureNode` |
| 3.6 | **Stage 非 NONE** | `_stage` 不能是 `RDGExecutionStage::NONE` |
| 3.7 | **Access 非 NONE** | `_access` 不能是 `RDGMemoryAccess::NONE` |
| 3.8 | **无重复边** | 同一对 `(from, to)` 节点之间不应有 stage + access + range 完全相同的多条 `ResourceDependencyEdge` |

## 4. PassDependencyEdge 连接规则

| # | 检查项 | 说明 |
|---|---|---|
| 4.1 | **两端都是 Pass** | `PassDependencyEdge` 的 `_from` 和 `_to` 必须都是 `RDGPassNode` 的子类（Tag 含 `Pass` 标志） |
| 4.2 | **不自依赖** | `_from` 和 `_to` 不能是同一个 Pass |
| 4.3 | **不重复** | 同一对 `(from, to)` Pass 之间最多一条 `PassDependencyEdge` |

## 5. 全局图结构校验

| # | 检查项 | 说明 |
|---|---|---|
| 5.1 | **无环（DAG）** | 整图（包含两种边）不能存在有向环。拓扑排序检测 |
| 5.2 | **Pass 可达性** | 从 Import 资源（或无入边的资源）出发，图中所有 Pass 都应可达 |
| 5.3 | **无孤立 Pass** | 每个 Pass 至少有一条 `ResourceDependencyEdge`（入或出）；纯 `PassDependencyEdge` 连接但没有任何资源交互的 Pass 无法执行有意义的工作 |
| 5.4 | **无孤立 Resource** | 每个 Resource 至少有一条 `ResourceDependencyEdge`（被至少一个 Pass 引用） |

## 6. Resource 节点校验

### 6.1 通用
| # | 检查项 | 说明 |
|---|---|---|
| 6.1.1 | **Ownership 合法** | 不能是 `UNKNOWN` |
| 6.1.2 | **External 需 Import** | `ownership == External` 时，必须有 importState |
| 6.1.3 | **Internal 无 Import** | `ownership == Internal` 时，不应有 importBuffer/importTexture 和 importState |

### 6.2 Buffer 节点
| # | 检查项 | 说明 |
|---|---|---|
| 6.2.1 | **Size > 0** | Internal Buffer 的 `_size` 必须 > 0 |
| 6.2.2 | **Usage 非 UNKNOWN** | `_usage` 不能是 `BufferUse::UNKNOWN` |
| 6.2.3 | **Import Handle 有效** | External Buffer 的 `_importBuffer` 必须有效 |
| 6.2.4 | **BufferRange 不越界** | 所有引用此 Buffer 的 `ResourceDependencyEdge` 上的 `_bufferRange`，`Offset + Size` 不超过 `_size`（Size 为 `All()` 除外） |

### 6.3 Texture 节点
| # | 检查项 | 说明 |
|---|---|---|
| 6.3.1 | **维度合法** | `_dim != UNKNOWN` |
| 6.3.2 | **尺寸有效** | `_width > 0 && _height > 0 && _depthOrArraySize > 0 && _mipLevels > 0 && _sampleCount > 0` |
| 6.3.3 | **格式合法** | `_format != TextureFormat::UNKNOWN` |
| 6.3.4 | **Usage 非 UNKNOWN** | `_usage != TextureUse::UNKNOWN` |
| 6.3.5 | **Import Handle 有效** | External Texture 的 `_importTexture` 必须有效 |
| 6.3.6 | **SubresourceRange 不越界** | 所有引用此 Texture 的边上 `BaseArrayLayer + ArrayLayerCount <= _depthOrArraySize`，`BaseMipLevel + MipLevelCount <= _mipLevels`（`All` 除外） |

## 7. Pass 节点校验

### 7.1 通用
| # | 检查项 | 说明 |
|---|---|---|
| 7.1.1 | **Pass 至少有一个写输出** | 每个 Pass 至少有一条出边（`ResourceDependencyEdge` 方向 Pass→Resource，含写 Access），否则 Pass 无副作用 |

### 7.2 GraphicsPass
| # | 检查项 | 说明 |
|---|---|---|
| 7.2.1 | **`_impl` 非空** | `_impl != nullptr` |
| 7.2.2 | **Color Attachment Slot 唯一** | `_colorAttachments` 中各 `Slot` 不重复 |
| 7.2.3 | **Color Attachment Slot 连续** | Slot 值从 0 开始连续无空洞 |
| 7.2.4 | **Color Attachment Handle 有效** | 每个 `Texture` Handle 有效且对应节点是 `RDGTextureNode` |
| 7.2.5 | **Color Attachment 非深度格式** | 引用的 Texture 格式不能是深度/模板格式 |
| 7.2.6 | **DepthStencil 是深度格式** | 若有 `_depthStencilAttachment`，Texture 格式必须是深度/模板格式 |
| 7.2.7 | **Color 与 DepthStencil 不冲突** | 同一 Texture 不能同时出现在 Color Attachment 和 DepthStencil Attachment 中 |
| 7.2.8 | **Clear 需 ClearValue** | `Load == Clear` 时 `ClearValue` 必须有值 |
| 7.2.9 | **边 Stage 范围** | 此 Pass 的 `ResourceDependencyEdge` 的 Stage 只含 `VertexInput`/`VertexShader`/`PixelShader`/`DepthStencil`/`ColorOutput`/`Indirect`，不含 `ComputeShader`/`Copy` |

### 7.3 ComputePass
| # | 检查项 | 说明 |
|---|---|---|
| 7.3.1 | **`_impl` 非空** | `_impl != nullptr` |
| 7.3.2 | **边 Stage = ComputeShader** | 此 Pass 的所有 `ResourceDependencyEdge` 的 Stage 只能是 `ComputeShader` |
| 7.3.3 | **Access 无图形管线参数** | 不应出现 `VertexRead`/`IndexRead`/`ColorAttachmentRead`/`ColorAttachmentWrite`/`DepthStencilRead`/`DepthStencilWrite`/`IndirectRead` |

### 7.4 CopyPass
| # | 检查项 | 说明 |
|---|---|---|
| 7.4.1 | **`_copys` 非空** | 至少一条 copy 记录 |
| 7.4.2 | **边 Stage = Copy** | 此 Pass 的所有 `ResourceDependencyEdge` 的 Stage 只能是 `Copy` |
| 7.4.3 | **Access 限 Transfer** | 只能是 `TransferRead` 或 `TransferWrite` |
| 7.4.4 | **Copy Record Handle 有效** | 每条 `RDGCopyRecord` 中的 Src/Dst Handle 有效且节点类型匹配 |
| 7.4.5 | **Copy Src ≠ Dst** | Buffer→Buffer 时 Src 和 Dst 不能是同一节点（或范围无重叠） |

## 8. Import / Export 校验

| # | 检查项 | 说明 |
|---|---|---|
| 8.1 | **Import Stage/Access 非 NONE** | Import 的 `Stage` 和 `Access` 至少指定一种 |
| 8.2 | **Import Texture Layout 非 UNKNOWN** | Import Texture 的 Layout 不能是 `UNKNOWN` |
| 8.3 | **Export Stage/Access 非 NONE** | Export 的 `Stage` 和 `Access` 至少指定一种 |
| 8.4 | **Export Texture Layout 非 UNKNOWN** | Export Texture 的 Layout 不能是 `UNKNOWN` |
| 8.5 | **Export 必须有写入源** | 被 Export 的 Resource（Internal）必须至少有一条 Pass→Resource 写入边，否则导出的内容未定义 |
| 8.6 | **Export Handle 存在** | Export 的目标 Handle 必须在图中存在且类型正确 |

## 9. Stage × Access 兼容性

| Stage | 允许的 Access |
|---|---|
| `VertexInput` | `VertexRead`, `IndexRead` |
| `VertexShader` | `ConstantRead`, `ShaderRead`, `ShaderWrite` |
| `PixelShader` | `ConstantRead`, `ShaderRead`, `ShaderWrite` |
| `DepthStencil` | `DepthStencilRead`, `DepthStencilWrite` |
| `ColorOutput` | `ColorAttachmentRead`, `ColorAttachmentWrite` |
| `Indirect` | `IndirectRead` |
| `ComputeShader` | `ConstantRead`, `ShaderRead`, `ShaderWrite` |
| `Copy` | `TransferRead`, `TransferWrite` |
| `Host` | `HostRead`, `HostWrite` |
| `Present` | 无 Access（或仅只读语义） |

当 Stage 是组合标志（如 `VertexShader | PixelShader`）时，Access 必须是对应 Stage 允许集合的**并集**子集。

## 10. Layout 一致性校验（Texture 特有）

| # | 检查项 | 说明 |
|---|---|---|
| 10.1 | **同 Pass 内同 Subresource 不能有不兼容 Layout** | 同一个 Pass 引用同一 Texture 的重叠 SubresourceRange 时，所有 `ResourceDependencyEdge` 上的 `_textureLayout` 必须相同（或其中一个是 `General`） |
| 10.2 | **Layout 与 Access 匹配** | `ColorAttachment` ↔ `ColorAttachmentRead/Write`；`DepthStencilAttachment` ↔ `DepthStencilRead/Write`；`DepthStencilReadOnly` ↔ `DepthStencilRead`；`ShaderReadOnly` ↔ `ShaderRead`/`ConstantRead`；`General` ↔ 任意读写；`TransferSource` ↔ `TransferRead`；`TransferDestination` ↔ `TransferWrite`；`Present` ↔ 无写入或只读 |
| 10.3 | **Import→Export Layout 连贯** | 如果 Texture 同时有 Import 和 Export，且中间没有 Pass 使用它，Import Layout 应与 Export Layout 一致（否则缺少转换） |

## 11. 数据冒险检测（Hazard）

| # | 检查项 | 说明 |
|---|---|---|
| 11.1 | **Write-After-Write** | 同一 Resource 的重叠范围（Buffer: BufferRange 重叠；Texture: SubresourceRange 重叠）不能被**多个 Pass 写入**，除非这些 Pass 之间有拓扑序保证（通过 ResourceDependencyEdge 中转或 PassDependencyEdge 直连） |
| 11.2 | **Read-After-Write** | 读取 Pass 必须在写入 Pass 之后（拓扑序）。Internal Resource 被读但无任何写入来源→报错（数据未初始化） |
| 11.3 | **Write-After-Read** | 写入 Pass 必须在读取 Pass 之后（拓扑序），否则会覆盖未消费的数据 |

检测方法：对每个 Resource 收集所有关联的 `ResourceDependencyEdge`，按边方向分为读集合（Resource→Pass）和写集合（Pass→Resource），然后：
- 写集合内的每对 Pass 之间需有拓扑序关系（通过 `PassDependencyEdge` 或资源中转可达）
- 写集合与读集合之间的每对 Pass 之间需有拓扑序关系
- 范围不重叠的访问可以豁免

## 12. PassDependencyEdge 特定校验

| # | 检查项 | 说明 |
|---|---|---|
| 12.1 | **不冗余** | 如果两个 Pass 已经通过 ResourceDependencyEdge（经由某个 Resource 中转）有明确的先后关系，额外的 PassDependencyEdge 虽不违规，但可以**警告**冗余 |
| 12.2 | **不矛盾** | PassDependencyEdge 指定的顺序不应与 ResourceDependencyEdge 推导出的拓扑序相反（若 A 通过资源依赖必须在 B 之后，则不能有 `A→B` 的 PassDependencyEdge） |
| 12.3 | **环检测含 PassDependency** | DAG 检测必须将 PassDependencyEdge 纳入。Resource 中转边 `Pass→Resource→Pass` 等价于 Pass 间一条间接边，与 PassDependencyEdge 合并后做环检测 |

---

**建议实现顺序**：

1. **第一级（结构正确性）**：2 → 3 → 4 → 5.1 — 保证边连接合法，图是 DAG
2. **第二级（节点合法性）**：1 → 6 → 7 → 8 — 保证各节点自身数据有效
3. **第三级（语义正确性）**：9 → 10 → 11 → 12 — 保证 barrier/同步推导的输入正确