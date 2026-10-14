> - 适用: 2026-10-13 GPU 生命周期实施任务的输入计划；历史快照
> - 权威: 仅保留原始计划及 7a807 基线，不替代 architecture/guide 当前契约；执行证据见 [验收记录](gpu-lifecycle-implementation-2026-10-13.md)
> - 锚点: `modules/runtime/src/gpu_system.cpp`, `modules/runtime/src/gpu_frame_resources.cpp`, `modules/render/src/vk/vulkan_render_pass_cache.cpp`

# RadRay GPU 生命周期与架构收缩：完整实施计划

状态：待实施的设计与验收计划；本文件不是实现结果。
日期：2026-10-13。
仓库：ksgfk/RadRay。
分支：refactor/render-framework-reset。
固定基线：7a807f037c5e15045bf2fc6c702d8819842b9ed5。
本轮通过 GitHub 插件复核分支及关键代码。容器 git clone 因 DNS 解析失败未成功；没有本地构建、GPU 测试或性能测量。

本计划替代前两轮文件中有关 dynamic rendering 最低要求、单份 Scene GPU buffer 实验、以及实施排序的建议。前两轮资源盘点仍可作为历史依据，不作为与本文并列的执行合同。

## 0. 已确定的范围

### 0.1 两个不可改变的前提

1. Vulkan 以 **1.2** 为基线。基础渲染路径不能依赖 Vulkan 1.3、VK_KHR_dynamic_rendering、synchronization2 或 imagelessFramebuffer。现有功能确实需要的 1.2 feature 仍必须查询并显式启用；“1.2 兼容”不等于忽略所有 feature 条件。
2. Scene 保留 **每个物理 flight 独立的矩阵和实例参数 GPU buffer**。不实施单份主 buffer，不把它列为本轮基准实验、可选阶段或交付后的默认切换候选。

### 0.2 仍然实施的架构减法

- 公共 RHI 用附件描述表达一次 rendering scope；不再要求 runtime 创建、缓存和退休独立 RenderPass/Framebuffer owner。
- Vulkan 原生 VkRenderPass/VkFramebuffer 保留，缓存收进 DeviceVulkan 私有实现，以命令持有的类型化引用控制使用期，以有界空闲缓存控制复用。
- 必要更新与可丢弃输出分离；必要更新被 flight 接纳以后，不因窗口隐藏或后续绘制失败被撤销。
- 压平 per-flight uploader/staging 内部重复的 flight 管理。
- 稳定的 Scene 绘制模板归 Scene；材质 pass 绑定计划归明确共享的不可变 pass 定义。
- 审计并迁移全部使用者以后，删除 AssetManager 内的 GPU 延期等待子系统。
- 收敛完成通知的重复事实，但保留 CPU-only、GT/RT、独立宿主、外部设备和 WSI 的真实边界。

### 0.3 明确不做

不新增全局 GpuResourceManager；不把 Scene、资产、staging 搬入一个全局 type-erased 列表；不把所有 RHI 资源改成 shared_ptr；不建立每个 GPU 小对象独立 fence；不引入逐 draw 保活、正常帧 GPU idle 或全量资源扫描。

本轮不实现图内显存 aliasing、多队列调度、设备丢失恢复、完整 TAA/历史效果、完整资产 GPU importer、任意半条原生命令回滚、运行中 shader/service 整代热退休。支持 Vulkan 1.2 的传统 render-pass 路径是完整主路径，不先维护另一套 dynamic-rendering 实现。

---

## 1. 当前源码事实与目标差异

| 当前事实 | 本轮目标 | 必须保留的部分 |
|---|---|---|
| Material 已有 MaterialGpuState，默认常量和静态 SPS 已能持久复用 [S4] | 把绑定计划的长期归属也从 RasterDraw 移回共享 pass 定义 | Material 的不可变版本、默认纹理依赖、静态组复用 |
| SceneGpuData 为每 flight 管理矩阵和参数流、Pending/Attempt [S3] | 保留物理布局；必要更新不可撤销后删除专用于跳过更新的 Attempt 回补 | F 份 buffer、F 份 dirty、generation、同 serial 最多准备一次 |
| 本机窗口隐藏可能丢弃全部应用工作，外部目标只丢关联 batch [S1] | 两类目标采用相同的“仅丢关联输出”政策 | acquire/present 收尾、真实 final fence |
| 每槽 uploader 又含 pending[flightCount] [S1,S5] | 每槽一套临时上传存储，不在其内部再次路由 flights | 页池、线性切片、大页和可恢复批量暂存 |
| SceneDraw 维护自己的场景记录及 revision 修复扫描 [S6] | Scene 持有 view 无关模板，Apply 时局部失效 | 每 view 剔除、排序、合批和动态参数 |
| RHI 有独立 RenderPass/Framebuffer 工厂；WindowManager 通知 registry [S7,S8] | 公共对象层删除；VulkanDevice 内保留原生缓存 | MRT、MSAA、depth/stencil、load/store、显式 barrier |
| Vulkan 已有 Device-scoped intrusive layout cache 的实现风格 [S9] | 复用风格，不复用“零引用立即销毁”作为唯一缓存政策 | 专业 typed cache，不生成万能缓存框架 |
| TextureAsset 用 AssetManager::DeferDestroy，StaticMesh 已使用调用方保活 [S10] | 所有合法纹理使用者接入完成后删除额外 GPU 等待 | 资产零引用候选、GT-only 非原子引用 |
| FrameTimeline 同时有 completion channel、WaitersCompleted 和记录上的 FlightComplete [S11] | 等待绑定明确工作身份，完成账本只更新一次 | 取消、固定批次截止、协程重入保护 |

上述是固定基线上的事实；以下名称除现有类型外均为职责示意，实施时优先扩展现有结构，不机械地按每个概念建立新类。

### 1.1 资源接入清单（现状 → 目标）

“当前回收 → 目标”不表示所有位置都必须移动；已经符合合同的 typed 容器继续保留。

| 资源/分配 | 创建入口 | 语义 owner | 逻辑寿命 | 更新方式 | 在途引用 | 回收条件 | 当前回收 → 目标 |
|---|---|---|---|---|---|---|---|
| 默认材质常量 | PersistentCBufferArena | MaterialGpuState | 持久版本 | 首次分配；材质变化换版本 | 资产/flight 保留材质依赖 | 最后合法使用结束再归还 range | Block 析构 → 保留，补齐外部合同 |
| 材质静态 SPS | RasterDraw 直接 Create | MaterialGpuState | 持久版本 | 初始化后复用 | 材质版本及 texture/layout 依赖 | 录制和 GPU 使用结束 | Group 析构 → 持久创建入口归并 |
| view/frame/mixed SPS | GpuFrameResources | frame pool | 帧内版本 | 每借出完整写入 | 当前完整 epoch | 全部批次终结、旧录制失效 | ResetForReuse → 完成协议控制的池复用 |
| 动态常量/storage 切片 | frame arenas | 当前工作域 | 帧内 | reserve/commit | 对应命令、SPS | final completion 且借用结束 | frame Reset → 组合存储统一管理 |
| staging 切片 | uploader/staging pool | 当前工作域 | 帧内 | 完整暂存后 copy | 已接纳 copy | GPU 不再读取 | pending[F]/Collect → 单 epoch 页管理 |
| Scene 矩阵 buffer | SceneGpuData::Prepare | SceneGpuData | 持久、每 flight 独立 | dirty 更新；安全槽扩容 | Scene 使用和删除锚点 | 各槽安全；删除覆盖全部旧使用 | Flight buffer/RetiredGpu → 保留物理布局和 typed 退休 |
| Scene 参数流 buffer | SceneGpuData::Prepare | SceneGpuData | 持久、每 flight 独立 | 各 stream dirty 更新 | 同上 | 同上 | ParameterFlight → 私有实现归并，不合并副本 |
| mesh vertex/index | 构造方/uploader | StaticMesh | 持久 | immutable 版本替换 | Scene/use refs 或手工 flight owner | 使用者保活结束后零引用 | OnUnload → 保留 |
| texture 与 SRV | 构造方/TextureAsset 子 view 工厂 | TextureAsset/外部 owner | 持久版本 | 按需创建或替换 | 材质、手工/override、外部任务 | 使用协议完成；views 先于 texture | DeferDestroy → 调用者迁移后删除额外等待 |
| 持久目标 texture/view | 现有 owner 直接创建 | 目标 owner | 持久版本 | resize 等按变化换代 | 使用它的所有工作域 | 旧 epoch 结束 | 明示 owner 回收 → 接入共同退休合同 |
| 帧内目标 texture/view | 本轮 typed pool | frame domain | 帧内版本 | 本轮写入 | 全部使用批次 | 本轮 final completion | 补齐池入口，不新增各目标延期器 |
| 历史/readback 结果 | 过程 owner；profiler 已有专用实现 | 历史/消费过程 | 结果版本跨帧保留 | producer 产生新代 | GPU producer/reader 与 CPU consumer | 保留权结束且全部相关使用结束 | 各用途合同 → 同一完成机制、显式消费/预算 |
| QueryPool/profiler readback 存储 | GpuFrameProfiler 初始化 | profiler | 持久容器，样本有结果寿命 | 每 flight 生成和 Resolve | 计时命令和 CPU 读取 | 完成后消费，再使用存储 | Resolve → 保留专用职责 |
| command wrapper/native storage | GpuFlightCommandAllocator/RHI | flight allocator | 容器持久、epoch 帧内 | 每 epoch Begin/End | 全部登记批次与内部命令 | 全部 GPU 使用结束且不再提交旧录制 | 下一 Begin Reset → 完成边界先结束旧 epoch |
| 原生 Vulkan RP/FB | 当前 RHI 工厂 → Device cache | Device cache，非业务 owner | 缓存存储；使用引用属于录制 epoch | typed key 复用 | 创建临时引用、command pins | 使用引用结束；idle 满足淘汰政策 | runtime registry → VulkanDevice 内部 |
| shader/layout/PSO | 专业服务缓存 | RenderSystem | 持久缓存 | 按请求创建/失效 | Material/command/service 使用者 | service 停止、drain、依赖结束 | OnShutdown → 保留专业缓存，PSO 去掉 RP 指针身份 |
| sampler | device cache | Device | 持久缓存 | 按 descriptor 去重 | SPS/原生布局/命令 | consumer 结束后关闭 provider | cache Clear → 保留 |
| backbuffer/acquire 同步 | swapchain 与窗口/外部 acquire | 原 swapchain/WSI；runtime 借用 token | 按其真实版本和 acquire 使用域 | acquire/呈现/重建 | 目标批次及 WSI | GPU 与 WSI 各自条件满足 | 原后端处理 → 保留，不转成普通 frame texture owner |

## 2. 目标职责图

```text
GT 语义 owner
  Material / SceneWriter / 资源任务 / 渲染目标 / 历史或 readback 过程
  ├─ 持有自己的真实资源或资产引用
  ├─ 决定何时停止后续使用
  └─ 将本次工作需要的 owner/退休包交给相应工作域
                 │ CPU 发布
                 ▼
RT
  RenderScene：稳定 CPU 数据、共享绘制模板
  SceneGpuData：每 flight 独立 GPU 镜像、增量更新
  Material/pass GPU state：绑定计划与持久默认参数
  Pipeline/SceneDraw/RasterDraw：准备、选择、编码
                 │ 使用当前工作域服务
                 ▼
GpuSystem / GpuFlightSlot
  command allocator、frame resources、已接纳的必要工作、输出批次、完成凭据
                 │ RHI 附件值描述和命令
                 ▼
D3D12 后端                    Vulkan 1.2 后端
直接映射附件                  DeviceVulkan 私有原生 RP/FB 缓存
                              命令 epoch 持有 typed 引用
```

RenderSystem 保留 program/PSO 等专业服务；不再拥有 RenderPassRegistry。DeviceVulkan 不知道 Material、Scene、Window 或 FrameSerial，不消费 runtime 完成消息，也不轮询这些缓存条目的 GPU fence。

## 3. 统一生命周期合同

### 3.1 合同标在版本和分配上

| 类别 | 逻辑退出由谁决定 | 物理复用条件 |
|---|---|---|
| 持久 | Material/Scene/目标/缓存等 owner 停止使用某版本 | 不再允许新使用；既有录制已提交或丢弃；全部提交使用完成 |
| 跨帧保留 | 历史窗口推进、CPU 消费、异步过程结束 | 保留权结束，并且相关读写使用已经完成 |
| 帧内 | 一次完整 flight 实例的工作结束 | 全部批次及内部收尾完成，未提交录制已处理，借用已经结束 |

容器常驻不使切片变成持久。历史容器常驻不要求它与每代结果建立两套物理 owner。持久资源进入退休阶段也不改称跨帧资源。

### 3.2 三件事不合并

逻辑释放阻止新使用；退休证明旧使用结束；回收才是释放对象、归还 range、重置 descriptor 或再次借出。引用计数、有效性 generation、FrameSerial 都不能独自代替 GPU 完成或 barrier。

### 3.3 两个 GPU 安全边界

**封口边界：** 本轮命令全部已处理、目标 token 有合法归宿、版本和依赖 owner 已确定、必要主机写入 flush、必要工作和可跳过输出已隔离，随后执行有序提交并建立 final signal。

**完成消费边界：** 核验 final signal 和工作身份，结束旧命令 epoch，处理结果，释放资源依赖和语义退休包，然后允许槽位下一次借出。

GT 的 CPU 发布仍独立存在。RT 可以在 CPU 发布后创建绑定；普通参数集一旦交给录制使用就不再原地修改，不能把“尚未 Submit”当作任意更新 descriptor 的许可 [N8]。

### 3.4 录制引用的结束必须显式

runtime 的一个 command 录制版本只属于一个 flight，不支持后续逻辑帧重放。归还只表示关闭；登记才表示已交付；discard 表示取消提交资格；Reset 才结束共享 native storage 的旧 epoch。

RHI 独立使用者仍可以在其合法合同下自行组织提交，但必须在所有使用完成后再 Reset；RHI 不因 runtime 有 final fence 就擅自判定独立使用者安全。

## 4. Vulkan 1.2 与公共 RHI 迁移

### 4.1 兼容性检查不是只改 apiVersion

P0 必须检查 instance 版本协商、物理设备最低版本、device feature/pNext 链、加载的函数、VMA 配置、shader SPIR-V target environment，以及 Query/Barrier/Submit/WSI 的实际入口。

传统路径使用 1.2 可用的 render pass/framebuffer 与同步 API。禁止在实际 1.2 设备上无条件调用 1.3 入口或无条件启用 1.3 feature struct。现有 timeline semaphore 等实际需要的 feature 应查询并启用；不支持时明确启动失败，不用无意义 fence 值模拟完成。

外部附加的 RHI Device 使用它已启用的能力，不重建设备、不擅自启用 feature。基础测试必须能在 dynamicRendering 和 imagelessFramebuffer 未启用时完成。

imagelessFramebuffer 虽已进入 Vulkan 1.2，但仍有独立 feature 条件 [N5,N6]。本轮以普通 framebuffer 为完整路径，不以 imageless 降低缓存设计难度后将其变成隐含必需条件。

### 4.2 公共接口表达一次渲染操作，不公开缓存 owner

演进当前 RenderPassBeginDescriptor，直接携带有序 color views、可选 depth/stencil view、load/store/clear、read-only 条件、render area、framebuffer extent/layers。描述是输入数据，不是一个可跨帧持有并手工 Destroy 的 framebuffer。

PSO 使用附件兼容签名。复用/归并现有 ColorTargets、DepthStencil、MultiSample 中的格式/采样信息，保证只有一份权威值；若暂时保留重复字段，迁移期间必须检查一致，并在切换完成时删除重复来源。

目标删除 public RenderPass/Framebuffer owner 类型及 Create 工厂、RenderPassRegistry 和 runtime 的 RemoveFramebuffersUsing。保留 Begin/End 渲染范围的语义及必要的值描述。

D3D12 沿当前附件访问语义直接编码；不为接口对称制造另一份无实际原生对象的长期 framebuffer cache [N9]。

### 4.3 将后端缓存 miss 的失败也放到本工作单元编码前

建议增加一个非常小的命令局部准备入口：

```text
CommandBuffer.PrepareRenderPass(附件描述)
  → 校验输入
  → Vulkan 获取 RP/FB 引用，D3D12 准备附件访问数据
  → 把 ready 数据保存于当前 command epoch
  → 返回 command/epoch/index 身份的非拥有 token

CommandBuffer.BeginRenderPass(token)
  → 只编码已准备数据，不再创建 RP/FB
```

token 不拥有资源、不公开 Vulkan 句柄、不允许跨 command/epoch 使用，不新增 runtime registry，也不要求 pipeline 输入外部 command；pipeline 仍从 IGpuCmdAllocator 取得命令。它替代两种长期 RHI owner，不是第三个长期 owner。

需要同一条独立命令承载多个 graph 节点时，先准备全部所需 scopes 再编码。准备失败时命令没有这些节点的半成品；此前独立接纳的资源更新仍有效。CPU 分配失败等现有不可恢复错误不通过新增 catch-all 隐藏。

### 4.4 三种 key 必须分开

| key | 包含 | 不包含 |
|---|---|---|
| 附件兼容签名 | 当前受支持单 subpass 的有序附件角色/格式/采样、depth/stencil 存在性等 | 实际 view、尺寸、clear 值、load/store |
| RenderPass 原生执行 key | 规范化后的实际 VkRenderPass 创建行为：兼容签名、load/store、layout、read-only 映射和依赖等 | clear 具体值、view、framebuffer 尺寸 |
| Framebuffer key | 兼容签名、有序稳定 view 身份、实际 extent 和 layers | clear 值、viewport/scissor；不把等价 RP 指针作为身份 |

兼容签名只描述当前明确支持的单 subpass 模型。依赖形状、attachment mapping 等未进入签名的条件必须是实现固定的不变量；不允许“只比较格式/采样”被当作任意多 subpass 的一般兼容算法。未来增加 resolve/input attachment/multiview 等能力时必须扩展签名或明确拒绝。

Vulkan 允许兼容 render pass 共享 framebuffer 和 pipeline，load/store 与若干 layout 差异不影响兼容性 [N1]。因此修改 clear 值不应产生新 RP/FB/PSO；修改 load/store 可以产生新的 RP 执行条目，但不应仅因此产生新 PSO 或不必要的 FB。

实际附件 layout 与外部 barrier 的职责仍清楚：当前 graph/调用方控制跨 scope 的 transition；缓存生成与合同一致的原生 initial/final/subpass layouts，不偷偷代替状态管理，不因 cache hit 省掉同步。

### 4.5 PSO 不长期持有“创建时那个 RenderPass”

创建 Vulkan PSO 时，从同一 RP cache 临时取得一个兼容的代表 render pass，在 vkCreateGraphicsPipelines 返回以后释放这份创建引用；PSO 保存兼容签名，不借用该 cache entry 的地址。

Framebuffer 创建也只需要创建调用期间的兼容 RP；不要人为添加 FB→某个 RP entry 的永久引用链。规范明确限定 VkGraphicsPipelineCreateInfo 和 VkFramebufferCreateInfo 中该 renderPass 的实现访问期 [N2,N3]。

实际 BeginRenderPass 所用的 RP 与 FB 则分别由 command epoch 持有。创建期引用和录制/执行期引用不可混同。

## 5. VulkanDevice 原生缓存的完整所有权设计

### 5.1 两张 typed 表，不是总资源管理器

使用 DeviceVulkan 私有实现中的 RP 和 FB 两张表。可放入一对同名 .h/.cpp 的私有缓存文件，按工程约定维护。条目包含规范化 key、native handle、使用引用计数和 idle-list 链接；不保存 FrameSerial/fence、不理解业务 owner。

采用已有 IntrusivePtr 风格即可，但必须明确 cache residency 与使用引用不同。建议表物理拥有 entry（例如 unique_ptr），引用计数只统计外部准备/命令使用。计数归零后条目可留在有界 idle cache；它不代表表已经失去对象。

不要让每个条目持有 shared_ptr<DeviceVulkan>，否则 Device→cache→entry→Device 形成循环。条目借用 Device；关闭 Device 的前提是其全部使用引用先结束。

### 5.2 引用取得和释放

1. 准备 pass 时取得 RP/FB 使用引用，即使尚未录制/提交也必须保留。
2. 引用进入当前 command 的 typed ready/keepalive 存储。一次 command 对同一条目去重，不逐 draw 增减。
3. EndRenderPass、CommandBuffer::End、Return、Submit 返回、Present 返回均不释放这些使用引用。
4. runtime 完整 flight 的 final signal 完成后，原生录制被 Reset/失效，再批量释放这些引用。
5. 明确未提交的取消命令也先结束其录制资格。共享 allocator 中还有在途兄弟命令时，不允许为取消单条 command 重置整个 native pool；其引用可保守保留到同一 epoch 的安全结束。
6. 独立 RHI 使用者在其全部提交完成且不再重放后 Reset，同样释放引用。

“引用数为零能安全淘汰”的理由是：所有会用到 native 对象的录制/创建都纳入引用，而且命令引用只有在安全结束后才释放；不是 Vulkan 会自动为 CPU 引用计数等待 GPU [N4,N7]。

### 5.3 缓存条目不偷偷拥有 attachment 的业务寿命

Framebuffer cache 不持有 StreamingAssetRef，不因一个 idle FB 把纹理、readback、backbuffer 或外部 surface 长期保活。纹理和 view 的使用期仍由目标 owner、frame pool 或粗粒度 flight owner 覆盖。

基线方案：

- 每个 ImageView 的物理版本取得 Device 内单调递增的稳定 ID；key 不仅用 wrapper 地址或 VkImageView 数值，防止地址/句柄复用误命中。
- 维护 view→相关 FB 条目的局部反向关联。不是每帧扫描全部 framebuffer。
- 实际销毁 view 前，后端自动移除关联 idle FB，再销毁 native view；上层 WindowManager 无需通知一个 runtime registry。
- 若物理销毁时仍存在使用该 view 的活跃 command pin，说明调用方未满足 owner 合同：在驱动调用前诊断，不能为了让断言通过提前清引用或等待整个 GPU。
- 逻辑关闭 view 版本时可以先禁止新 lookup；物理 owner 仍由既有退休路径保持到引用和 GPU 使用结束。

RHI 原始/外部借用调用者仍必须履行使用者保活合同。此方案不试图让任意时刻直接 Destroy 或宿主绕过包装销毁 native handle 自动变安全。

### 5.4 完成顺序是本迁移的前置任务

当前代码在完成消息消费时清 Payloads，而命令 allocator 的 Reset 发生于下一次 BeginFrameRecord [S1]。加入命令持有原生缓存引用后，不能照搬这一顺序。

目标顺序：

```text
确认本 flight 全部提交及内部收尾完成，CPU Submit/Present 已结束
→ 禁止旧 command 再提交，核验未归还/未处理录制
→ Reset 当前 RHI command allocator，失效所有旧 token/recording
→ 清理 ended encoders，释放 command 的 RP/FB/其他临时原生引用
→ 消费需要在临时数据重用前完成的 profiler/result 数据
→ 向 GT 交付匹配的完成
→ 释放 Scene/Material/目标等 typed 退休 owner 与 frame payload
→ 按合同 reset/trim frame 临时存储
→ 槽位转为下一次可借出
```

结果读取与 command Reset 的顺序可根据实际依赖在同一完成边界内调整，但必须保证任何结果存储被重用前完成读取，并且 native command 依赖先于它们引用的 view/texture owner 释放。Reset 移到完成阶段后，Begin 不再二次 Reset，也不能自动将尚未消费完成的槽位置为 writable。

此任务同时更新 Application 的完成消费和独立宿主示例，避免 SceneManager 先清 RetiredGpu、GpuSystem 才清命令依赖。

还有一个顺序条件：当某个后续 flight 作为旧 owner 的退休锚点，其 final fence 覆盖更早的提交时，必须先结束这一已完成提交前缀中的旧 command epochs，再释放该锚点的 owner。不能先消费后续 flight 的删除包，却把更早、已完成但未 Reset 的命令引用留在缓存里。当前单主队列可按提交顺序处理至多 flightCount 个槽位；这是有界槽位处理，不是全资源扫描。不同宿主不能用一个全局最大 FrameSerial 推断对方的 epoch 已结束。

### 5.5 空闲预算、线程和关停

idle entries 使用容量预算和明确 Trim 策略，不能零引用即无条件删除导致单 flight 每帧重建，也不能 Device 活着就永不清理。

缓存计数/索引/idle 变更通过私有同步保护。首版采用一个缓存级互斥量协调 Acquire/Release/Invalidate/Trim 和两个 typed 表即可；独立 allocator 不共享录制内存。释放在 epoch 边界批量完成，Draw 不碰 cache 锁。冷 miss 的 native 创建可先串行化，只有实际基准显示瓶颈才分解锁。

锁不覆盖 GPU wait，也不依赖“只有一个宿主 RT”这一隐含前提。当前宿主间队列串行约定仍保留；多个宿主共享 device cache，不共享 FrameSerial 完成账本。A 宿主完成只能释放 A 的命令引用，B 的引用仍使对象不可淘汰。

cache idle 预算限制条目/已知主机开销，不能把不可见的驱动内存精确声称为已知。活跃 pin 不因预算被销毁。硬上限不足时在 Prepare 之前淘汰 idle 或返回可恢复失败；不正常帧 WaitIdle。空闲条目淘汰从候选表进行，不全量扫描 live resources。

最终 Device 关闭必须停止所有消费者，完成所有相关 queue/WSI 使用，结束全部 command epochs、释放资源 owner、清空原生缓存，然后销毁 device。仍有 pin 时是合同错误，不强行清缓存后继续使用设备。

## 6. 提交政策：必要工作不可撤销，可选输出独立

### 6.1 接纳是资源更新的提交承诺

```text
Validate/Allocate/Stage
  失败 → dirty 保留，目标未修改
Record 独立更新 command
Return/关闭
RegisterRequired（flight 同步接管；从这里起正常路径必须执行）
  → 更新安排状态与 dirty 提交
Submit（可合并批次）
真实完成 → 回收 staging、命令存储和结束的 owner
```

IGpuCmdAllocator 仍只负责分配、关闭和明确处理录制，不能把 Submit 语义塞进 allocator。PipelineContext 可借用一个很窄的必要工作登记入口，由现有 AppFrameContext 实现/转接；它只登记本 flight 的关闭命令，不是新 scheduler。

必要更新不能只放进 PipelineRecordResult 中等调用方“有空再登记”，同时已经清空 dirty。要么当场完成不可撤销接纳，要么在接纳前不得提交镜像安排状态。

### 6.2 不建立额外调度图

继续沿用现有有序 batch 序列，区分必要资源工作与关联目标的可选输出即可。必要工作不得依赖可被跳过的 producer；有真实可选依赖的操作必须与该 producer 一同处理或保留明确结果条件。

窗口隐藏、输出无效、绘制准备失败，只影响对应输出。保持其他必要上传与独立离屏工作的合法顺序。合并批次不能跨越外部 wait/signal、目标呈现及必要的依赖边界。

### 6.3 统一 AppWindow 与外部目标，但保留平台职责

AppWindow 在 WindowManager 层生成相同的 borrowed target 描述，包含 swapchain、真实 token、backbuffer/view、初始状态、可呈现检查和结果通知。GpuSystem 不再因为 Window 与 ExternalSwapChain 两个字段采用不同丢弃政策。

resize/recreate/窗口操作的现有安全维护仍保留；不能让原始窗口 API 绕过阶段。Vulkan 的 acquire 同步、未执行输出时的必要 transition/收尾、以及 present semaphore 的重新使用按照现有后端合同处理。graphics final fence 不等于 WSI 呈现完成 [N10]。

### 6.4 完成和结果数据

FlightCompletion 表示特定工作域已安全终结。必要工作的执行保证来自接纳合同；history/readback/可选输出另外保存实际需要的状态：是否接纳/提交/跳过、producer 内容有效性、CPU 消费状态和 Present 结果。

不增加逐 draw receipt，也不用一个 completed/Rendered 布尔值表示所有事情。只有确实有消费者的任务记录 outcome。若保留原 GpuWorkCompleted 字段用于迁移，必须标明其旧语义，并在调用方迁移后删除或改成明确字段；不能静默改其含义。

D3D12/Vulkan 原生提交故障按项目真实错误合同进入终止/错误路径，绝不通过发布“成功完成”掩盖。用户取消任务或等待不撤销已接纳工作，也不提前释放 owner。

## 7. Scene GPU 镜像：保留副本，缩小状态机

### 7.1 保留的数据

每 Scene 仍有一个 SceneGpuData；每 flight 仍分别有矩阵 buffer、各参数 stream buffer、容量、generation、dirty/Pending 和本 serial 的准备结果。GT 发布 CPU Scene，RT Apply 增量，F 个镜像各自追上最新版本。

扩容只发生在已经安全借出的目标 flight 的首次准备。其他 flight 的 buffer 不变。本次 view 取得的绑定不能在后续 view 准备时被原地替换；PreparedSerial 保留“一轮最多准备一次”的合同，包含空结果和失败。

### 7.2 普通更新状态机

每个目的 stream 是一个更新单元：完整校验/暂存其全部递增不重叠范围，再录制、关闭、接纳。接纳成功后清除本次 dirty，并提交“该内容已安排”的状态。这个状态不是 GPUCompleted，不允许用它回收内存。

首次和扩容需要完整初始化该镜像，稳定阶段只访问 dirty。必要的 Before/After barriers 和 host visibility 不变。完整 ShapeId generation 继续参与去重和删除，旧 ID 不更新新行。

### 7.3 失败和部分成功

同一目的 buffer 的一组 TryUploadBufferRanges 失败，不能录制一半目的修改。不同 stream 之间不强制全 Scene 原子事务：A 已完整接纳、B 准备失败时，A 保留且执行，B 保留 dirty，相关 draw 可以暂时缺少完整 SceneGpuView 而跳过。

不得因为 B 失败而将 A 的有效更新撤销；也不得把 B 标为已经初始化。扩大 buffer 的候选对象在接纳前失败，应保留旧合法版本或明确保持未初始化；不能交出一个被误认为有效的新 buffer。

### 7.4 可以删除的状态及条件

只在所有普通 Scene 更新均走不可撤销接纳、退出/停止路径也得到证明后，删除用于“本次上传可能整体被跳过”的 Attempt/AttemptSerial/AttemptGeneration、CompletedExecuted 等回补字段和 GT→RT 绕行。

PreparedScenes 若在固定基线只用于投递这类反馈，可以删除；若实施前发现有新增用途，只删除反馈职责，不按字段名机械删除。SceneGPU 的 RetiredGpu typed 容器继续保留，它承载真实语义退出，不是多余的 fence 调度器。

矩阵与参数流的容量、dirty、准备过程可以收敛到一个私有 stream 实现，但不构造公共万能 GPU table，不改变 F 份物理镜像，也不在本轮减少 dirty 扇出的必要 F 因子。

## 8. 压平 flight 临时存储与分配入口

### 8.1 组合关系

GpuFlightSlot 拥有 command allocator 和 GpuFrameResources。GpuFrameResources 组合 HostWriteBatch、constant/storage arenas、staging 页、参数池、临时目标池及 uploader 的复用 scratch。ResourceUploader 借用当前 epoch 的 staging/HostWrites，仅负责数据布局和 copy 编码。

删除 uploader/staging 内的全局 flightCount、activeFlightIndex、pending[F] 以及与之对应的 EndFlight/CollectFlight 路由。采用一个 epoch 的活动页/空闲页关系，不把 F 个槽位里面又放 F 个待回收列表。

### 8.2 写入和复用

GT 的发布不封死 RT 的 frame allocation；真正停止该批绑定改写的是局部交给录制和最终提交封口。未 Commit 的 Reservation 必须在封口时被发现，任何已封口 HostWriteBatch 不接受新写入。

一次 flight 的所有 application batches、profiler 和目标收尾都共享本次临时版本。第一个 Submit 或中间 fence 完成不能重置 staging/parameter/command storage。

结束 frame 参数版本时，也要清理其 CPU 侧旧借用记录。物理 descriptor 存储可留在池中，但不得经旧 metadata 访问已经释放的 view；下一次借出必须完整写入该组。provider/layout 关闭以前须清理依赖它的池，generation 标志本身不是 native owner。

冷却/缩容次数只作为已经安全的 idle 容量政策。PersistentCBufferArena 的 Trim 从逐 view 的 Prepare 移到现有 RT 维护/分配压力边界；不把它的 RT-owned 容器交给并发 GT 扫描。

### 8.3 固定入口不是一套 Device 镜像

持久常量仍走 PersistentCBufferArena；Material 持久 SPS、Scene buffer、目标 texture/view 走少数 typed 创建路径，最终 owner 返回对应语义对象。shader/PSO/sampler 现有专业 cache 继续是固定入口。

如果一个新包装只是转发 CreateBuffer，没有策略、预算、owner 接入或校验职责，就不新增。允许 RHI/平台/外部附加作为明示例外，但必须写明其使用者、完成凭据和回收位置。

## 9. 稳定派生数据回归 owner

### 9.1 Scene 的 view 无关绘制模板

把 Primitive/Section/默认 MaterialBinding 的模板移入 RenderScene 已有 mesh 结构或 Scene-owned 私有表。Apply 已知道 mesh/material/remove 变化，直接局部失效；同一轮反复编辑最终合并为一个结果。

模板按 mesh version 与不可变 MaterialBindings version 共享，不能每个 shape 深拷贝 section 列表。变换更新只改变矩阵、bounds 和相关排序/反面信息，不重建几何模板。

SceneDraw 删除 _scenes/PruneScenes 和结构 revision 改变后的全缓存修复扫描，只读取稳定模板并完成当前 view 的 mask/frustum、顺序、batching、visible slots 和参数。CPU 读者与下一次 Apply 的现有隔离合同继续保留。

### 9.2 材质 pass 计划

演进现有 MaterialPass 来表达可共享的不可变 pass 定义。共同 program request、输入映射、反射计划和 pipeline 固定状态由该定义明确共享；不同 Material 默认常量/纹理变化不复制同一份计划。

MaterialGpuState 继续保存本材质版本的持久常量和静态 SPS，按明确 pass 身份/索引直接访问。RasterDraw 删除长期 _plans 容器和每次 Resolve 的全表比较/失效清理，成为准备和编码算法。

CPU pass 定义发布后不可变；GPU 计划的惰性准备只在对应 RenderSystem/device generation 的 RT 准备阶段进行。跨 service/device 不能共享原生句柄；需要另一设备的 material GPU representation 时是明确的新版本。RenderServiceGeneration 仍仅是有效性/代际标记，不声称持有 native program 的寿命。

混合寿命组、view/frame 输入与 override 继续走 frame 参数池。共享长期默认资源不赋予混合组跨帧寿命。shader layout/provider 的拆除仍要求消费者停止与 GPU drain，本轮不引入热退休系统。

## 10. 资产与通用 owner 保活

RenderAssetLifetime 保留 typed resident use map、归零候选、per-flight retired refs。AssetManager 的引用操作和卸载仍只在 GT。逐 draw 不增加 AssetRef。

RetainForFrameGT 保留其功能；补齐 RT 创建/替换 GPU owner 的当前工作域登记。RT 接口只接受其可合法操作的 GPU ownership 数据，不复制、查询或析构 StreamingAssetRef。小量多态 payload 可以沿用现有机制承载粗粒度包，不推广为全部资源的全局容器。

登记某个当前 flight 作为退休锚点，必须证明它覆盖此前所有使用、已发布但尚未录制的引用，以及本次工作；不能任取一个已经完成的槽位或“最后已提交”的 fence 掩盖更早未提交录制。Scene 的有序交付和单主队列可提供该证明；多宿主资源各自保活，不能借用一方的 FrameSerial 代表另一方。

纹理迁移范围包括 Material 默认纹理、override、手工 draw、缓存 SRV、外部共享 immutable texture、已提交上传/消费者取消和 native frame 数据。完成这一清单以后，TextureAsset::OnUnload 直接按 views→texture 顺序释放自身资源，删除 AssetManager::DeferDestroy 及其 GPU retirement scope/pending batch/wait 注入。

资产加载等仍有其他等待用途时只删除 GPU deferred 依赖，不删除真正的 CPU/加载等待。最终每个旧调用者必须移除或接入明确使用协议，不保留 AlreadySafe 布尔捷径。

## 11. 帧内目标与跨帧保留

### 11.1 帧内目标

由 frame resources 的 typed texture/view pool 借出当前 epoch 的目标版本；物理纹理/view 可复用，分配不能逃逸到下一逻辑帧。Vulkan FB cache 只复用其原生组合，不替代纹理池，也不把临时纹理提升为持久 owner。

按资源兼容条件查池，考虑尺寸、格式、采样、用途、mip/layers。首版不用 aliasing。整域安全后归还并按预算修剪。相同 native view 复用时，新的内容状态由本次使用明确设置，不从旧的逻辑 Initialized 无条件继承。

### 11.2 跨帧过程

历史/异步过程保留既有 owner，不另建通用 RetainedResourceManager。过程至少记录结果 generation、producer 工作身份、有效性、消费者/窗口和预算。

readback：GPU完成但CPU未消费不能回池；CPU取消消费只终止消费权，GPU写入仍需完成。历史：某代不再供未来工作选择以后，过去已接纳的读者仍然保活该代。缺少有效 producer 时不得把它发布为下一代历史。

需要跨帧结果时从申请之初就使用允许保留的 owner 路径；禁止把 frame arena 的一个裸切片直接“升级”为 history。预算满时采用明确的失败/丢弃可选结果/背压政策，不无限增长或每帧 GPU idle。

生产验证优先复用 profiler 的 query/readback 模式；额外以测试用过程 owner 验证延迟 CPU 消费和两代历史，不为验收先实现完整效果系统。

## 12. 完成身份和通知收缩

### 12.1 工作身份提前确定

把本轮工作身份确定于 GT 取得 writable 工作域、开始本轮更新之前。Retain、等待者和 Scene 发布引用同一身份；RT 为该身份绑定真实 GPU signal，不另生成一份提交身份。

FlightIndex 只是可复用位置，FrameSerial/slot generation 才能区分版本。逻辑 FrameIndex、Scene generation、Scene delivery sequence、queue fence value 不合并。

现有 BeginUpdate/BeginFrameRecord 对 FrameSerial==0 的检查需要改成明确阶段检查；不能预留 serial 后仍拿零值代表 writable。独立宿主可使用同样的开域接口；CPU-only 路径同样能开域和终结，但不声称有 GPU 完成。

### 12.2 唯一完成账本

完成事件被合法观察并消费后更新相应工作身份；等待者绑定目标身份，不再同时依赖 WaitersCompleted 和 FlightComplete 两个布尔中转。

保留 IWaitFrameProcessor/Wait 的对外可观察恢复时机，特别是窗口操作、同次 Pump 新等待者、取消另一等待者、停机和 CPU-only 模式。不能为了删除字段让新等待者消费上轮完成或同一次 Pump 无限制重入。

未发布/未接纳的预留域在退出时明确 abandon/cancel；已接纳必要工作则按合同提交并 drain。不是每个预留 serial 都有 GPU fence，也不能给空洞 serial 伪造完成。

### 12.3 有界完成通知

规则若保证每槽最多一条未消费终结事件、消费以前不能复用槽位，则通知容量上界为 flightCount。用预分配有界队列替换 UnboundedChannel，但必须先实现身份匹配、重复发布拒绝和唯一消费。

独立宿主的跨线程完成通知仍需同步；不能用无锁队列假设单生产者但实际允许多个退役线程。首版维持现有串行 retire 前提或使用小型互斥保护，有界容量满是合同错误，不能丢消息。

## 13. 正常、失败和退出路径矩阵

| 情况 | 更新/结果语义 | 生命周期动作 |
|---|---|---|
| 不变材质多帧多 view | 重用持久默认参数 | 不产生逐 draw 保活，不重复写静态组 |
| 更新分配/暂存失败 | 未接纳，dirty 保留 | 当前临时页随 epoch 结束，不单独延期 |
| A stream 接纳后 B 失败 | A 执行，B 未初始化/dirty | 保留 A 所需 owner，draw 可跳过 |
| pipeline 准备失败 | 不发布失败 draw/history | 已接纳必要工作照常执行 |
| 录制但未提交的输出取消 | 取消提交资格 | 不重置仍有在途兄弟的共享 pool |
| 窗口最小化/隐藏 | 只跳该目标输出 | 必要更新执行；token 和 WSI 合法收尾 |
| Present RetryLater | 不承诺画面显示 | 不倒退已执行上传；GPU completion 独立 |
| 用户取消 wait | 只停止该消费者 | 不释放 GPU 仍使用的 owner |
| Material/texture 改版 | 新工作取得新版本 | 旧版本经有序退休锚点和已持有者完成退出 |
| Scene 删除/ID 重用 | 旧 Scene 不接受新更新 | 旧 GPU owner 保留到覆盖使用的完成；旧反馈不进入新 generation |
| 某 flight 扩容 | 新版本只发布到该安全槽 | 其他 flight buffer 不动；本次已交出绑定不换 |
| readback 已完成未消费 | GPU 安全但 CPU 仍保留结果 | 不复用存储，预算政策生效 |
| 缓存预算到顶 | 仅淘汰 idle 或准备失败 | 绝不回收 active pin 或等待整个 GPU |
| Device/service 关闭 | 拒绝新生产 | drain→command reset→owner 退出→cache 清理→provider 销毁 |
| 无 GPU / 未发布退出 | CPU 域终结或取消 | 不制造假的 GPU 执行或成功结果 |
| 设备丢失/等待超时 | 明确错误/终止路径 | 不把超时当完成后继续回收 |

## 14. 分阶段执行与合并门槛

每阶段都提交代码、对应测试和当前架构文档更新。过渡适配器只能存在于明确的迁移提交中；最终不保留双 registry/双更新政策来隐藏未完成迁移。

### P0：基线、资源清单和 Vulkan 1.2 能力审计

读取 AGENTS.md、build-test、cpp-conventions，以及当前 frame/asset/RHI 文档。固定 commit；全仓查 Create/Destroy/Reset/DeferDestroy、裸 view 和 command 登记调用者。更新八字段资源清单；识别 examples/benchmarks/外部宿主一起迁移的范围。

用已存在测试及 probe 记录 baseline：材质静态组创建/写入、Scene 每 flight 上传字节、command Reset、原生 RP/FB 创建、资产实际析构点、GPU final fence 与窗口行为。核实 1.2 实际初始化/编译入口，不只验证头文件可编译。

出口：有可重现运行配置、1.2 能力矩阵、精确调用者清单；不得把环境不支持记成测试成功。

### P1：工作域与完成清理顺序

主要文件：gpu_system.h/.cpp、frame_timeline.h/.cpp、application.cpp、gpu_cmd_allocator.h/.cpp、两后端 command allocator/commandbuffer。

将“观测 fence 完成”和“语义 owner 已释放/槽位可写”明确区分；结束 command epoch 与释放缓存依赖先于 view/texture owner。补 RT GPU ownership 接入，保留 GT AssetRef 规则。命令关闭、登记和 discard 的状态必须有唯一事实；保留 allocator 分配入口，不引入 command lease 总包装。

出口：中间 signal 不退休、已录制未提交不退休、旧 serial 不释放新轮、多个 batch 和空帧正确；两个后端 reset 仅一次，无每帧 idle。

### P2：必要更新接纳与目标政策统一

主要文件：gpu_system、app frame/context、render_pipeline、scene_gpu、scene_manager、window_manager、外部宿主与 samples。

增加窄必要工作登记接入；将所有必执行资源更新拆出可选目标命令。本机/外部 target 统一，不丢其他窗口/离屏工作。独立结果消费者保留 typed outcome，修改错误的整帧 bool 依赖。

先让行为通过测试，再删除 Scene 的跳过补传字段。同步所有失败/取消/关停出口，必须保证接纳之后仍有合法 Submit。

出口：最小化时上传执行；A 成功 B 失败不回滚 A；frame completion 与 present/result 有效性不混同；F 份 Scene buffer 布局没有改变。

### P3：flight 存储组合与池边界收缩

主要文件：gpu_frame_resources、gpu_resource、gpu_system。

FrameResources 组合 HostWrites/staging/uploader scratch；删嵌套 F 路由。保留 Try 批量暂存、大小页策略和各类对齐。将 per-view Trim 移至已确定边界。封口检查 Reservation/参数版本使用期限。

出口：所有批次共用一个 epoch；warm 页复用不退化；分配失败不会泄漏或重复 reset；临时版本不能逃逸。

### P4：公共附件 API 与 Vulkan 原生引用缓存

主要文件：rhi.h/.cpp、render pass registry（最终删除）、Vulkan/D3D12 实现、graphics_pipeline_cache、render_system、render_output、scene_draw/raster_draw，以及所有测试/示例。

先实现三种 key 和 typed cache 测试，再接入命令局部 Prepare token、command keepalive、view ID/局部失效及预算；将 PSO 从 Pass 指针改成兼容签名。D3D12 同步切换直接附件输入。切换全部调用方后删除 public RP/FB owners 和 runtime registry。

可用小型中间适配保持每提交可编译，但不能最终同时保留旧 cache 与新 cache。

出口：Vulkan 1.2 无 dynamic/imageless 路径完整运行；单 flight 暖机后无每帧原生 RP/FB 重建；clear/load/尺寸变化的 key 行为正确；RT/GT 和多宿主计数正确；重建窗口没有 stale framebuffer 或缓存保活 backbuffer。

### P5：Scene 与 Material 稳定数据归属迁移

主要文件：render_scene、scene_draw、material、material_gpu_state、raster_draw，以及相关 header/source 对。

Scene Apply 局部维护共享模板；MaterialPass 明确共享定义和 native plan 的 generation。删除两个绘制 helper 的长期旁路 cache。保持透明/显式实例顺序、material override、CPU read 隔离和多 view 行为。

出口：变换只更新相关数据；一处 mesh/material 改绑不扫描所有缓存记录；三 view 不重复物化相同模板；共享 pass 的内存不按 Material 数线性复制同一 plan；静态默认 SPS 稳态写入增量为零。

### P6：持久创建入口与资产 GPU deferred 退场

主要文件：texture_asset、static_mesh、asset_manager、render_asset_lifetime、资源创建调用者。

先完成裸 texture/view 使用者迁移和完整生命周期测试，再删 DeferDestroy 子系统。持久 owner 返回原语义对象，RT 不接触非原子 AssetRef。清楚记录 raw RHI 和外部 provider 的显式例外。

出口：取消/改绑/跨宿主使用不提前释放；纹理不再额外等无关 flight；AssetManager 不持有 GPU retirement coroutine；没有新增总 owner 注册表。

### P7：帧内目标和显式保留过程

主要文件：frame resource typed pool、GpuFrameProfiler、过程 owner 的最小示例/测试。

加入必要的 frame texture/view 借用策略与预算；用 query/readback 和测试历史窗口验证 retained 合同。结果保留不偷用 frame 切片，不新增第二套 fence 轮询。

出口：GPU完成但CPU未消费不回池；历史退出不影响已接纳读者；预算到顶结果确定；pool idle 不占有外部 backbuffer。

### P8：完成通知与身份精简

主要文件：frame_timeline、application、gpu_system、窗口等待及 standalone examples。

将工作身份在 GT 开域时确定；等待登记目标身份；通过同一完成账本恢复。证明每槽一条未消费终结事件后替换为有界通知。删除重复布尔中转；保留停止/重入/固定批次截止。

出口：CPU-only、窗口 wait、新等待者、取消其他等待者、未发布退出、外部退休线程、重复消息均有测试；不因改字段改变用户可见帧节奏。

### P9：全矩阵回归、性能对照和文档收束

按第 15/16 节运行全部矩阵。核查旧 public 工厂/registry、uploader flight 路由、资产 deferred、helper cache 和 Scene Attempt 是否真正消失。更新长期架构文档；临时计划留档但不与长期合同重复。

P1 是 P4 原生引用缓存与 P6 资产退场的前置条件；P2 是删除 Scene Attempt 的前置条件；P4/P5 可在 P1 稳定后分别推进；P6 必须等待使用者清单闭合；P8 不抢在核心资源路径之前。

## 15. 测试验收明细

用 GTest 承载正确性，Google Benchmark 承载性能。仅测试 fixture/probe 保存计数、析构轨迹和故障注入；不在生产对象增加只为测试服务的字段。不以 sleep 猜测 fence 完成，使用现有/测试 native gate。

### 15.1 CPU / fake device 测试

| ID | 场景 | 通过条件 |
|---|---|---|
| C01 | 同一附件描述规范化两次 | key 相同且 hash/equality 一致 |
| C02 | 仅修改 clear 值 | RP/FB/PSO key 均不变 |
| C03 | 仅修改 load/store | RP execution key 改变；兼容 signature/FB/PSO 不被错误分裂 |
| C04 | 修改格式、采样或附件次序 | 正确改变兼容 key，拒绝不匹配 PSO |
| C05 | 修改 viewport/scissor 或 renderArea | 不错误制造新 FB；extent/layers 真改变时改变 FB key |
| C06 | 地址与 VkImageView 数值复用 | 新稳定 view ID 不命中旧条目 |
| C07 | 引用归零后再次借用 | 预算内可复用，非每帧创建 |
| C08 | 淘汰 idle | 不淘汰任何 pinned 条目，索引/反向关联一致 |
| C09 | 单 view 多 FB 销毁 | 只访问局部关联条目，无全缓存扫描 |
| C10 | cache 创建第二步失败 | 没有半成品发布，已取得引用正确退回 |
| C11 | shader/PSO 临时 RP 创建引用结束 | 不使 PSO 依赖悬垂 wrapper 指针 |
| C12 | 未使用的 prepare token | epoch reset 后无泄漏；旧 token 被拒绝 |
| C13 | Scene 多次同 Shape 改绑 | Apply 最终状态只局部失效 |
| C14 | 仅 transform dirty | 不重建 section/material 模板 |
| C15 | 多 Material 共享 pass 定义 | 共享同一计划而默认资源独立 |
| C16 | frame completion 重复/旧 ID | 不释放新域、不重复唤醒 |
| C17 | 同 Pump 创建/取消等待者 | 尊重序号截止与取消身份 |
| C18 | CPU-only 与未发布终止 | 无假 GPU completion，无残留等待 |

### 15.2 两后端 GPU 与生命周期测试

| ID | 场景 | 通过条件 |
|---|---|---|
| G01 | F=1/2/3/4，多轮复用 | 命令、切片、SPS 和 owner 对应正确版本 |
| G02 | 第一批 signal 完成，后续 gate 阻塞 | RP/FB pin、staging 和整个 flight 不退休 |
| G03 | 已准备/已录制但未提交，逻辑 owner 替换 | 旧资源保持有效，未拿“没有 fence”证明安全 |
| G04 | 必要 copy 与 backbuffer 输出；提交前隐藏 | copy 执行、输出可跳；回读值正确 |
| G05 | A stream 接纳，B staging 分配失败 | A 回读正确，B dirty 留下，无全局回滚 |
| G06 | 后续 pipeline/PSO/pass prepare 失败 | 之前接纳的必要更新未丢失 |
| G07 | 取消已提交任务/等待 | owner 活至真实完成 |
| G08 | Framebuffer 两条 command 共同引用 | 一条结束不销毁；全部结束才进入 idle |
| G09 | 同 command 多 pass 重用相同条目 | 不发生逐 draw refcount 或重复 native 创建 |
| G10 | clear/load/store/MRT/depth-only/read-only | 实际内容与旧实现语义一致 |
| G11 | MSAA 与现有 resolve 路径 | 采样/输出正确，不隐式加入不支持特性 |
| G12 | Vulkan 1.2，dynamic/imageless 均禁用 | 主路径通过；没有调用未启用 API |
| G13 | 1.3+ 设备仍强制传统主路径 | 行为一致，不靠可用扩展掩盖 1.2 依赖 |
| G14 | resize/recreate/销毁 texture view | old FB 在 view 前退出，没有 stale 命中 |
| G15 | 相同兼容 signature 不同 RP 实例 | pipeline/FB 复用合法，validation 无错 |
| G16 | 销毁创建期代表 RP 后使用 PSO/兼容 FB | 实际 Begin 所需 RP 另有有效 pin，运行正确 |
| G17 | Scene 某 flight 扩容 | 其他 flight native buffer 不变；各自内容正确 |
| G18 | Scene 删除后复用 SceneId/ShapeId | 旧退休/反馈不污染新 generation |
| G19 | 多 view/多 pass 准备同 Scene | 一 serial 每镜像最多准备一次，失败结果也一致 |
| G20 | 同 Device 两宿主/allocator 使用同 RP/FB | A 完成不释放 B pin；各自 final proof 独立 |
| G21 | Device 关闭仍有命令 pin | 清晰诊断，不静默销毁在途对象 |
| G22 | Material default / override / mixed group | 默认内容不被 override 改写，混合组不跨 epoch |
| G23 | readback 已 GPU 完成但延迟 CPU 消费 | 不复用结果存储；预算行为确定 |
| G24 | 历史窗口推进、旧代仍被另 flight 使用 | 旧代直到最后使用退出才回池 |
| G25 | 关停于准备/接纳/提交/完成消费各阶段 | 已接纳必执行、未发布明确取消、回收顺序一致 |
| G26 | Present RetryLater / acquire 跳过 | 不把画面结果当上传结果；WSI 同步无提前复用 |
| G27 | 材质/纹理所有外部 ref 释放 | 移除 DeferDestroy 后仍由真实使用者 owner 保活 |
| G28 | native pass/FB 缓存预算压力 | 只退 idle，不通过 GPU idle 或破坏 active 引用缓解 |
| G29 | 后续退休锚点已完成，但更早命令 epoch 尚未消费 | 先结束覆盖前缀的命令引用，再释放删除包；无误报或提前销毁 |
| G30 | frame SPS 物理对象被池保留，旧 texture/view 退出 | 旧借用 metadata 不被解引用，下一次完整写入后再绑定 |

负面合同测试在驱动调用前拦截，避免用真实 device lost 作为一般测试方法。Vulkan validation、synchronization validation 和 D3D12 debug layer 分别运行；被环境 skip 的结果单独报告。

## 16. 性能验收

沿用 bench_scene_gpu、bench_raster_prepare、bench_material_scene、bench_command_allocator、scene sync 和 lifecycle 的现有基准入口 [S13]。

### 16.1 工作负载矩阵

- 场景规模：1k、10k、100k 对象；1 和 3 个 view；F=1/2/3/4。
- 更新：稳定不变、单次变化后静止、每帧稀疏变化、每帧全量变化；频繁 Material/mesh 改绑另测。
- 目标：稳定附件、clear 值变化、load/store 变化、多窗口、连续 resize、预算压力。
- 提交：单 batch、多 batch、被 gate 阻塞的后续 batch、可选输出跳过、必要更新仍执行。

### 16.2 应当出现的结构性结果

不变材质暖机后，默认常量分配、静态 SPS 创建、静态组 Set/FlushWrites 的增量为零。动态 view/frame 绑定正常写入不在零增量范围。

稳定附件暖机后，尤其单 flight，不应每帧 vkCreateRenderPass/vkCreateFramebuffer；修改 clear 值不创建新条目；load/store 不仅因为 pointer 身份造成新 PSO。

Scene 模板变化成本与受影响对象/共享模板有关，不因三 view 重做三次旁路缓存修复。Scene GPU 驻留仍按 F 份镜像计算，不宣称本轮得到单 buffer 的显存/带宽收益。

没有 F×F staging pending 路由、没有每次 view 执行 PersistentCBufferArena 全页 Trim、没有每 draw typed/native owner pin。

### 16.3 测量和门槛

分别记录 CPU Prepare/Apply/Submit、GPU 时间、尾部帧延迟、上传字节、command Reset/Submit 次数、native 对象创建和 cache 命中，以及 resident/retired/retained/cached 占用。活跃资源与预算内 idle cache 分开报告。

在同机、同构建选项、同验证设置下交错执行基线/改版，单独测冷启动和暖机稳定区间。性能至少不出现超出测量噪声的可重复退化；不能用 CPU 变快掩盖 GPU 明显变慢，也不靠均值掩盖尾部停顿。

不用未经实测的百分比承诺收益。若缓存锁或新 token 查找抵消收益，优化该局部实现，而不是添加更多全局状态。正常稳态不得新增 GPU WaitIdle，预算压力不得强制销毁 pinned 资源。

## 17. 最终删除与保留清单

### 删除或归并

公共 RenderPass/Framebuffer GPU owner 及工厂；RenderPassRegistry；WindowManager 到 registry 的失效通知；PSO 的 CompatibleRenderPass 指针身份；SceneDraw 的旁路 Scene 生命周期；RasterDraw 的长期全表 plan cache；uploader/staging 的嵌套 flight 路由；经证明可替代的 Scene Attempt 回补；迁移完成后的 AssetManager GPU deferred 等待；重复完成布尔通路；逐 view Trim。

### 必须保留

Vulkan 1.2 原生 RP/FB 及 typed cache；每 flight Scene GPU 副本、dirty 和 generation；MaterialGpuState；SceneWriter typed 资产使用计数与退休包；真实 GPU 完成；CPU 发布与 RT 准备边界；WSI 同步；独立宿主和外部 provider 所有权；专业 allocator 的不同策略；结果有效性与 CPU 消费权；CPU-only 时间线；现有 CPU Scene 读者隔离。

## 18. 工程交付要求

每阶段遵守仓库 AGENTS.md：头/源对应、项目容器别名、Nullable、现有 profiler 宏、不增加未经批准的全局共享状态或异常结构、third_party/SDKs 只读。新增私有 helper 放最窄所属模块；不把 GPU 缓存业务上移或把资产/Scene 名称带进 RHI。

长期合同更新到 docs/architecture/frame-and-gpu.md、render-rhi.md、render-framework.md、asset-system.md；用法和测试更新到相应 guide。只把执行状态/基线留在 docs/temp，不维护相互矛盾的多份权威文档。

完成标准不是新增生命周期类，而是：owner 不变得含糊，安全证明集中，重复状态确实删掉，Vulkan 1.2 主路径完整，每 flight Scene 镜像保持，失败/取消/跳帧/关停同一合同，性能有可重现的验证。

---

## 源码与规范索引

以下源码固定到本计划基线；规范引用只支撑 Vulkan/D3D12 的要求，本文的数据结构和迁移排序是本项目设计建议。

[S1] [GpuSystem：提交、Reset 与 frame owner 释放](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/gpu_system.cpp)

[S2] [SceneManager：SceneGPU 退休和完成交接](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/render_framework/scene_manager.cpp)

[S3] [SceneGpuData：per-flight buffer 和 Pending/Attempt](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/render_framework/scene_gpu.cpp)

[S4] [MaterialGpuState](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/include/radray/runtime/render_framework/material_gpu_state.h)；[RasterDraw](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/render_framework/raster_draw.cpp)

[S5] [GpuFrameResources](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/include/radray/runtime/gpu_frame_resources.h)；[Arena/staging/uploader](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/gpu_resource.cpp)

[S6] [SceneDraw：cache 与 Trim](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/render_framework/scene_draw.cpp)

[S7] [RHI 接口](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/render/include/radray/render/rhi.h)

[S8] [RenderPassRegistry](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/render/src/render_pass_registry.cpp)；[WindowManager](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/window_manager.cpp)

[S9] [Vulkan 后端类型、缓存与 command](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/render/include/radray/render/backend/vulkan_impl.h)；[Vulkan 实现](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/render/src/vk/vulkan_impl.cpp)

[S10] [TextureAsset](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/texture_asset.cpp)；[AssetManager](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/include/radray/runtime/asset_manager.h)；[RenderAssetLifetime](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/render_framework/render_asset_lifetime.cpp)

[S11] [FrameTimeline](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/include/radray/runtime/frame_timeline.h)

[S12] [Pipeline 接口](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/include/radray/runtime/render_framework/render_pipeline.h)；[RasterGraph](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/modules/runtime/src/render_framework/raster_graph.cpp)

[S13] [Benchmark 入口](https://github.com/ksgfk/RadRay/blob/7a807f037c5e15045bf2fc6c702d8819842b9ed5/benchmarks/CMakeLists.txt)

[N1] [Vulkan Render Pass Compatibility](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-compatibility)

[N2] [VkGraphicsPipelineCreateInfo：renderPass 创建期访问](https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineCreateInfo.html)

[N3] [VkFramebufferCreateInfo：renderPass 创建期访问与附件条件](https://docs.vulkan.org/refpages/latest/refpages/source/VkFramebufferCreateInfo.html)

[N4] [vkDestroyRenderPass](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyRenderPass.html)；[vkDestroyFramebuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyFramebuffer.html)

[N5] [VK_KHR_imageless_framebuffer：进入 1.2](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_imageless_framebuffer.html)

[N6] [VkPhysicalDeviceImagelessFramebufferFeatures](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceImagelessFramebufferFeatures.html)

[N7] [Command Buffer Lifecycle](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html#commandbuffers-lifecycle)

[N8] [vkUpdateDescriptorSets](https://docs.vulkan.org/refpages/latest/refpages/source/vkUpdateDescriptorSets.html)

[N9] [D3D12 BeginRenderPass](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist4-beginrenderpass)

[N10] [Vulkan Swapchain Semaphore Reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
