# Render Graph 系统设计与实现调研

> 调研对象：Falcor (NVIDIA)、Kajiya (Embark Studios)、Unity (SRP)、Unreal Engine (RDG)
>
> 基础参考：[FrameGraph: Extensible Rendering Architecture in Frostbite (GDC 2017)](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in) — Yuriy O'Donnell 在 GDC 2017 的演讲，奠定了业界 Frame Graph 的设计范式。

---

## 目录

- [一、概述](#一概述)
- [二、Falcor (NVIDIA)](#二falcor-nvidia)
- [三、Kajiya (Embark Studios)](#三kajiya-embark-studios)
- [四、Unity (Render Graph / SRP)](#四unity-render-graph--srp)
- [五、Unreal Engine (RDG)](#五unreal-engine-rdg)
- [六、Pass 内部如何发出绘制指令](#六pass-内部如何发出绘制指令)
- [七、横向对比](#七横向对比)
- [八、参考资料](#八参考资料)

---

## 一、概述

Render Graph（又称 Frame Graph）是一种将单帧渲染描述为**有向无环图（DAG）**的架构模式。每个节点代表一个渲染 Pass，边代表资源依赖关系。该架构的核心优势：

- **自动资源生命周期管理**：根据依赖图精确计算资源的首次/末次使用时间
- **自动屏障/同步插入**：编译器全局分析资源状态转换，自动在 Pass 之间插入 GPU 屏障
- **Pass 裁剪**：自动移除对最终输出无贡献的 Pass（死代码消除）
- **资源池化与内存别名**：生命周期不重叠的资源可共享物理内存
- **声明式设计**：用户描述"需要什么"，系统决定"如何做"

---

## 二、Falcor (NVIDIA)

> - GitHub: [NVIDIAGameWorks/Falcor](https://github.com/NVIDIAGameWorks/Falcor)
> - 文档: [Falcor Render Graph 文档](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/render-graph.md)
> - NVIDIA 开发者页面: [developer.nvidia.com/falcor](https://developer.nvidia.com/falcor)

### 2.1 核心架构

Falcor 的 Render Graph 位于 `Source/Falcor/RenderGraph/`，基于插件架构设计，面向研究场景优化。

| 文件 | 职责 |
|------|------|
| `RenderGraph.h/cpp` | 主调度器，管理 DAG、Pass 注册、边创建、输出标记、执行 |
| `RenderPass.h/cpp` | Pass 基类，插件架构（PluginManager 动态加载） |
| `RenderPassReflection.h/cpp` | I/O 反射系统，声明资源需求 |
| `ResourceCache.h/cpp` | 资源分配、生命周期管理、资源别名 |
| `RenderGraphCompiler.h/cpp` | 图编译管线（拓扑排序、裁剪、验证、分配） |
| `RenderGraphExe.h/cpp` | 编译后的可执行表示 |
| `RenderGraphUI.h/cpp` | ImGui 节点可视化编辑器 |
| `RenderGraphIR.h/cpp` | 中间表示，用于 Python 脚本导入/导出 |
| `RenderGraphImportExport.h/cpp` | 图的序列化与反序列化 |
| `RenderPassHelpers.h/cpp` | 通用 Pass 输入输出声明辅助函数 |

核心数据结构（`RenderGraph.h`）：

```cpp
class RenderGraph : public Object {
    std::unique_ptr<DirectedGraph> mpGraph;           // DAG
    std::unordered_map<std::string, uint32_t> mNameToIndex;  // Pass名 → 节点ID
    std::unordered_map<uint32_t, EdgeData> mEdgeData; // 边元数据
    std::unordered_map<uint32_t, NodeData> mNodeData; // 节点元数据
    std::vector<GraphOut> mOutputs;                   // 图输出列表
    std::unique_ptr<RenderGraphExe> mpExe;            // 编译后的可执行体
};
```

### 2.2 Pass 系统

**基类定义**（`RenderPass.h`）：

```cpp
class FALCOR_API RenderPass : public Object {
    FALCOR_OBJECT(RenderPass)
public:
    using PluginCreate = std::function<ref<RenderPass>(ref<Device>, const Properties&)>;
    FALCOR_PLUGIN_BASE_CLASS(RenderPass);

    struct CompileData {
        uint2 defaultTexDims;
        ResourceFormat defaultTexFormat;
        RenderPassReflection connectedResources;
    };

    // 核心虚方法
    virtual RenderPassReflection reflect(const CompileData& compileData) = 0;  // 声明I/O
    virtual void compile(RenderContext*, const CompileData&) {}                // 编译
    virtual void execute(RenderContext*, const RenderData&) = 0;               // 执行
};
```

**Pass 通过 PluginManager 动态加载**，支持热重载。如果插件未加载，会按名称自动加载。

### 2.3 反射系统

`RenderPassReflection` 提供声明式资源需求描述（`RenderPassReflection.h`）：

```cpp
RenderPassReflection reflect(const CompileData& compileData) override {
    RenderPassReflection r;
    r.addInput("worldPos", "World position").texture2D();
    r.addInput("normal", "Normal").texture2D().format(ResourceFormat::RGBA16Float);
    r.addOutput("color", "Output color")
        .texture2D(0, 0)                       // 宽高默认为交换链尺寸
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(ResourceBindFlags::RenderTarget);
    r.addInternal("temp", "Temporary buffer")
        .texture2D().flags(RenderPassReflection::Field::Flags::Optional);
    return r;
}
```

每个 Field 可设置的属性：

| 属性 | 说明 |
|------|------|
| `Visibility` | `Input` / `Output` / `InputOutput` / `Internal` |
| `Type` | `Texture1D` / `Texture2D` / `Texture3D` / `TextureCube` / `RawBuffer` |
| `Flags` | `None` / `Optional`（不使用不分配） / `Persistent`（跨帧保持） |
| 尺寸/格式/采样数 | `width`, `height`, `depth`, `mipCount`, `sampleCount`, `format` |
| `BindFlags` | `ShaderResource`, `RenderTarget`, `UnorderedAccess` 等 |

**资源别名**：多个逻辑 Field 可映射到同一物理资源，属性通过 `field.merge()` 自动合并。

### 2.4 编译流程

编译管线（`RenderGraphCompiler.cpp`）分五个阶段：

```
┌─────────────────────────────────────────────────────────┐
│ 1. resolveExecutionOrder()                              │
│    ├─ 识别必需 Pass（图输出 + 执行依赖边）              │
│    ├─ 从输出反向 DFS，找到所有参与 Pass                 │
│    └─ 拓扑排序确定执行顺序（DAG 裁剪）                  │
├─────────────────────────────────────────────────────────┤
│ 2. compilePasses()                                      │
│    └─ 按执行顺序调用每个 Pass 的 compile()              │
├─────────────────────────────────────────────────────────┤
│ 3. insertAutoPasses()                                   │
│    ├─ 检测 MSAA 解析需求（输出采样数 > 输入采样数）     │
│    └─ 自动插入 ResolvePass                              │
├─────────────────────────────────────────────────────────┤
│ 4. validateGraph()                                      │
│    ├─ 确保所有 required input 已连接                    │
│    ├─ 确保图至少有一个输出                              │
│    └─ 检查 input 不能同时有边和外部资源                 │
├─────────────────────────────────────────────────────────┤
│ 5. allocateResources()                                  │
│    ├─ 注册所有输出资源                                  │
│    ├─ 将输入注册为输出的别名                            │
│    └─ 创建实际 GPU 资源                                 │
└─────────────────────────────────────────────────────────┘
```

### 2.5 资源管理

**ResourceCache**（`ResourceCache.h`）管理两类资源：

```cpp
struct ResourceData {
    RenderPassReflection::Field field;      // 合并后的属性
    std::pair<uint32_t, uint32_t> lifetime; // 使用时间范围 [start, end]
    ref<Resource> pResource;                // 实际 GPU 资源
    bool resolveBindFlags;
    std::string name;                       // "PassName.FieldName"
};
```

- **Graph-owned 资源**：由 ResourceCache 分配和管理，生命周期为 `{startTime, endTime}`
- **External 资源**：用户通过 `RenderGraph::setInput()` 提供，不由图管理
- **Optional 资源**：标记为 `Flags::Optional` 的资源，不使用时不分配
- **屏障管理**：Falcor 不在图层显式插入屏障，由底层 `RenderContext` 自动处理

### 2.6 执行

```cpp
void RenderGraphExe::execute(const Context& ctx) {
    for (const auto& pass : mExecutionList) {
        RenderData renderData(pass.name, *mpResourceCache,
            ctx.passesDictionary, ctx.defaultTexDims, ctx.defaultTexFormat);
        pass.pPass->execute(ctx.pRenderContext, renderData);
    }
}
```

### 2.7 API 设计

**C++ API**（`RenderGraph.h`）：

```cpp
// 创建
auto graph = RenderGraph::create(device, "MyGraph");
auto graph = RenderGraph::createFromFile(device, "path/to/script.py");

// Pass 管理
graph->createPass("GBuffer", "GBufferPass", properties);
graph->addPass(passRef, "Lighting");
graph->removePass("DebugView");
graph->updatePass("GBuffer", newProperties);

// 边管理
graph->addEdge("GBuffer.posW", "Lighting.posW");   // 数据依赖
graph->addEdge("GBuffer", "Lighting");              // 执行依赖
graph->removeEdge("GBuffer.posW", "Lighting.posW");

// 输出管理
graph->markOutput("Lighting.output");
graph->unmarkOutput("Lighting.output");
auto output = graph->getOutput("Lighting.output");

// 编译与执行
std::string log;
graph->compile(renderContext, log);
graph->execute(renderContext);
```

**Python 脚本 API**：

```python
def render_graph_DeferredPipeline():
    g = RenderGraph("Deferred Pipeline")

    GBuffer = createPass("GBufferRaster")
    g.addPass(GBuffer, "GBuffer")

    Lighting = createPass("DeferredLighting", {'envMapIntensity': 1.0})
    g.addPass(Lighting, "Lighting")

    ToneMap = createPass("ToneMapper", {'autoExposure': False})
    g.addPass(ToneMap, "ToneMap")

    g.addEdge("GBuffer.posW",   "Lighting.posW")
    g.addEdge("GBuffer.normW",  "Lighting.normW")
    g.addEdge("GBuffer.diffuse","Lighting.diffuse")
    g.addEdge("Lighting.color", "ToneMap.src")
    g.markOutput("ToneMap.dst")
    return g
```

### 2.8 可视化图编辑器

Falcor 提供完整的 **ImGui 节点编辑器**（`RenderGraphUI.h`、`RenderGraphEditor.h`）：

- 节点拖拽创建/连接
- 实时图预览
- Pass 属性编辑
- 图输出标记
- Python 脚本导入/导出
- 错误日志与验证反馈

### 2.9 设计特点总结

| 特点 | 说明 |
|------|------|
| **插件化 Pass** | PluginManager 动态加载，支持热重载 |
| **反射式配置** | 零拷贝 Field 声明，自动资源绑定 |
| **Python 脚本化** | 图定义、序列化、导入/导出全部通过 Python |
| **可视化编辑** | 完整的 ImGui 节点编辑器 |
| **研究友好** | 灵活组合 Pass，快速原型迭代 |

---

## 三、Kajiya (Embark Studios)

> - GitHub: [EmbarkStudios/kajiya](https://github.com/EmbarkStudios/kajiya)
> - 架构文档: [kajiya/docs/architecture.md](https://github.com/EmbarkStudios/kajiya/blob/main/docs/architecture.md)
> - 作者: [Tomasz Stachowiak (@h3r2tic)](https://github.com/h3r2tic)

### 3.1 核心架构

Kajiya 的 Render Graph 位于 `crates/lib/kajiya-rg/`，基于 Rust 类型系统实现编译期安全。

| 文件 | 职责 |
|------|------|
| `graph.rs` (1212行) | 核心 RenderGraph、编译、执行 |
| `pass_builder.rs` (339行) | PassBuilder 流式 API |
| `pass_api.rs` (768行) | RenderPassApi（GPU 命令录制） |
| `resource.rs` (205行) | Handle/Ref 类型系统 |
| `resource_registry.rs` (152行) | 运行时资源注册表 |
| `hl.rs` (421行) | SimpleRenderPass 高层 API |
| `temporal.rs` (355行) | 跨帧持久资源（Temporal Resource）管理 |

核心数据结构（`graph.rs`）：

```rust
pub struct RenderGraph {
    passes: Vec<RecordedPass>,
    resources: Vec<GraphResourceInfo>,
    exported_resources: Vec<(ExportableGraphResource, vk_sync::AccessType)>,
    pub(crate) compute_pipelines: Vec<RgComputePipeline>,
    pub(crate) raster_pipelines: Vec<RgRasterPipeline>,
    pub(crate) rt_pipelines: Vec<RgRtPipeline>,
    pub predefined_descriptor_set_layouts: HashMap<u32, PredefinedDescriptorSet>,
    pub debug_hook: Option<GraphDebugHook>,
    pub debugged_resource: Option<Handle<Image>>,
}
```

### 3.2 类型安全的 Handle 系统

Kajiya 利用 Rust 的类型系统在**编译期**保证资源访问安全（`resource.rs`）：

```rust
/// 拥有资源的句柄 — 可传递给 Pass
pub struct Handle<ResType: Resource> {
    pub(crate) raw: GraphRawResourceHandle,   // { id: u32, version: u32 }
    pub(crate) desc: <ResType as Resource>::Desc,
    pub(crate) marker: PhantomData<ResType>,
}

/// 带特定访问类型的引用视图 — 传入 render function
pub struct Ref<ResType: Resource, ViewType: GpuViewType> {
    pub(crate) handle: GraphRawResourceHandle,
    pub(crate) desc: <ResType as Resource>::Desc,
    pub(crate) marker: PhantomData<(ResType, ViewType)>,
}

// 零开销视图类型标记
pub struct GpuSrv;   // 只读  (IS_WRITABLE = false)
pub struct GpuUav;   // 写入  (IS_WRITABLE = true)
pub struct GpuRt;    // 渲染目标 (IS_WRITABLE = true)
```

**版本追踪**：每次写操作生成新版本 `raw.next_version()`，用于依赖追踪。

**资源类型**：

```rust
pub trait Resource {
    type Desc: ResourceDesc;
    fn borrow_resource(res: &AnyRenderResource) -> &Self;
}

impl Resource for Image { type Desc = ImageDesc; }
impl Resource for Buffer { type Desc = BufferDesc; }
impl Resource for RayTracingAcceleration { type Desc = RayTracingAccelerationDesc; }
```

### 3.3 Pass 系统

**PassBuilder API**（`pass_builder.rs`）：

```rust
pub struct PassBuilder<'rg> {
    pub(crate) rg: &'rg mut RenderGraph,
    pub(crate) pass_idx: usize,
    pub(crate) pass: Option<RecordedPass>,
}

impl<'rg> PassBuilder<'rg> {
    // 在 pass 内创建资源
    pub fn create<Desc: ResourceDesc>(&mut self, desc: Desc) -> Handle<Desc::Resource>;

    // 声明读（返回只读引用）
    pub fn read<Res: Resource>(
        &mut self, handle: &Handle<Res>, access_type: vk_sync::AccessType
    ) -> Ref<Res, GpuSrv>;

    // 声明写（返回可写引用，handle 版本递增）
    pub fn write<Res: Resource>(
        &mut self, handle: &mut Handle<Res>, access_type: vk_sync::AccessType
    ) -> Ref<Res, GpuUav>;

    // 写入但跳过同步（优化，同类型访问不插入屏障）
    pub fn write_no_sync<Res: Resource>(...) -> Ref<Res, GpuUav>;

    // 渲染目标
    pub fn raster<Res: Resource>(...) -> Ref<Res, GpuRt>;

    // 注册管线
    pub fn register_compute_pipeline(&mut self, path: &str) -> RgComputePipelineHandle;
    pub fn register_raster_pipeline(&mut self, ...) -> RgRasterPipelineHandle;
    pub fn register_ray_tracing_pipeline(&mut self, ...) -> RgRtPipelineHandle;

    // 设置执行回调
    pub fn render(self, render: impl FnOnce(&mut RenderPassApi) -> Result<()> + 'static);
}
```

**同步类型**（`graph.rs`）：

```rust
pub enum PassResourceAccessSyncType {
    AlwaysSync,                    // 始终插入屏障
    SkipSyncIfSameAccessType,     // 相同访问类型时跳过
}
```

### 3.4 高层 API（SimpleRenderPass）

`hl.rs` 提供链式调用的便利封装：

```rust
pub fn light_gbuffer(
    rg: &mut RenderGraph,
    gbuffer_depth: &GbufferDepth,
    shadow_mask: &rg::Handle<Image>,
    output: &mut rg::Handle<Image>,
    sky_cube: &rg::Handle<Image>,
) {
    SimpleRenderPass::new_compute(
        rg.add_pass("light gbuffer"),
        "/shaders/light_gbuffer.hlsl"
    )
    .read(&gbuffer_depth.gbuffer)
    .read_aspect(&gbuffer_depth.depth, vk::ImageAspectFlags::DEPTH)
    .read(shadow_mask)
    .write(output)
    .read(sky_cube)
    .constants((
        gbuffer_depth.gbuffer.desc().extent_inv_extent_2d(),
        debug_shading_mode as u32,
    ))
    .dispatch(gbuffer_depth.gbuffer.desc().extent);
}
```

`SimpleRenderPass` 支持的操作：

| 方法 | 说明 |
|------|------|
| `new_compute(pass, shader)` | 创建计算 Pass |
| `new_rt(pass, rgen, miss, hit)` | 创建光线追踪 Pass |
| `.read(handle)` | 声明 SRV 读取 |
| `.read_aspect(handle, aspect)` | 读取特定 aspect（如深度） |
| `.write(handle)` | 声明 UAV 写入 |
| `.write_no_sync(handle)` | 写入（跳过同步优化） |
| `.constants(data)` | Push constants |
| `.raw_descriptor_set(set, desc)` | 绑定原始描述符集 |
| `.bind(binding)` / `.bind_mut(binding)` | Trait-based 绑定 |
| `.dispatch(extent)` | 提交计算 |
| `.trace_rays(tlas, extent)` | 提交光追 |

### 3.5 资源管理

**资源创建与导入/导出**（`graph.rs`）：

```rust
// 创建 transient 资源
let tex = rg.create(ImageDesc::new_2d(extent, format));

// 导入外部资源
let imported = image_arc.clone().import(&mut rg, AccessType::ComputeShaderReadSampledImage);

// 导出资源（执行后保持存活）
let exported = Image::export(handle, &mut rg, AccessType::ComputeShaderReadSampledImage);
```

**Transient 资源池**（`graph.rs`）：

```rust
// 执行时从 transient cache 分配
let image = transient_resource_cache
    .get_image(&desc)                              // 尝试复用
    .unwrap_or_else(|| device.create_image(desc));  // 否则新建

// 执行后归还
retired_rg.release_resources(&mut transient_resource_cache);
```

**资源使用标志聚合**：编译时遍历所有 Pass，聚合每个资源的完整 usage flags：

```rust
fn calculate_resource_info(&self) -> ResourceInfo {
    for (pass_idx, pass) in self.passes.iter().enumerate() {
        for res_access in pass.read.iter().chain(pass.write.iter()) {
            let access_mask = get_access_info(res_access.access.access_type).access_mask;
            image_usage_flags[res_id] |= image_access_mask_to_usage_flags(access_mask);
        }
    }
}
```

### 3.6 同步与屏障

自动屏障插入（`graph.rs`）：

```rust
fn transition_resource(device, cb, resource, access, ...) {
    // Skip-sync 优化
    if RG_ALLOW_PASS_OVERLAP
        && resource.access_type == access.access_type
        && matches!(access.sync_type, SkipSyncIfSameAccessType)
    {
        return; // 跳过屏障
    }

    match resource.resource.borrow() {
        Image(image) => {
            record_image_barrier(device, cb, ImageBarrier::new(
                image.raw,
                resource.access_type,   // 前一状态
                access.access_type,     // 目标状态
                aspect_mask,
            ));
        }
        Buffer(buffer) => {
            vk_sync::cmd::pipeline_barrier(device, cb, None,
                &[vk_sync::BufferBarrier {
                    previous_accesses: &[resource.access_type],
                    next_accesses: &[access.access_type],
                    buffer: buffer.raw,
                    offset: 0, size: buffer.desc.size,
                    ..
                }], &[]);
        }
    }
    resource.access_type = access.access_type;
}
```

**首次访问批量转换**：执行开始时一次性将所有资源转换到首次访问状态，减少后续屏障开销。

### 3.7 Temporal 资源系统

Kajiya 独创的跨帧持久资源管理（`temporal.rs`）：

```rust
// 状态机
enum TemporalResourceState {
    Inert { resource, access_type },   // 空闲，持有资源和最终访问状态
    Imported { resource, handle },      // 已导入到当前帧的 RG
    Exported { resource, handle },      // 已从当前帧的 RG 导出
}

// 使用方式
let history = temporal_rg.get_or_create_temporal("taa_history", ImageDesc { ... })?;
// 首帧：创建新 Image + 导入
// 后续帧：导入上一帧导出的 Image（携带正确的 access_type）
```

生命周期流转：

```
prepare_frame():
    Inert → get_or_create_temporal() → Imported
    → 用户在 RG 中 read/write → export_temporal() → Exported

draw_frame():
    → execute → retire_temporal() → Inert（更新 access_type 为执行后的最终状态）

下一帧:
    Inert → get_or_create_temporal() → Imported ...
```

### 3.8 执行流程

```
┌─────────────────────────────────────────────────────────────────┐
│ prepare_frame()                                                 │
│   TemporalRenderGraph::new()                                    │
│   → 用户通过 rg.add_pass() + SimpleRenderPass 定义 Pass         │
│   → rg.export_temporal()  → (RenderGraph, ExportedState)        │
│   → RenderGraph::compile(pipeline_cache) → CompiledRenderGraph  │
│     ├─ 聚合资源 usage flags                                     │
│     └─ 编译所有 pipeline（compute/raster/rt）                   │
├─────────────────────────────────────────────────────────────────┤
│ draw_frame()                                                    │
│   CompiledRenderGraph::begin_execute()                          │
│     └─ 从 transient cache 分配资源 → ExecutingRenderGraph       │
│   ExecutingRenderGraph::record_main_cb(cb)                      │
│     ├─ 批量首次访问转换                                         │
│     └─ 逐 Pass: transition_resource() → render_fn()            │
│   ExecutingRenderGraph::record_presentation_cb(cb, swapchain)   │
│     ├─ 替换 pending swapchain 资源                              │
│     ├─ 录制剩余 Pass                                           │
│     └─ 转换导出资源到请求的 access type                         │
│   → RetiredRenderGraph                                          │
│     ├─ release_resources(transient_cache) — 归还 transient 资源 │
│     └─ retire_temporal() — 更新 temporal 状态                   │
└─────────────────────────────────────────────────────────────────┘
```

### 3.9 设计特点总结

| 特点 | 说明 |
|------|------|
| **Rust 编译期类型安全** | `Handle<Image>` vs `Ref<Image, GpuSrv>` 防止资源误用 |
| **零开销抽象** | PhantomData 标记，运行时无额外开销 |
| **Temporal 资源一等公民** | 内建跨帧持久资源管理（TAA History、光照缓存等） |
| **vk_sync 集成** | 精确的 Vulkan 同步原语 |
| **Builder + 链式调用** | 人体工学友好的 Pass 定义 API |
| **无 Pass 裁剪** | 按录制顺序执行（简化实现，依赖用户正确组织） |

---

## 四、Unity (Render Graph / SRP)

> - 官方文档: [Unity 6 Render Graph](https://docs.unity3d.com/6000.0/Documentation/Manual/render-graph.html)
> - URP Render Graph: [URP 17.0 Render Graph](https://docs.unity3d.com/Packages/com.unity.render-pipelines.universal@17.0/manual/render-graph.html)
> - SRP Core 源码: [Unity-Technologies/Graphics](https://github.com/Unity-Technologies/Graphics) → `Packages/com.unity.render-pipelines.core/Runtime/RenderGraph/`
> - SRP Core 文档: [Render Graph System (SRP Core)](https://docs.unity3d.com/Packages/com.unity.render-pipelines.core@17.0/manual/render-graph-system.html)

### 4.1 核心架构

Unity 的 Render Graph 位于 SRP Core 包中，被 URP 和 HDRP 共用。

| 类/概念 | 职责 |
|---------|------|
| `RenderGraph` | 顶层图对象，管理 Pass 注册、资源创建、编译执行 |
| `RenderGraphBuilder` | 添加 Pass 时返回的构建器（Unity 6+） |
| `TextureHandle` / `BufferHandle` | 轻量级资源句柄（struct，索引到图的资源注册表） |
| `TextureDesc` / `BufferDesc` | 资源描述符 |
| `RendererListHandle` | 渲染列表句柄（DrawCall 集合） |
| `RasterGraphContext` | 光栅 Pass 执行上下文（RasterCommandBuffer） |
| `ComputeGraphContext` | 计算 Pass 执行上下文（ComputeCommandBuffer） |
| `UnsafeGraphContext` | 非安全 Pass 执行上下文（UnsafeCommandBuffer） |
| `ContextContainer` | URP 帧数据容器 |

资源句柄内部结构：

```csharp
public struct ResourceHandle {
    internal int index;            // 资源注册表索引
    internal ResourceType type;    // Texture / Buffer
    internal int version;          // 写操作版本追踪
}

public struct TextureHandle {
    internal ResourceHandle handle;
    public static readonly TextureHandle nullHandle;
}
```

### 4.2 Pass 类型（Unity 6+）

Unity 6 引入了强类型的 Pass 分类：

| Pass 类型 | 方法 | 执行上下文 | 用途 |
|-----------|------|-----------|------|
| **Raster Pass** | `AddRasterRenderPass<T>()` | `RasterGraphContext` | 光栅化（绘制几何体、全屏效果） |
| **Compute Pass** | `AddComputePass<T>()` | `ComputeGraphContext` | 计算着色器调度 |
| **Unsafe Pass** | `AddUnsafePass<T>()` | `UnsafeGraphContext` | 逃生舱（兼容旧 CommandBuffer 代码） |
| **Low-Level Raster** | `AddLowLevelRasterRenderPass<T>()` | — | Native render pass 手动控制 |

每种 Context 的 CommandBuffer 类型受限于合法操作——`RasterCommandBuffer` 不能 dispatch compute，`ComputeCommandBuffer` 不能绘制几何体，**编译期**防止无效操作。

### 4.3 PassData 模式

所有 Pass 使用 **PassData 类**在录制阶段和执行阶段之间传递数据：

```csharp
// Raster Pass 完整示例
class BlitPassData {
    public TextureHandle source;
    public TextureHandle destination;
    public Material material;
}

public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
{
    var resourceData = frameData.Get<UniversalResourceData>();

    // 创建 transient 纹理
    var desc = renderGraph.GetTextureDesc(resourceData.activeColorTexture);
    desc.name = "BlitIntermediate";
    TextureHandle tempTex = renderGraph.CreateTexture(desc);

    using (var builder = renderGraph.AddRasterRenderPass<BlitPassData>(
        "Blit Effect", out var passData))
    {
        // 配置 PassData
        passData.source = resourceData.activeColorTexture;
        passData.destination = tempTex;
        passData.material = myMaterial;

        // 声明资源使用
        builder.UseTexture(passData.source, AccessFlags.Read);
        builder.SetRenderAttachment(passData.destination, 0, AccessFlags.Write);

        // 设置执行函数（Lambda 在图执行阶段调用）
        builder.SetRenderFunc((BlitPassData data, RasterGraphContext ctx) => {
            Blitter.BlitTexture(ctx.cmd, data.source,
                new Vector4(1, 1, 0, 0), data.material, 0);
        });
    }
}
```

**Compute Pass 示例**：

```csharp
class ComputePassData {
    public BufferHandle inputBuffer;
    public BufferHandle outputBuffer;
    public TextureHandle outputTexture;
    public ComputeShader computeShader;
    public int kernelIndex;
}

using (var builder = renderGraph.AddComputePass<ComputePassData>(
    "My Compute", out var passData))
{
    passData.computeShader = myCS;
    passData.kernelIndex = myCS.FindKernel("CSMain");
    passData.outputTexture = renderGraph.CreateTexture(new TextureDesc(256, 256) {
        colorFormat = GraphicsFormat.R32G32B32A32_SFloat,
        enableRandomWrite = true
    });

    builder.UseBuffer(passData.inputBuffer, AccessFlags.Read);
    builder.UseTexture(passData.outputTexture, AccessFlags.Write);

    builder.SetRenderFunc((ComputePassData data, ComputeGraphContext ctx) => {
        ctx.cmd.SetComputeTextureParam(data.computeShader, data.kernelIndex,
            "_OutputTex", data.outputTexture);
        ctx.cmd.DispatchCompute(data.computeShader, data.kernelIndex, 32, 32, 1);
    });
}
```

### 4.4 资源声明 API（Unity 6+）

```csharp
// 读取纹理
builder.UseTexture(handle, AccessFlags.Read);

// 写入渲染目标（Color Attachment）
builder.SetRenderAttachment(handle, colorIndex, AccessFlags.Write);

// 深度附件
builder.SetRenderAttachmentDepth(depthHandle, AccessFlags.Write);  // 读写
builder.SetRenderAttachmentDepth(depthHandle, AccessFlags.Read);   // 只读深度

// 输入附件（Subpass Input，用于 Native Render Pass 合并）
builder.SetInputAttachment(handle, inputIndex, AccessFlags.Read);

// Buffer
builder.UseBuffer(bufferHandle, AccessFlags.ReadWrite);

// 渲染列表
builder.UseRendererList(rendererListHandle);

// 全局纹理（在此 Pass 之后全局可用）
builder.SetGlobalTextureAfterPass(handle, shaderPropertyId);

// 禁止裁剪
builder.AllowPassCulling(false);
```

### 4.5 资源管理

**Transient 资源**：

```csharp
// 创建（描述符指定属性，图管理生命周期）
TextureHandle tex = renderGraph.CreateTexture(new TextureDesc(Screen.width, Screen.height) {
    colorFormat = GraphicsFormat.R8G8B8A8_UNorm,
    clearBuffer = true,
    clearColor = Color.clear,
    name = "TempTexture",
});

// 也可用缩放因子
TextureHandle halfRes = renderGraph.CreateTexture(new TextureDesc(Vector2.one * 0.5f) {
    colorFormat = GraphicsFormat.B10G11R11_UFloatPack32,
});
```

**Imported 资源**：

```csharp
TextureHandle imported = renderGraph.ImportTexture(myRTHandle);
TextureHandle backbuffer = renderGraph.ImportBackbuffer(BuiltinRenderTextureType.CameraTarget);
BufferHandle importedBuf = renderGraph.ImportBuffer(myGraphicsBuffer);
```

**池化复用**：兼容描述符 + 不重叠生命周期的 Transient 资源自动共享同一物理 GPU 资源。底层通过 `RTHandleSystem` 管理，支持动态分辨率缩放。

### 4.6 编译流程

```
┌─────────────────────────────────────────────────────────────────┐
│ Phase 1: Recording                                              │
│   用户通过 AddXxxPass() 添加 Pass，声明资源使用 → 构建 DAG     │
├─────────────────────────────────────────────────────────────────┤
│ Phase 2: Compilation                                            │
│   ① Dependency Analysis — 根据资源读写构建邻接表                │
│   ② Pass Culling — 从 imported 输出反向遍历，移除未使用 Pass   │
│   ③ Resource Lifetime — 计算每个 transient 资源的 [first, last] │
│   ④ Pass Merging — 合并相邻兼容 raster pass 为 native render   │
│     pass（subpass），关键移动端优化                              │
│   ⑤ Barrier Insertion — 自动插入资源状态转换                    │
├─────────────────────────────────────────────────────────────────┤
│ Phase 3: Execution                                              │
│   按拓扑序执行 Pass，从 pool 获取/归还资源                      │
└─────────────────────────────────────────────────────────────────┘
```

### 4.7 Native Render Pass 合并（移动端优化）

Unity 6 编译器可**自动合并**相邻兼容 raster pass 为单一 native render pass：

```csharp
// Pass 1: GBuffer 写入
builder.SetRenderAttachment(gBuffer0, 0, AccessFlags.Write);
builder.SetRenderAttachment(gBuffer1, 1, AccessFlags.Write);
builder.SetRenderAttachmentDepth(depthBuffer, AccessFlags.Write);

// Pass 2: Deferred Lighting 读取 GBuffer（通过 input attachment）
builder.SetInputAttachment(gBuffer0, 0, AccessFlags.Read);  // subpass input!
builder.SetInputAttachment(gBuffer1, 1, AccessFlags.Read);
builder.SetRenderAttachment(colorResult, 0, AccessFlags.Write);

// → 编译器自动合并为单一 native render pass 的 2 个 subpass
// → GBuffer 数据留在 tile memory，避免昂贵的 DRAM 读写
```

对应底层 API：
- **Vulkan**: `VkRenderPass` + `VkSubpassDescription`
- **Metal**: `MTLRenderPassDescriptor` + framebuffer fetch

### 4.8 自动屏障管理

根据声明的 `AccessFlags`，编译器自动插入：

- **纹理 layout 转换**：RenderTarget → ShaderResource → UnorderedAccess
- **UAV 屏障**：compute 写 → 后续读
- **缓存刷新**：确保写操作对后续读可见

映射到不同 API：
- **Vulkan**: `vkCmdPipelineBarrier`
- **D3D12**: `ID3D12GraphicsCommandList::ResourceBarrier`
- **Metal**: `MTLFence` / render pass load/store actions

### 4.9 演进历史

| 时间 | 里程碑 |
|------|--------|
| **2018 (Unity 2018.1)** | SRP 引入，CommandBuffer 方式 |
| **2019-2020** | HDRP 内部采用 Render Graph |
| **2021 (Unity 2021.2)** | HDRP Render Graph 成熟，API: `AddRenderPass<T>()` |
| **2023 (Unity 2023.1)** | URP 开始实验性 Render Graph 支持 |
| **Unity 6 (2024)** | **Render Graph 成为 URP 强制要求**，新增类型化 API |
| **2024-2025** | Native render pass 合并优化，Render Graph Viewer |

**旧 CommandBuffer 方式 vs 新 Render Graph**：

```csharp
// 旧（手动管理一切）
public override void Execute(ScriptableRenderContext context, ref RenderingData data) {
    CommandBuffer cmd = CommandBufferPool.Get("MyPass");
    cmd.GetTemporaryRT(tempId, desc);         // 手动分配
    cmd.SetRenderTarget(tempId);              // 手动设置 RT
    cmd.Blit(source, tempId, material);       // 执行
    cmd.ReleaseTemporaryRT(tempId);           // 手动释放
    context.ExecuteCommandBuffer(cmd);
}

// 新（声明式，系统自动处理）
public override void RecordRenderGraph(RenderGraph rg, ContextContainer frameData) {
    using (var builder = rg.AddRasterRenderPass<PassData>("MyPass", out var data)) {
        data.dest = rg.CreateTexture(desc);   // 图管理生命周期
        builder.SetRenderAttachment(data.dest, 0, AccessFlags.Write);  // 声明式
        builder.SetRenderFunc((d, ctx) => { Blitter.BlitTexture(ctx.cmd, ...); });
    }
}
```

### 4.10 URP vs HDRP 差异

| 方面 | HDRP | URP |
|------|------|-----|
| 采用时间 | 2020 起 | Unity 6 (2024) |
| 图复杂度 | 高（体积雾、SSS、屏幕空间阴影等） | 较低 |
| 兼容模式 | 不需要（从早期就基于图） | 提供（旧 Execute() 自动包装为 Unsafe Pass） |
| 移动端优化 | 次要 | **关键**（Native Render Pass 合并） |
| 帧数据访问 | HDCamera / HDRenderPipelineAsset | `ContextContainer` → `UniversalResourceData` 等 |
| 共享基础 | \- | SRP Core 的 RenderGraph、资源句柄、编译裁剪、池化 |

### 4.11 设计特点总结

| 特点 | 说明 |
|------|------|
| **类型化 CommandBuffer** | RasterCommandBuffer / ComputeCommandBuffer 编译期安全 |
| **Native Render Pass 自动合并** | 移动端 TBDR GPU 的关键优化 |
| **兼容模式** | 旧 Execute() 自动包装为 Unsafe Pass，渐进迁移 |
| **Render Graph Viewer** | 编辑器可视化工具（Window > Analysis > Render Graph Viewer） |
| **PassData 模式** | 录制/执行分离，数据通过 PassData 类传递 |
| **ContextContainer** | URP 帧数据访问的统一接口 |

---

## 五、Unreal Engine (RDG)

> - 官方文档: [Render Dependency Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
> - RDG 编程指南: [RDG Programmer's Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/rdg-programmers-guide-for-unreal-engine)
> - 源码（需 Epic 账号）: [EpicGames/UnrealEngine](https://github.com/EpicGames/UnrealEngine) → `Engine/Source/Runtime/RenderCore/`
> - 灵感来源: [FrameGraph (Frostbite, GDC 2017)](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in)

### 5.1 核心架构

RDG 系统位于 `Engine/Source/Runtime/RenderCore/`：

| 类 | 文件 | 职责 |
|----|------|------|
| `FRDGBuilder` | `RenderGraphBuilder.h/cpp` | 中央调度器（每帧一个实例） |
| `FRDGTexture` / `FRDGBuffer` | `RenderGraphResources.h` | 轻量级资源代理（延迟分配） |
| `FRDGTextureSRV/UAV` / `FRDGBufferSRV/UAV` | `RenderGraphResources.h` | 视图类 |
| `FRDGPass` | `RenderGraphPass.h/cpp` | Pass 节点 |
| `FRDGTextureDesc` / `FRDGBufferDesc` | `RenderGraphResources.h` | 资源描述符 |
| 工具函数 | `RenderGraphUtils.h` | AddCopyTexturePass 等辅助函数 |

引用类型（原始指针，仅在 FRDGBuilder 生命周期内有效）：

```cpp
using FRDGTextureRef    = FRDGTexture*;
using FRDGBufferRef     = FRDGBuffer*;
using FRDGTextureSRVRef = FRDGTextureSRV*;
using FRDGTextureUAVRef = FRDGTextureUAV*;
using FRDGBufferSRVRef  = FRDGBufferSRV*;
using FRDGBufferUAVRef  = FRDGBufferUAV*;
```

### 5.2 SHADER_PARAMETER 宏系统（声明式依赖）

这是 UE RDG 最核心的设计——**资源依赖通过参数结构体声明**。编译器通过反射参数结构体自动提取依赖关系。

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FMyPassParameters, )
    // 纹理 SRV 读
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
    // 纹理 UAV 写
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
    // Buffer SRV 读
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InputBuffer)
    // Buffer UAV 写
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputBuffer)
    // 显式 SRV 描述符
    SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, ExplicitSRV)
    // 采样器
    SHADER_PARAMETER_SAMPLER(SamplerState, MySampler)
    // 常量
    SHADER_PARAMETER(FVector4f, SomeConstant)
    SHADER_PARAMETER(uint32, Width)
    // 渲染目标绑定槽（光栅 Pass）
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()
```

关键宏说明：

| 宏 | 声明的依赖类型 |
|----|---------------|
| `SHADER_PARAMETER_RDG_TEXTURE` | SRV 读取纹理 |
| `SHADER_PARAMETER_RDG_TEXTURE_UAV` | UAV 读写纹理 |
| `SHADER_PARAMETER_RDG_TEXTURE_SRV` | 显式 SRV 描述符 |
| `SHADER_PARAMETER_RDG_BUFFER_SRV` | SRV 读取 Buffer |
| `SHADER_PARAMETER_RDG_BUFFER_UAV` | UAV 读写 Buffer |
| `RENDER_TARGET_BINDING_SLOTS()` | 光栅 Pass 的渲染目标输出 |

### 5.3 AddPass API

```cpp
// 模板函数签名
template <typename ParameterStructType, typename LambdaType>
FRDGPassRef FRDGBuilder::AddPass(
    FRDGEventName&& Name,                       // Pass 名（支持格式化）
    const ParameterStructType* ParameterStruct,  // 参数结构体（声明依赖）
    ERDGPassFlags Flags,                         // Pass 类型标志
    LambdaType&& Lambda                          // 执行 Lambda
);
```

### 5.4 ERDGPassFlags

| Flag | 说明 |
|------|------|
| `Raster` | 光栅化 Pass（有渲染目标） |
| `Compute` | 计算 Pass |
| `AsyncCompute` | 异步计算队列执行 |
| `Copy` | 拷贝操作 |
| `NeverCull` | 禁止裁剪（即使输出未使用） |
| `SkipRenderPass` | 跳过自动 render pass begin/end |
| `UntrackedAccess` | 访问 RDG 追踪范围外的资源 |

### 5.5 完整 Pass 示例

**Compute Pass**：

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FMyComputeParams, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputTexture)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
    SHADER_PARAMETER(uint32, TextureWidth)
    SHADER_PARAMETER(uint32, TextureHeight)
END_SHADER_PARAMETER_STRUCT()

void AddMyComputePass(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap,
    FRDGTextureRef InputTexture, FRDGTextureRef OutputTexture)
{
    // 1. 从图的线性分配器分配参数
    FMyComputeParams* Params = GraphBuilder.AllocParameters<FMyComputeParams>();

    // 2. 填充资源绑定（隐式声明依赖）
    Params->InputTexture = InputTexture;
    Params->OutputTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutputTexture));
    Params->TextureWidth = Width;
    Params->TextureHeight = Height;

    // 3. 获取 Shader
    TShaderMapRef<FMyComputeShader> ComputeShader(ShaderMap);

    // 4. 添加 Pass
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("MyCompute %dx%d", Width, Height),
        ComputeShader,
        Params,
        FComputeShaderUtils::GetGroupCount(FIntPoint(Width, Height), FIntPoint(8, 8))
    );
}
```

**Raster Pass**：

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FMyRasterParams, )
    SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneColor)
    SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
    RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void AddMyRasterPass(FRDGBuilder& GraphBuilder, const FViewInfo& View,
    FRDGTextureSRVRef SceneColorSRV, FRDGTextureRef OutputTexture)
{
    FMyRasterParams* Params = GraphBuilder.AllocParameters<FMyRasterParams>();
    Params->SceneColor = SceneColorSRV;
    Params->SceneColorSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
    Params->RenderTargets[0] = FRenderTargetBinding(
        OutputTexture, ERenderTargetLoadAction::EClear);

    GraphBuilder.AddPass(
        RDG_EVENT_NAME("MyRasterPass"),
        Params,
        ERDGPassFlags::Raster,
        [&View, Params](FRHICommandList& RHICmdList) {
            RHICmdList.SetViewport(0, 0, 0.f,
                View.ViewRect.Width(), View.ViewRect.Height(), 1.f);
            // ... 绘制几何体 ...
        }
    );
}
```

**Pipeline 使用模式**：

```cpp
void FDeferredShadingSceneRenderer::Render(FRHICommandListImmediate& RHICmdList)
{
    FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("SceneRenderer"));

    // 注册外部资源
    FRDGTextureRef SceneColor = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor());
    FRDGTextureRef SceneDepth = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);

    // 创建 transient 资源
    FRDGTextureRef Velocity = GraphBuilder.CreateTexture(
        FRDGTextureDesc::Create2D(Extent, PF_G16R16, FClearValueBinding::Black,
            TexCreate_RenderTargetable | TexCreate_ShaderResource),
        TEXT("Velocity"));

    // 添加 Pass 链
    AddBasePass(GraphBuilder, ...);
    AddLightingPass(GraphBuilder, ...);
    AddPostProcessPass(GraphBuilder, ...);

    // 提取执行后需要保持的资源
    GraphBuilder.QueueTextureExtraction(SceneColor, &SceneContext.PooledRT);

    // 编译并执行
    GraphBuilder.Execute();
}
```

### 5.6 资源管理

**Transient 资源**（延迟分配）：

```cpp
// 创建时只记录描述符，不分配 GPU 内存
FRDGTextureRef Tex = GraphBuilder.CreateTexture(
    FRDGTextureDesc::Create2D(Size, PF_R8G8B8A8, ClearValue, Flags),
    TEXT("MyTexture"));

// 编译时计算生命周期 → 执行时从 pool 获取 → 最后使用后归还 pool
```

**External 资源**（持久化）：

```cpp
// 导入已有资源
FRDGTextureRef Ext = GraphBuilder.RegisterExternalTexture(PooledRT, TEXT("History"));

// 导出执行后保持的资源
TRefCountPtr<IPooledRenderTarget> PersistentRT;
GraphBuilder.QueueTextureExtraction(SomeTexture, &PersistentRT);
```

**内存别名**：

```
Resource A: [Pass 1 .... Pass 5]
Resource B:                       [Pass 7 .... Pass 12]
→ A 和 B 可共享同一 GPU 物理内存（D3D12 placed resources / Vulkan memory aliasing）
```

UE5 的 `FRHITransientResourceAllocator` 从大堆上子分配，进一步优化内存别名效率。

### 5.7 编译流程

```
┌─────────────────────────────────────────────────────────────────┐
│ Phase 1: Dependency Analysis                                    │
│   遍历所有 Pass 的 SHADER_PARAMETER 结构体                      │
│   → 自动提取每个 Pass 的资源读写依赖                            │
│   → 构建 Pass-to-Pass DAG                                      │
├─────────────────────────────────────────────────────────────────┤
│ Phase 2: Pass Culling                                           │
│   从 "root" Pass 反向遍历：                                     │
│   ├─ NeverCull 标志的 Pass                                      │
│   ├─ 写入被 Extract 资源的 Pass                                 │
│   └─ 有外部副作用的 Pass                                       │
│   未被遍历到的 Pass 全部裁剪                                    │
├─────────────────────────────────────────────────────────────────┤
│ Phase 3: Resource Scheduling                                    │
│   计算每个资源的 [firstPass, lastPass]                          │
│   → 判断内存别名机会                                            │
│   → 从 pool 分配物理资源                                       │
├─────────────────────────────────────────────────────────────────┤
│ Phase 4: Barrier Computation                                    │
│   ├─ 计算最优屏障位置                                           │
│   ├─ 合并多个转换为单次 Transition() 调用                      │
│   └─ 插入 Split Barrier（begin/end 分离）                      │
├─────────────────────────────────────────────────────────────────┤
│ Phase 5: Execution                                              │
│   按录制顺序执行（跳过被裁剪的 Pass）                           │
│   → 屏障 → 获取资源 → 执行 Lambda → 释放资源                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.8 屏障系统

**ERHIAccess 标志**：

| 标志 | 说明 |
|------|------|
| `SRVCompute` / `SRVGraphics` | Shader Resource View 读取 |
| `UAVCompute` / `UAVGraphics` | Unordered Access View 读写 |
| `RTV` | 渲染目标写入 |
| `DSVRead` / `DSVWrite` | 深度模板读/写 |
| `CopySrc` / `CopyDest` | 拷贝源/目标 |
| `IndirectArgs` | 间接调度/绘制参数 |
| `VertexOrIndexBuffer` | 顶点/索引缓冲 |

**自动屏障插入**：

```cpp
// 内部简化示意
void FRDGBuilder::ExecutePass(FRDGPass* Pass) {
    TArray<FRHITransitionInfo> Transitions;
    for (FRDGResource* Res : Pass->GetInputResources()) {
        ERHIAccess Previous = Res->GetCurrentAccess();
        ERHIAccess Required = Pass->GetRequiredAccess(Res);
        if (Previous != Required) {
            Transitions.Add(FRHITransitionInfo(Res->GetRHI(), Previous, Required));
        }
    }
    if (Transitions.Num() > 0)
        RHICmdList.Transition(MakeArrayView(Transitions));  // 批量屏障
    Pass->Execute(RHICmdList);
}
```

**Split Barrier**（GPU 并行优化）：

```
Pass 5: 最后一次写 TextureA (UAV)
  → BeginTransition(TextureA, UAV → SRV)     // 提前开始转换

Pass 6, 7, 8: 其他工作（转换与这些 Pass 重叠执行）

Pass 9: 首次读 TextureA (SRV)
  → EndTransition(TextureA, UAV → SRV)       // 完成转换
```

**子资源级别追踪**：不同 mip level / array slice 可在不同状态下同时使用，RDG 独立追踪每个子资源。

### 5.9 异步计算

```cpp
// 声明异步计算 Pass
GraphBuilder.AddPass(
    RDG_EVENT_NAME("AsyncOcclusionCull"),
    Params,
    ERDGPassFlags::AsyncCompute,    // 异步计算队列
    [Params, Shader](FRHIComputeCommandList& RHICmdList) {
        FComputeShaderUtils::Dispatch(RHICmdList, Shader, *Params, GroupCount);
    }
);
```

RDG 自动管理跨队列同步：

```
Graphics Queue:  [Pass A] ─signal→ [Pass C] [Pass D] ─wait→ [Pass F]
                                                   │
Async Compute:           ─wait→ [Pass B (async)] ─signal→
```

- Graphics → Async Compute：在 graphics 队列 signal，async 队列 wait
- Async Compute → Graphics：在 async 队列 signal，graphics 队列 wait
- Fence 位置自动优化（尽晚 signal、尽早 wait）

### 5.10 调试工具

| 控制台变量 | 说明 |
|-----------|------|
| `r.RDG.ImmediateMode 1` | 立即执行模式（禁用所有优化，便于定位问题） |
| `r.RDG.Debug 1` | 验证层（检测未声明的资源访问、参数结构体错误等） |
| `r.RDG.CullPasses 0` | 禁用 Pass 裁剪 |
| `r.RDG.DumpGraph 1` | 导出 Graphviz DOT 格式图结构 |
| `r.RDG.TransitionLog 1` | 记录所有资源状态转换 |
| `r.RDG.BreakpointPass "Name"` | 在指定 Pass 断点 |
| `r.RDG.AsyncCompute 0` | 禁用异步计算 |
| `r.RDG.OverlapUAVs 1` | UAV overlap 优化 |

**Unreal Insights 集成**：每个 `RDG_EVENT_NAME()` 创建性能标记，可在时间线中查看资源生命周期、转换、异步计算重叠。

### 5.11 演进历史

| 版本 | 里程碑 |
|------|--------|
| **UE 4.22 (2019)** | 实验性 RDG，少量 Pass 使用 |
| **UE 4.23-4.25** | 更多 Pass 迁移，API 稳定 |
| **UE 4.26** | 大规模扩展，延迟着色大量 Pass 迁移 |
| **UE 5.0 (2022)** | 全面推进——Nanite、Lumen、Virtual Shadow Maps 全基于 RDG |
| **UE 5.1-5.3** | 优化 Split Barrier、异步计算、资源别名 |
| **UE 5.4+** | 编译开销优化、并行 Pass 设置 |

### 5.12 设计特点总结

| 特点 | 说明 |
|------|------|
| **宏反射声明依赖** | `SHADER_PARAMETER_RDG_*` 宏，编译器自动提取依赖 |
| **Split Barrier** | 最优化的 GPU 并行（begin/end 分离） |
| **异步计算一等公民** | 自动 GPU fence 管理、跨队列同步 |
| **最成熟工业级实现** | Nanite/Lumen/VSM 全部基于 RDG |
| **强大调试工具** | ImmediateMode、Insights、Graphviz 导出 |
| **延迟分配 + 内存别名** | Transient Allocator 大堆子分配 |

---

## 六、Pass 内部如何发出绘制指令

Render Graph 解决的是 Pass 之间的调度、资源管理和同步问题。但最终让模型真正渲染出来，还需要在 Pass 的执行回调内部发出实际的 GPU 绘制/计算指令。这一节详细分析各框架从 Pass 执行到 GPU 指令的完整调用链。

### 6.1 Falcor：RenderContext + GraphicsState + ProgramVars

#### 调用链概览

```
RenderGraphExe::execute()
  → 遍历 mExecutionList
    → 构造 RenderData（封装 ResourceCache）
      → pass.pPass->execute(pRenderContext, renderData)
        → 用户在 execute() 内调用 RenderContext 的绘制 API
```

#### 类继承层次

```
CopyContext                    ← 拷贝操作（copyResource, updateBuffer, readBuffer...）
  └─ ComputeContext            ← 计算着色器（dispatch, dispatchIndirect, clearUAV...）
      └─ RenderContext         ← 完整图形（draw*, blit, clear*, raytrace...）
```

#### RenderContext 绘制 API

`RenderContext` 是最终发出 GPU 指令的核心类（`Source/Falcor/Core/API/RenderContext.h`），每个绘制方法都需要两个关键参数：**状态对象** + **资源绑定**。

```cpp
class RenderContext : public ComputeContext {
public:
    // ── 基础绘制 ──
    void draw(GraphicsState* pState, ProgramVars* pVars,
              uint32_t vertexCount, uint32_t startVertex);

    void drawInstanced(GraphicsState* pState, ProgramVars* pVars,
                       uint32_t vertexCount, uint32_t instanceCount,
                       uint32_t startVertex, uint32_t startInstance);

    void drawIndexed(GraphicsState* pState, ProgramVars* pVars,
                     uint32_t indexCount, uint32_t startIndex, int32_t baseVertex);

    void drawIndexedInstanced(GraphicsState* pState, ProgramVars* pVars,
                              uint32_t indexCount, uint32_t instanceCount,
                              uint32_t startIndex, int32_t baseVertex,
                              uint32_t startInstance);

    // ── 间接绘制 ──
    void drawIndirect(GraphicsState* pState, ProgramVars* pVars,
                      uint32_t maxCommandCount, const Buffer* pArgBuffer,
                      uint64_t offset, const Buffer* pCountBuffer, uint64_t countOffset);

    void drawIndexedIndirect(GraphicsState* pState, ProgramVars* pVars,
                             uint32_t maxCommandCount, const Buffer* pArgBuffer,
                             uint64_t offset, const Buffer* pCountBuffer,
                             uint64_t countOffset);

    // ── 光线追踪 ──
    void raytrace(Program* pProgram, RtProgramVars* pVars,
                  uint32_t width, uint32_t height, uint32_t depth);

    // ── 清除 / Blit ──
    void clearFbo(const Fbo* pFbo, const float4& color, float depth, uint8_t stencil, ...);
    void clearRtv(const RenderTargetView* pRtv, const float4& color);
    void blit(const ref<ShaderResourceView>& pSrc, const ref<RenderTargetView>& pDst, ...);
};
```

#### 两大核心对象

**① GraphicsState — 管线状态**（`Source/Falcor/Core/State/GraphicsState.h`）

```cpp
class GraphicsState : public Object {
public:
    GraphicsState& setProgram(const ref<Program>& pProgram);      // 着色器程序
    GraphicsState& setVao(const ref<Vao>& pVao);                  // 顶点/索引缓冲 + 布局
    GraphicsState& setFbo(const ref<Fbo>& pFbo, bool setVp = true); // 渲染目标
    GraphicsState& setBlendState(ref<BlendState>);                // 混合状态
    GraphicsState& setRasterizerState(ref<RasterizerState>);      // 光栅化状态
    GraphicsState& setDepthStencilState(ref<DepthStencilState>);  // 深度模板
    void setViewport(uint32_t index, const Viewport& vp, ...);   // 视口
    void setScissors(uint32_t index, const Scissor& sc);          // 裁剪矩形

    // 获取编译后的 PSO（Pipeline State Object）
    ref<GraphicsStateObject> getGSO(const ProgramVars* pVars);
};
```

**② ProgramVars — 着色器资源绑定**（`Source/Falcor/Core/Program/ProgramVars.h`）

```cpp
class ProgramVars : public ParameterBlock {
    // 继承自 ParameterBlock，提供资源绑定：
    void setTexture(std::string_view name, const ref<Texture>& pTexture);
    void setBuffer(std::string_view name, const ref<Buffer>& pBuffer);
    void setSrv(const BindLocation&, const ref<ShaderResourceView>&);
    void setUav(const BindLocation&, const ref<UnorderedAccessView>&);
    void setSampler(std::string_view name, const ref<Sampler>& pSampler);
    void setParameterBlock(std::string_view name, const ref<ParameterBlock>& pBlock);

    // 便捷语法：通过 ShaderVar 下标赋值
    ShaderVar getRootVar() const;
    // 用法：var["gTexture"] = myTexture; var["CB"]["gParam"] = value;
};
```

#### 内部机制：drawCallCommon()

每个 draw 方法内部都先调用 `drawCallCommon()`（`RenderContext.cpp`），执行以下步骤：

```
drawCallCommon(pState, pVars)
 ├─ pVars->prepareDescriptorSets()          // 将所有绑定的资源写入描述符集
 ├─ ensureFboAttachmentResourceStates()     // 为渲染目标插入屏障
 ├─ pState->getGSO(pVars)                   // 获取/创建 PSO
 ├─ resourceBarrier(vertexBuffer, VertexBuffer)  // 为 VB/IB 插入屏障
 ├─ encoder = getRenderCommandEncoder(...)  // 获取底层命令编码器（开始 render pass）
 ├─ encoder->bindPipelineWithRootObject(pso, shaderObject)  // 绑定 PSO
 ├─ encoder->setVertexBuffer(...)           // 设置顶点缓冲
 ├─ encoder->setIndexBuffer(...)            // 设置索引缓冲
 ├─ encoder->setPrimitiveTopology(...)      // 设置图元拓扑
 ├─ encoder->setViewports(...)              // 设置视口
 └─ encoder->setScissorRects(...)           // 设置裁剪矩形
 → 返回 encoder

然后：
 encoder->draw(vertexCount, startVertex)              // 或
 encoder->drawIndexed(indexCount, startIndex, base)   // 或
 encoder->drawIndexedIndirect(count, argBuffer, ...)  // 实际 GPU 指令
```

#### 场景渲染：Scene::rasterize()

Falcor 将所有网格数据合并到一个大 VAO 中，使用 **Indirect Draw** 绘制整个场景（`Source/Falcor/Scene/Scene.cpp`）：

```cpp
void Scene::rasterize(RenderContext* pRenderContext, GraphicsState* pState,
                      ProgramVars* pVars,
                      const ref<RasterizerState>& pRasterizerStateCW,
                      const ref<RasterizerState>& pRasterizerStateCCW)
{
    // 绑定场景参数块（包含所有材质、变换、灯光等）
    pVars->setParameterBlock(kParameterBlockName, mpSceneBlock);

    for (const auto& draw : mDrawArgs) {
        // 设置 VAO（顶点 + 索引缓冲）
        pState->setVao(draw.ibFormat == R16Uint ? mpMeshVao16Bit : mpMeshVao);
        // 设置面剔除方向
        pState->setRasterizerState(draw.ccw ? pRasterizerStateCCW : pRasterizerStateCW);

        // 间接绘制：一次调用绘制多个 mesh
        if (isIndexed)
            pRenderContext->drawIndexedIndirect(pState, pVars, draw.count,
                                                draw.pBuffer.get(), 0, nullptr, 0);
        else
            pRenderContext->drawIndirect(pState, pVars, draw.count,
                                         draw.pBuffer.get(), 0, nullptr, 0);
    }
}
```

VAO 的顶点布局（`Scene::createMeshVao()`）：

```
静态顶点数据: POSITION(RGB32Float) + PACKED_NORMAL_TANGENT(RGB32Float) + TEXCOORD(RG32Float)
实例数据:     DRAW_ID(R16Uint/R32Uint) ← 标识哪个 mesh instance
索引缓冲:     所有 mesh 的三角形索引合并
```

#### 完整示例：GBufferRaster Pass

```cpp
void GBufferRaster::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // ① 从 RenderData 获取图分配的资源
    auto pDepth = renderData.getTexture("depth");

    // ② 清除渲染目标
    pRenderContext->clearDsv(pDepth->getDSV().get(), 1.f, 0);
    for (size_t i = 0; i < kGBufferChannels.size(); ++i) {
        ref<Texture> pTex = getOutput(renderData, kGBufferChannels[i].name);
        mpFbo->attachColorTarget(pTex, uint32_t(i));
    }
    pRenderContext->clearFbo(mpFbo.get(), float4(0), 1.f, 0, FboAttachmentType::Color);

    // ③ 深度预 Pass
    mDepthPass.pState->setProgram(mDepthPass.pProgram);    // 设置着色器
    mDepthPass.pState->setFbo(mpFbo);                       // 设置渲染目标
    mpScene->rasterize(pRenderContext, mDepthPass.pState.get(),
                       mDepthPass.pVars.get(), cullMode);   // 绘制场景

    // ④ GBuffer Pass
    auto var = mGBufferPass.pVars->getRootVar();
    var["PerFrameCB"]["gFrameDim"] = mFrameDim;             // 设置 uniform
    for (const auto& ch : kGBufferExtraChannels)
        var[ch.texname] = getOutput(renderData, ch.name);   // 绑定额外 UAV 输出
    mGBufferPass.pState->setFbo(mpFbo);
    mpScene->rasterize(pRenderContext, mGBufferPass.pState.get(),
                       mGBufferPass.pVars.get(), cullMode);
}
```

#### 完整示例：Compute Pass（AccumulatePass）

```cpp
void AccumulatePass::accumulate(RenderContext* pRenderContext,
                                const ref<Texture>& pSrc, const ref<Texture>& pDst)
{
    // ① 绑定资源
    auto var = mpVars->getRootVar();
    var["PerFrameCB"]["gResolution"] = mFrameDim;
    var["PerFrameCB"]["gAccumCount"] = mFrameCount;
    var["gCurFrame"]    = pSrc;       // 输入纹理
    var["gOutputFrame"] = pDst;       // 输出纹理

    // ② 计算 dispatch 尺寸
    uint3 numGroups = div_round_up(uint3(mFrameDim.x, mFrameDim.y, 1u),
                                    pProgram->getReflector()->getThreadGroupSize());

    // ③ 设置程序并 dispatch
    mpState->setProgram(pProgram);
    pRenderContext->dispatch(mpState.get(), mpVars.get(), numGroups);
}
```

#### 完整示例：FullScreenPass（最简绘制）

```cpp
// 构造：创建全屏三角形 VAO（4顶点 triangle strip）
FullScreenPass::FullScreenPass(...) {
    mpState->setVao(mpSharedData->pVao);
    mpState->setDepthStencilState(DepthStencilState::create(/*depth disabled*/));
}

// 执行：一次 draw 调用
void FullScreenPass::execute(RenderContext* pRenderContext, const ref<Fbo>& pFbo, ...) {
    mpState->setFbo(pFbo);
    pRenderContext->draw(mpState.get(), mpVars.get(), 4 /*vertices*/, 0);
}
```

---

### 6.2 Kajiya：RenderPassApi + 原生 Vulkan 指令

#### 调用链概览

```
RenderGraph::record_main_cb(cb)
  → 遍历 passes
    → transition_resource()         // 自动屏障
    → RenderPassApi { cb, resources }  // 构造执行上下文
      → pass.render_fn(&mut api)    // 调用用户闭包
        → api.bind_compute_pipeline() → pipeline.dispatch()     // 计算
        → api.bind_raster_pipeline()  → raw cmd_draw_indexed()  // 光栅（原生Vulkan）
        → api.bind_ray_tracing_pipeline() → pipeline.trace_rays() // 光追
```

#### RenderPassApi（`pass_api.rs`）

```rust
pub struct RenderPassApi<'a, 'exec_params, 'constants> {
    pub cb: &'a CommandBuffer,        // Vulkan 命令缓冲（公开，可直接访问 cb.raw）
    pub resources: &'a mut ResourceRegistry<'exec_params, 'constants>,
}

impl RenderPassApi {
    fn device(&self) -> &Device;                             // 获取设备（可调用 raw Vulkan）
    fn dynamic_constants(&mut self) -> &mut DynamicConstants; // 动态常量分配器
    fn bind_compute_pipeline(...)    -> BoundComputePipeline;     // 绑定计算管线
    fn bind_raster_pipeline(...)     -> BoundRasterPipeline;      // 绑定光栅管线
    fn bind_ray_tracing_pipeline(...) -> BoundRayTracingPipeline; // 绑定光追管线
    fn begin_render_pass(...);                               // 开始 Vulkan render pass
    fn end_render_pass();                                    // 结束 render pass
    fn set_default_view_and_scissor([u32; 2]);              // 设置视口（Y翻转）和裁剪
}
```

#### 管线绑定内部流程

所有管线类型共享 `bind_pipeline_common()`：

```rust
fn bind_pipeline_common(&self, device, pipeline, binding) {
    // 1. 绑定管线（vkCmdBindPipeline）
    device.raw.cmd_bind_pipeline(cb.raw, pipeline.pipeline_bind_point, pipeline.pipeline);

    // 2. 绑定帧常量（描述符集 2）
    //    包含：globals_offset, instance_dynamic_parameters_offset, triangle_lights_offset
    cmd_bind_descriptor_sets(..., set=2, frame_constants_descriptor_set, dynamic_offsets);

    // 3. 分配并写入用户描述符集（描述符集 0, 1 等）
    for each user descriptor set binding:
        allocate vk::DescriptorPool
        allocate vk::DescriptorSet
        write descriptors (sampled image / storage image / storage buffer / acceleration structure)
        cmd_bind_descriptor_sets(...)
}
```

资源绑定类型自动推断（`pass_api.rs`）：

```rust
// Ref<Image, GpuSrv>.bind() → SHADER_READ_ONLY_OPTIMAL → SAMPLED_IMAGE
// Ref<Image, GpuUav>.bind() → GENERAL                   → STORAGE_IMAGE
// Ref<Buffer, _>.bind()     →                            → STORAGE_BUFFER
// DynamicConstants(offset)  →                            → UNIFORM_BUFFER_DYNAMIC
// RayTracingAcceleration    →                            → ACCELERATION_STRUCTURE_KHR
```

#### 计算调度（封装完善）

`BoundComputePipeline` 自动计算 work group 数量：

```rust
impl BoundComputePipeline {
    // threads 是线程总数，自动除以 group_size（向上取整）
    pub fn dispatch(&self, threads: [u32; 3]) {
        let gs = self.pipeline.group_size;
        self.api.device().raw.cmd_dispatch(
            self.api.cb.raw,
            (threads[0] + gs[0] - 1) / gs[0],
            (threads[1] + gs[1] - 1) / gs[1],
            (threads[2] + gs[2] - 1) / gs[2],
        );
    }

    pub fn dispatch_indirect(&self, args_buffer: Ref<Buffer, GpuSrv>, offset: u64) {
        self.api.device().raw.cmd_dispatch_indirect(
            self.api.cb.raw,
            self.api.resources.buffer(args_buffer).raw,
            offset,
        );
    }
}
```

#### 光线追踪（封装完善）

```rust
impl BoundRayTracingPipeline {
    pub fn trace_rays(&self, threads: [u32; 3]) {
        self.api.device().ray_tracing_pipeline_ext.cmd_trace_rays(
            self.api.cb.raw,
            &self.pipeline.sbt.raygen_shader_binding_table,
            &self.pipeline.sbt.miss_shader_binding_table,
            &self.pipeline.sbt.hit_shader_binding_table,
            &self.pipeline.sbt.callable_shader_binding_table,
            threads[0], threads[1], threads[2],
        );
    }
}
```

#### 光栅绘制（原生 Vulkan，未封装）

`BoundRasterPipeline` 只提供 `push_constants()`，**不封装任何 draw 指令**。用户必须直接调用 `ash`（Rust Vulkan 绑定）的原始命令：

```rust
impl BoundRasterPipeline {
    pub fn push_constants(&self, cb: vk::CommandBuffer,
                          stage_flags: vk::ShaderStageFlags,
                          offset: u32, constants: &[u8]);
    // 没有 draw / draw_indexed / draw_indirect 方法
}
```

#### 完整示例：场景网格光栅化（`raster_meshes.rs`）

```rust
pass.render(move |api| {
    // ① 上传 instance 变换到动态常量
    let instance_transforms_offset = api.dynamic_constants()
        .push_from_iter(instances.iter().map(|inst| /* 变换矩阵数据 */));

    // ② 开始 Vulkan render pass（设置 framebuffer、附件、load/store action）
    api.begin_render_pass(
        &render_pass, [width, height],
        &[
            (geometric_normal_ref, ClearValue::default()),
            (gbuffer_ref,          ClearValue::default()),
            (velocity_ref,         ClearValue::default()),
        ],
        Some((depth_ref, ClearValue { depth_stencil: ... })),
    )?;

    // ③ 设置视口和裁剪
    api.set_default_view_and_scissor([width, height]);

    // ④ 绑定管线 + 描述符
    let bound_pipeline = api.bind_raster_pipeline(
        pipeline.into_binding()
            .descriptor_set(0, &[
                RenderPassBinding::DynamicConstantsStorageBuffer(instance_transforms_offset),
            ])
            .raw_descriptor_set(1, bindless_descriptor_set),
    )?;

    // ⑤ 逐 mesh 发出绘制指令 —— 直接调用原始 Vulkan
    unsafe {
        let raw_device = &api.device().raw;
        let cb = api.cb;

        for (draw_idx, instance) in instances.into_iter().enumerate() {
            let mesh = &meshes[instance.mesh.0];

            // 绑定索引缓冲
            raw_device.cmd_bind_index_buffer(
                cb.raw, vertex_buffer.raw,
                mesh.index_buffer_offset, vk::IndexType::UINT32,
            );

            // Push Constants（draw_idx + material 信息）
            bound_pipeline.push_constants(
                cb.raw, vk::ShaderStageFlags::ALL_GRAPHICS,
                0, bytes_of(&[draw_idx as u32, mesh.material_index]),
            );

            // 发出索引绘制指令
            raw_device.cmd_draw_indexed(
                cb.raw,
                mesh.index_count,  // 索引数量
                1,                 // 实例数量
                0, 0, 0,
            );
        }
    }

    api.end_render_pass();
    Ok(())
});
```

#### 完整示例：计算 Pass（使用 SimpleRenderPass）

```rust
// 高层 API：一行链式调用完成 pass 定义 + 资源绑定 + dispatch
SimpleRenderPass::new_compute(
    rg.add_pass("light gbuffer"),
    "/shaders/light_gbuffer.hlsl",
)
.read(&gbuffer_depth.gbuffer)
.read_aspect(&gbuffer_depth.depth, vk::ImageAspectFlags::DEPTH)
.read(&shadow_mask)
.write(&mut output)
.constants((extent_inv, debug_mode))
.dispatch(gbuffer_depth.gbuffer.desc().extent);
// ↑ 内部展开为：
//   pass.render(|api| {
//       patch_const_blobs(api);                               // 上传常量
//       let pipeline = api.bind_compute_pipeline(binding)?;   // 绑定管线
//       pipeline.dispatch(extent);                            // 发出 dispatch
//   });
```

---

### 6.3 Unity：RenderFunc + 类型化 CommandBuffer

#### 调用链概览

```
RenderGraph.Execute()
  → 编译（裁剪、资源分配、屏障）
    → 按拓扑序执行 Pass
      → 从 Pool 获取资源
      → 插入屏障 / 开始 render pass
      → 调用 SetRenderFunc() 注册的 Lambda
        → Lambda 接收类型化 Context（RasterGraphContext / ComputeGraphContext）
          → 通过 ctx.cmd 发出绘制指令
```

#### 三种类型化 Context

| Context | CommandBuffer 类型 | 可执行的操作 |
|---------|-------------------|-------------|
| `RasterGraphContext` | `RasterCommandBuffer` | DrawMesh, DrawRenderer, Blit, SetRenderTarget, ClearRenderTarget 等 |
| `ComputeGraphContext` | `ComputeCommandBuffer` | DispatchCompute, SetComputeBuffer/Texture 等 |
| `UnsafeGraphContext` | `UnsafeCommandBuffer` | 全部操作（逃生舱，兼容旧代码） |

**编译期安全**：`RasterCommandBuffer` 不能调 `DispatchCompute`，`ComputeCommandBuffer` 不能调 `DrawMesh`。

#### 光栅化：RendererList（渲染列表）

Unity 的模型渲染不是手动发出 draw call，而是通过 **RendererList** 批量提交：

```csharp
// ① 创建 RendererList（描述需要绘制哪些对象）
var sortingSettings = new SortingSettings(camera) { criteria = SortingCriteria.CommonOpaque };
var drawingSettings = new DrawingSettings(shaderTagId, sortingSettings) {
    perObjectData = PerObjectData.MotionVectors | PerObjectData.LightProbe,
};
var filteringSettings = new FilteringSettings(RenderQueueRange.opaque, layerMask);

RendererListHandle rendererList = renderGraph.CreateRendererList(
    new RendererListParams(cullingResults, drawingSettings, filteringSettings));

// ② 在 Pass 中使用
using (var builder = renderGraph.AddRasterRenderPass<DrawOpaqueData>("Draw Opaques", out var passData))
{
    passData.rendererList = rendererList;

    builder.UseRendererList(passData.rendererList);   // 声明依赖
    builder.SetRenderAttachment(colorTarget, 0, AccessFlags.Write);
    builder.SetRenderAttachmentDepth(depthTarget, AccessFlags.Write);

    builder.SetRenderFunc((DrawOpaqueData data, RasterGraphContext ctx) => {
        // 绘制所有匹配的渲染器 ← 底层发出批量 DrawIndexed
        ctx.cmd.DrawRendererList(data.rendererList);
    });
}
```

`DrawRendererList()` 内部由 Unity 引擎根据 `cullingResults`、`filteringSettings` 等自动：
- 遍历可见对象
- 排序（前后/材质/距离）
- SRP Batcher 合批
- 发出优化后的 DrawIndexed / DrawIndexedInstanced 调用

#### 全屏效果（Blitter）

```csharp
builder.SetRenderFunc((BlitPassData data, RasterGraphContext ctx) => {
    // Blitter 内部：设置全屏三角形，绑定材质，draw 3 vertices
    Blitter.BlitTexture(ctx.cmd, data.source, scaleBias, data.material, pass: 0);
});
```

#### Compute Dispatch

```csharp
builder.SetRenderFunc((ComputeData data, ComputeGraphContext ctx) => {
    ctx.cmd.SetComputeTextureParam(data.cs, data.kernel, "_Input", data.inputTex);
    ctx.cmd.SetComputeTextureParam(data.cs, data.kernel, "_Output", data.outputTex);
    ctx.cmd.SetComputeFloatParam(data.cs, "_Intensity", data.intensity);
    ctx.cmd.DispatchCompute(data.cs, data.kernel, groupsX, groupsY, 1);
});
```

#### 手动绘制网格

```csharp
builder.SetRenderFunc((MeshPassData data, RasterGraphContext ctx) => {
    // 绑定材质属性
    data.material.SetTexture("_MainTex", data.texture);
    data.material.SetMatrix("_ObjectToWorld", data.transform);

    // 绘制单个 Mesh
    ctx.cmd.DrawMesh(data.mesh, data.transform, data.material, submeshIndex: 0, shaderPass: 0);

    // 或：绘制 mesh 的多个实例
    ctx.cmd.DrawMeshInstanced(data.mesh, submeshIndex: 0, data.material,
                               shaderPass: 0, data.matrices);

    // 或：GPU 间接绘制
    ctx.cmd.DrawMeshInstancedIndirect(data.mesh, submeshIndex: 0, data.material,
                                       shaderPass: 0, data.argsBuffer);
});
```

#### 完整示例：URP 不透明物体渲染

```csharp
public class DrawOpaquePass : ScriptableRenderPass
{
    class PassData {
        public RendererListHandle rendererList;
    }

    public override void RecordRenderGraph(RenderGraph renderGraph, ContextContainer frameData)
    {
        var resourceData = frameData.Get<UniversalResourceData>();
        var cameraData = frameData.Get<UniversalCameraData>();

        using (var builder = renderGraph.AddRasterRenderPass<PassData>("Draw Opaques", out var data))
        {
            // ① 创建渲染列表
            var desc = new RendererListDesc(shaderTagIds, cameraData.cullingResults, cameraData.camera) {
                renderQueueRange = RenderQueueRange.opaque,
                sortingCriteria = SortingCriteria.CommonOpaque,
            };
            data.rendererList = renderGraph.CreateRendererList(desc);

            // ② 声明资源使用
            builder.UseRendererList(data.rendererList);
            builder.SetRenderAttachment(resourceData.activeColorTexture, 0, AccessFlags.ReadWrite);
            builder.SetRenderAttachmentDepth(resourceData.activeDepthTexture, AccessFlags.Write);

            // ③ 设置执行函数
            builder.SetRenderFunc((PassData d, RasterGraphContext ctx) => {
                ctx.cmd.DrawRendererList(d.rendererList);   // 一次调用绘制所有不透明物体
            });
        }
    }
}
```

---

### 6.4 Unreal Engine：Lambda + FRHICommandList

#### 调用链概览

```
FRDGBuilder::Execute()
  → Compile()（依赖分析、裁剪、屏障计算）
  → 遍历 Pass 列表
    → 插入计算好的屏障（RHICmdList.Transition）
    → 调用 Pass Lambda
      → Lambda 接收 FRHICommandList&
        → 通过 RHICmdList 发出绘制指令
```

#### FRHICommandList 绘制 API

```cpp
class FRHICommandList {
public:
    // ── 状态设置 ──
    void SetGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Init, ...);
    void SetBoundShaderState(FRHIBoundShaderState* BoundShaderState);
    void SetViewport(float MinX, float MinY, float MinZ,
                     float MaxX, float MaxY, float MaxZ);
    void SetScissorRect(bool bEnable, uint32 MinX, uint32 MinY, uint32 MaxX, uint32 MaxY);
    void SetStreamSource(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset);
    void SetStencilRef(uint32 StencilRef);
    void SetBlendFactor(const FLinearColor& BlendFactor);

    // ── 基础绘制 ──
    void DrawPrimitive(uint32 BaseVertexIndex, uint32 NumPrimitives, uint32 NumInstances);
    void DrawIndexedPrimitive(FRHIBuffer* IndexBuffer, int32 BaseVertexIndex,
                              uint32 FirstInstance, uint32 NumVertices,
                              uint32 StartIndex, uint32 NumPrimitives, uint32 NumInstances);

    // ── 间接绘制 ──
    void DrawPrimitiveIndirect(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset);
    void DrawIndexedIndirect(FRHIBuffer* IndexBuffer, FRHIBuffer* ArgumentBuffer,
                             int32 DrawArgumentsIndex, uint32 NumInstances);

    // ── 计算着色器 ──
    void DispatchComputeShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY,
                               uint32 ThreadGroupCountZ);
    void DispatchIndirectComputeShader(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset);

    // ── Mesh Shader ──
    void DispatchMeshShader(uint32 ThreadGroupCountX, uint32 ThreadGroupCountY,
                            uint32 ThreadGroupCountZ);
    void DispatchIndirectMeshShader(FRHIBuffer* ArgumentBuffer, uint32 ArgumentOffset);

    // ── 资源转换（自动由 RDG 处理，通常不需手动调用）──
    void Transition(TArrayView<const FRHITransitionInfo> Infos);

    // ── Render Pass 管理 ──
    void BeginRenderPass(const FRHIRenderPassInfo& InInfo, const TCHAR* Name);
    void EndRenderPass();
};
```

#### Mesh Draw Command 系统

Unreal 不是在 RDG Pass 内手动发出逐 mesh draw call，而是使用 **Mesh Draw Command** 批处理系统：

```
FMeshPassProcessor                  ← 决定哪些 mesh 参与哪个 pass
  → 生成 FMeshDrawCommand 列表     ← 缓存的绘制命令（PSO + 资源绑定 + 几何体）
    → FMeshDrawCommand::SubmitDraw() ← 在 RDG Pass Lambda 内批量提交

FMeshDrawCommand 包含:
  ├─ CachedPipelineId              // 缓存的 PSO
  ├─ ShaderBindings                // 所有着色器资源绑定
  ├─ VertexStreams[MaxStreams]      // 顶点缓冲绑定
  ├─ IndexBuffer                   // 索引缓冲
  ├─ FirstIndex / NumPrimitives / NumInstances  // 几何体参数
  └─ IndirectArgs                  // 间接绘制参数（Nanite 使用）
```

**RDG 与 MeshDrawCommand 的关系**：

```cpp
// RDG Pass 内部
GraphBuilder.AddPass(
    RDG_EVENT_NAME("BasePass"),
    PassParameters,
    ERDGPassFlags::Raster,
    [&View, PassParameters](FRHICommandList& RHICmdList) {
        // 设置 render pass
        // ...

        // 提交所有缓存的 Mesh Draw Commands
        View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(
            nullptr, RHICmdList, &MeshPassProcessorRenderState);

        // DispatchDraw 内部对每个 FMeshDrawCommand:
        //   1. SetGraphicsPipelineState() — 绑定 PSO
        //   2. SetShaderParameters()      — 绑定着色器参数
        //   3. SetStreamSource()          — 绑定顶点缓冲
        //   4. DrawIndexedPrimitive()     — 发出绘制指令
    }
);
```

#### Compute Pass 完整示例

```cpp
// 使用便利函数 FComputeShaderUtils::AddPass
TShaderMapRef<FMyComputeShader> ComputeShader(GetGlobalShaderMap(FeatureLevel));
FMyComputeShader::FParameters* Params = GraphBuilder.AllocParameters<FMyComputeShader::FParameters>();
Params->InputTexture = InputTexture;
Params->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
Params->Width = Width;

FComputeShaderUtils::AddPass(
    GraphBuilder,
    RDG_EVENT_NAME("MyCompute"),
    ComputeShader,
    Params,
    FComputeShaderUtils::GetGroupCount(FIntPoint(Width, Height), FIntPoint(8, 8))
);

// 内部展开为:
GraphBuilder.AddPass(Name, Params, ERDGPassFlags::Compute,
    [Params, ComputeShader, GroupCount](FRHICommandList& RHICmdList) {
        FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *Params, GroupCount);
        // → SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader())
        // → SetShaderParameters(RHICmdList, ComputeShader, Params)
        // → RHICmdList.DispatchComputeShader(X, Y, Z)
    }
);
```

#### 全屏绘制（FPixelShaderUtils）

```cpp
FPixelShaderUtils::AddFullscreenPass(
    GraphBuilder,
    ShaderMap,
    RDG_EVENT_NAME("PostProcess"),
    PixelShader,
    PassParameters,
    FIntRect(0, 0, Width, Height)
);
// 内部：
// → BeginRenderPass()
// → SetGraphicsPipelineState()     — 全屏三角形 VS + 用户 PS
// → SetShaderParameters()
// → DrawPrimitive(0, 1, 1)        — 绘制一个全屏三角形
// → EndRenderPass()
```

#### Nanite 的特殊模式

Nanite 使用 **GPU-driven rendering**，完全不在 CPU 发 draw call：

```cpp
// Nanite 在 RDG 中的模式：
GraphBuilder.AddPass(
    RDG_EVENT_NAME("Nanite::Rasterize"),
    PassParameters,
    ERDGPassFlags::Compute,   // 注意：Nanite 的光栅化是 Compute Pass！
    [](FRHICommandList& RHICmdList) {
        // GPU 上：compute shader 执行软件/硬件光栅化
        // 所有裁剪、LOD 选择、draw dispatch 都在 GPU 完成
        RHICmdList.DispatchIndirectComputeShader(IndirectArgsBuffer, 0);
        // 或使用 Mesh Shader：
        RHICmdList.DispatchIndirectMeshShader(IndirectArgsBuffer, 0);
    }
);
```

---

### 6.5 四框架绘制指令方式对比

| 方面 | Falcor | Kajiya | Unity | Unreal |
|------|--------|--------|-------|--------|
| **绘制指令层** | `RenderContext` 方法 | 原生 `ash` Vulkan | 类型化 `CommandBuffer` | `FRHICommandList` |
| **状态管理** | `GraphicsState` + `ProgramVars` | 管线绑定后返回 Bound 对象 | 材质系统 + `CommandBuffer` | `FGraphicsPipelineState` + Shader 参数 |
| **场景绘制** | `Scene::rasterize()` — Indirect Draw | 手动遍历 mesh + `cmd_draw_indexed` | `DrawRendererList()` — 引擎自动合批 | `MeshDrawCommand` 批处理系统 |
| **Compute dispatch** | `pRenderContext->dispatch(state, vars, groups)` | `BoundComputePipeline::dispatch(threads)` | `ctx.cmd.DispatchCompute(cs, kernel, x, y, z)` | `RHICmdList.DispatchComputeShader(x, y, z)` |
| **资源绑定** | `ShaderVar` 下标语法 `var["name"] = res` | `Ref<T>.bind()` 组装描述符集 | `cmd.SetComputeTextureParam(cs, k, n, t)` | `SHADER_PARAMETER` 宏 + 自动绑定 |
| **全屏 Pass** | `FullScreenPass::execute()` | `SimpleRenderPass + dispatch` | `Blitter.BlitTexture()` | `FPixelShaderUtils::AddFullscreenPass()` |
| **GPU-driven** | `drawIndexedIndirect` | `cmd_draw_indexed` 循环 | `DrawMeshInstancedIndirect` | Nanite: `DispatchIndirectMeshShader` |
| **抽象程度** | 中（封装 GFX API） | 低（光栅直接调原生 Vulkan） | 高（RendererList 自动合批） | 高（MeshDrawCommand 缓存 + 自动提交） |

---

## 七、横向对比

### 6.1 总览

| 特性 | Falcor | Kajiya | Unity | Unreal |
|------|--------|--------|-------|--------|
| **语言** | C++ + Python | Rust | C# | C++ |
| **图形 API** | D3D12 / Vulkan | Vulkan | 跨平台 | D3D11/D3D12/Vulkan/Metal |
| **依赖声明方式** | `reflect()` 虚方法 | Builder + 类型标记 | Builder + AccessFlags | 参数结构体宏反射 |
| **Pass 裁剪** | ✅ 拓扑排序 + 反向 DFS | ❌ 按顺序执行 | ✅ 反向遍历输出 | ✅ 从根 Pass 反向 |
| **自动屏障** | ❌ 底层处理 | ✅ vk_sync | ✅ 自动 | ✅ + Split Barrier |
| **资源池化** | ResourceCache | TransientResourceCache | RTHandle 池 | FRenderTargetPool / TransientAllocator |
| **内存别名** | 逻辑层资源别名 | Transient cache 复用 | 生命周期不重叠共享 | 物理内存别名（placed resources） |
| **异步计算** | ❌ | ❌ | 计划中 | ✅ 完整支持 + 自动 fence |
| **Temporal 资源** | ❌ | ✅ 一等公民 | 手动 Import/Export | 手动 Register/Extract |
| **可视化编辑** | ✅ ImGui 节点编辑器 | ❌ | ✅ Render Graph Viewer | ✅ Graphviz 导出 + Insights |
| **移动端优化** | ❌ | ❌ | ✅ Native Render Pass 合并 | ✅ Pass Merging |
| **类型安全** | 运行时验证 | **编译期**（Rust 类型系统） | 编译期（类型化 Context） | 运行时验证 |
| **脚本化** | ✅ Python | ❌ | ❌ | ❌ |
| **调试工具** | 图编辑器 + 日志 | Debug hook | Render Graph Viewer | ImmediateMode + Insights + Graphviz |

### 6.2 依赖声明方式对比

**Falcor — 反射虚方法**：

```cpp
RenderPassReflection reflect(const CompileData& data) override {
    RenderPassReflection r;
    r.addInput("src", "").texture2D();
    r.addOutput("dst", "").texture2D().format(ResourceFormat::RGBA16Float);
    return r;
}
```

**Kajiya — Rust Builder + 类型标记**：

```rust
let input = pass.read(&tex, AccessType::ComputeShaderReadSampledImage);  // → Ref<Image, GpuSrv>
let output = pass.write(&mut tex, AccessType::ComputeShaderWrite);       // → Ref<Image, GpuUav>
```

**Unity — Builder + AccessFlags**：

```csharp
builder.UseTexture(source, AccessFlags.Read);
builder.SetRenderAttachment(dest, 0, AccessFlags.Write);
```

**Unreal — 参数结构体宏反射**：

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FParams, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Source)           // 隐式声明 SRV 读
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Dest) // 隐式声明 UAV 写
END_SHADER_PARAMETER_STRUCT()
```

### 6.3 设计哲学对比

| 引擎 | 设计哲学 | 适用场景 |
|------|---------|---------|
| **Falcor** | 研究友好、灵活组合、可视化编辑、脚本化 | 学术研究、算法原型、渲染实验 |
| **Kajiya** | Rust 类型安全、零开销抽象、Temporal 资源 | 实验性实时全局光照 |
| **Unity** | 渐进演进、兼容性优先、移动端优化 | 跨平台游戏、移动端渲染 |
| **Unreal** | 工业级完整性、声明式宏系统、异步计算 | AAA 游戏、大规模渲染系统 |

---

## 八、参考资料

### 基础理论

- [FrameGraph: Extensible Rendering Architecture in Frostbite (GDC 2017)](https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in) — Yuriy O'Donnell，奠基性演讲
- [Halcyon Architecture (GDC 2019)](https://www.gdcvault.com/play/1025896/Halcyon-Architecture) — Graham Wihlidal，Frostbite/SEED 后续演讲
- [Render Graphs and Vulkan — A Deep Dive](http://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/) — Hans-Kristian Arntzen，Vulkan render graph 实现详解
- [Organizing GPU Work with Directed Acyclic Graphs](https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3) — Pavlo Muratov，实践文章
- [Render Graphs (Blog Series)](https://logins.github.io/graphics/2021/05/31/RenderGraphs.html) — Dustin Land，多篇实现教程

### Falcor

- [NVIDIAGameWorks/Falcor (GitHub)](https://github.com/NVIDIAGameWorks/Falcor)
- [Falcor Render Graph 文档](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/usage/render-graph.md)
- [Falcor 入门文档](https://github.com/NVIDIAGameWorks/Falcor/blob/master/docs/getting-started.md)
- [NVIDIA Developer — Falcor](https://developer.nvidia.com/falcor)

### Kajiya

- [EmbarkStudios/kajiya (GitHub)](https://github.com/EmbarkStudios/kajiya)
- [Kajiya 架构文档](https://github.com/EmbarkStudios/kajiya/blob/main/docs/architecture.md)
- [Tomasz Stachowiak (@h3r2tic)](https://github.com/h3r2tic) — Kajiya 作者

### Unity

- [Unity 6 Render Graph 文档](https://docs.unity3d.com/6000.0/Documentation/Manual/render-graph.html)
- [URP Render Graph 文档](https://docs.unity3d.com/Packages/com.unity.render-pipelines.universal@17.0/manual/render-graph.html)
- [SRP Core Render Graph System](https://docs.unity3d.com/Packages/com.unity.render-pipelines.core@17.0/manual/render-graph-system.html)
- [Unity-Technologies/Graphics (GitHub)](https://github.com/Unity-Technologies/Graphics) — SRP Core 源码
- [Render Graph 源码目录](https://github.com/Unity-Technologies/Graphics/tree/master/Packages/com.unity.render-pipelines.core/Runtime/RenderGraph)

### Unreal Engine

- [Render Dependency Graph (官方文档)](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine)
- [RDG Programmer's Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/rdg-programmers-guide-for-unreal-engine)
- [EpicGames/UnrealEngine (GitHub, 需权限)](https://github.com/EpicGames/UnrealEngine) — RDG 源码在 `Engine/Source/Runtime/RenderCore/`
- [获取 UE GitHub 访问权限](https://www.unrealengine.com/en-US/ue-on-github)
