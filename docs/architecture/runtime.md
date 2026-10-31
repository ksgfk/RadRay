> - 适用: 创建 runtime 环境与设备，组织 Scene 绘制输入，扩展 MeshPassProcessor 并构建网格绘制命令
> - 权威: 本文维护 runtime 当前接口边界与所有权；底层资源契约见 RHI 文档
> - 锚点: `modules/runtime/include/radray/runtime/`, `modules/runtime/src/`, `modules/runtime/CMakeLists.txt`, `modules/core/include/radray/sparse_set.h`

# Rendering runtime

`RadRay::runtime` 是 C++20 静态库，实际 target 为 `radrayruntime`，公共依赖 `RadRay::render`，
并通过它依赖 core。公共类型位于 `namespace radray`，RHI 类型位于 `radray::render`。
构建开关与包消费见[构建与测试](../guide/build-test.md#runtime-构建)。

当前模块包含 `gpu_env.h/.cpp` 中的 `Environment`、`gpu_device.h/.cpp` 中的 `DeviceContext`，
以及 Scene 的 CPU 绘制输入数据、MeshDrawCommand、Processor 接口与命令输出 Context。
候选遍历、Element 命令构建、具体 Pass 的筛选、shader 参数、PSO 选择与 RHI 录制由调用方实现。
旧 Renderer、可编程管线、Render Graph、几何/临时资源池和 PSO 缓存仍未恢复；
删除前的设计、源码基线和旧测试入口见[设计归档](../history/2026-10-31-runtime-design-archive.md)，不作为当前接口契约。

## Scene 绘制输入

| 类型 | 文件对 | 当前内容 |
|---|---|---|
| `Scene` | `scene.h/.cpp` | `Primitives`、`Meshes`、`Materials` 三个 SparseSet |
| `PrimitiveSceneInfo` | `primitive_scene_info.h/.cpp` | 本地到世界变换、`Aabb LocalBounds`、MeshBatch 句柄列表 |
| `MeshBatch` | `mesh_batch.h/.cpp` | 一个材质句柄、一个顶点输入引用、拓扑和内嵌的 Elements |
| `MeshBatchElement` | `mesh_batch_element.h/.cpp` | 借用的索引缓冲与绘制范围，固定单实例 |
| `VertexInput` | `vertex_input.h/.cpp` | 顶点布局、属性和借用的顶点缓冲流 |
| `MeshSection` | `mesh_section.h/.cpp` | 几何资源的 submesh 分段：材质槽和索引范围；Scene 不存储它 |
| `MaterialRenderProxy` | `material_render_proxy.h/.cpp` | 材质身份占位；shader、参数及实际材质资源接口尚未定义 |

这些类型参考 UE 传统 Mesh Drawing Pipeline 的分工，当前是可修改的基础数据版本，
不要求与 UE 的类层次或生命周期完全一致。Scene 不存储用户侧网格和材质资产。
Scene 直接持有 `SparseSet<PrimitiveSceneInfo>`、`SparseSet<MeshBatch>` 和 `SparseSet<MaterialRenderProxy>`，
通过公开成员使用容器的增删查与遍历接口。它不创建或依赖 GPU Device。

### 关联与存储

- `PrimitiveSceneInfo::Meshes` 中的句柄在同一 Scene 的 `Meshes` 中解析，表达一个 primitive 的多个绘制 Batch。
  这一版约定每个已关联 Batch 属于一个 primitive；复用几何通过共享 VertexInput 和缓冲实现。
- `MeshBatch::Material` 在同一 Scene 的 `Materials` 中解析；多个 Batch 可以引用同一个材质记录。
  一个 Batch 的所有 Elements 共用这个材质、顶点输入和拓扑，Element 本身没有材质槽。
- `MeshBatch::Elements` 直接按值存储 `MeshBatchElement`，不另建 SparseSet；这些片段没有稳定独立身份。
- `MeshBatch::VertexInput` 借用外部顶点输入描述，可被多个 Batch 共享。调用方必须维持其地址稳定，
  不可引用会因容器扩容、搬移或销毁而失效的对象。

句柄沿用 `SparseSetHandle`，仅在所属容器内解释，当前没有类型或 Scene 身份校验。
扩容与删除可能移动记录，不能把元素指针或稠密数组下标作为长期身份；
详细容器契约见 [Core / SparseSet](core-facilities.md#sparseset)。
Scene 暂不维护反向引用、级联删除或引用计数；调用方在删除 Batch/Material 前移除引用，
删除 primitive 时自行处理其 Batch，并在访问前用所属容器的 `IsAlive` 检查句柄。

### 几何输入与 submesh

`MeshSection` 表示某个几何资源或 LOD 中的一段索引范围，`MaterialSlot` 是该资源的材质槽编号，
不是 Scene 材质句柄。上层用槽位查出材质句柄，填入 Batch 的 `Material`，
再将 `FirstIndex`、`IndexCount`、`VertexOffset` 填入 Element。Scene 不要求保留原始 Section。
普通双材质网格可生成两个各含一个 Element 的 Batch；Section 与 Batch 不建立强制一一对应关系。

当前只表达直接索引单实例绘制：`IndexCount` 是索引数量，三角形列表的一个三角形对应三个索引。
`IndexBufferOffset` 是字节偏移，`IndexStride` 为 2 或 4；`FirstIndex` 相对此偏移，单位是索引。
`VertexOffset` 是有符号的基础顶点偏移。Element 与 DrawCommand 均固定从实例 0 绘制一个实例，
不提供实例范围字段或逐实例变换表。实例绘制、非索引绘制、间接绘制和 LOD 选择尚未实现。

`VertexInput` 命名对应其顶点输入描述职责，当前只表达普通顶点缓冲输入；
UE VertexFactory 的 shader permutation、蒙皮或自定义取数机制不在当前实现范围内。
`Layouts` 使用 RHI 的 `VertexBufferLayout`，通过 Binding 对应 `Streams`；
`Attributes` 使用拥有 `Semantic` 字符串的 `VertexInputAttribute`，避免长期保存 RHI `VertexAttribute` 的临时 string_view。
属性的 `BufferBinding` 必须对应布局和已绑定的流，Offset/Size 使用字节单位。

所有 GPU Buffer 引用均为借用，不因记录销毁而释放。外部资源所有者负责保持 Buffer 有效，
直到 CPU 不再使用相关描述且 GPU 已完成所有引用这些资源的工作。
默认构造只得到未填充记录：材质句柄无效、VertexInput 和 Buffer 引用为空、索引数量为零、Primitive 包围盒为空。
用于实际绘制前，调用方必须补全引用、有效布局、缓冲范围及包围盒。
Scene 与 Processor 基类不自动验证输入；句柄、布局及 GPU 资源检查、录制和提交由调用方负责。

`PrimitiveSceneInfo::LocalBounds` 使用 core 的 `Aabb`，端点位于物体本地空间；
可用 `LocalBounds.Transform(LocalToWorld)` 得到世界空间 AABB，Scene 不自动维护其副本。
空盒、包含/相交和扩展操作的契约见 [Core 数学](core-facilities.md#数学)。

例如，已由外部准备好 `VertexInput cubeVertices` 和 `render::Buffer& cubeIndices` 后，
可用下面的数据组织一个顶面金属、其余五面木头的立方体。假定索引缓冲按木头 30 个索引、金属 6 个索引排列，
索引宽度为 4 字节，立方体本地坐标范围为 [-0.5, 0.5]。此处材质仍只是身份占位：

```cpp
#include <radray/runtime/scene.h>
#include <radray/runtime/mesh_section.h>

// scene must stop referencing cubeVertices and cubeIndices before they are destroyed.
void AddCube(Scene& scene, const VertexInput& cubeVertices, render::Buffer& cubeIndices) {
    const array<SparseSetHandle, 2> materials{
        scene.Materials.Emplace(),  // wood
        scene.Materials.Emplace()   // metal
    };
    const array<MeshSection, 2> sections{
        MeshSection{.MaterialSlot = 0, .FirstIndex = 0, .IndexCount = 30},
        MeshSection{.MaterialSlot = 1, .FirstIndex = 30, .IndexCount = 6}
    };

    const auto primitiveHandle = scene.Primitives.Emplace();
    auto& primitive = scene.Primitives.Get(primitiveHandle);
    primitive.LocalBounds = Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    for (const auto& section : sections) {
        MeshBatch batch;
        batch.Material = materials[section.MaterialSlot];
        batch.VertexInput = &cubeVertices;
        batch.Elements.push_back(MeshBatchElement{
            .IndexBuffer = &cubeIndices,
            .FirstIndex = section.FirstIndex,
            .IndexCount = section.IndexCount,
            .VertexOffset = section.VertexOffset});
        primitive.Meshes.push_back(scene.Meshes.Emplace(std::move(batch)));
    }
}
```

示例在 `namespace radray` 内使用。材质/Pass 筛选策略、可见性、缓存失效与 GPU 资源退役，
均不由上述数据容器隐式完成。

## Mesh Drawing Pipeline 基础接口

| 文件对 | 接口与职责 |
|---|---|
| `mesh_draw_command.h/.cpp` | `MeshDrawCommand` 纯数据结构：PSO、几何绑定和绘制范围 |
| `mesh_pass_processor.h/.cpp` | `MeshBatchContext`、`MeshPassProcessor`，单个 Batch 的处理接口与输出 Context 引用 |
| `mesh_pass_draw_list_context.h/.cpp` | `MeshPassDrawListContext` 输出接口、`DynamicMeshPassDrawListContext` 本次命令存储 |

调用关系为：

```text
调用方组织候选 Batch，解析句柄并提供所属 Primitive
  processor.AddMeshBatch(context)             void，具体 Pass 策略
    筛选 Batch，选择 PSO
    调用方实现 Element 命令构建
      DrawListContext.AddCommand(initializer)  返回输出存储中的命令引用
      DrawListContext.FinalizeCommand(...)     填入该 Element 的绘制范围
```

调用方派生 `MeshPassProcessor`，构造时传入输出 Context，实现返回 void 的 `AddMeshBatch`。
Pass 名称和教学日志由样例自行组织，不属于 Processor 基类接口。
Context 必须晚于 Processor 销毁。相机、材质参数表与 Pass 策略
由具体 Processor 持有或借用，不放进通用接口。`MeshBatchContext` 携带本次 Primitive/Batch 的 const
引用及句柄，仅在该次调用期间有效；构建期间不得修改输入记录或使这些引用失效。
候选收集、Primitive/Batch 遍历及句柄解析由调用方负责，runtime 不接收 Scene 或候选列表，
也不做可见性、LOD 或 Pass 分组。调用方为每个候选 Batch 调用 `AddMeshBatch`，
具体 Processor 自行组织 Element 选择与命令构建。Processor 头文件只依赖所需记录类型，不包含 Scene 容器。

`AddMeshBatch` 返回 void，通过 Context 输出零条或多条命令；跳过 Batch 时直接返回。
runtime 不提供 Batch/Element 构建 helper，也不定义统一错误返回或回滚策略。
调用方负责提供与 Primitive/Batch/Material 句柄匹配的有效记录、检查输入并填充命令。

`MeshPassDrawListContext::AddCommand` 将共享的 PSO/顶点绑定描述复制到 Context 自己的输出存储，
返回供本次填充的命令引用；`FinalizeCommand` 默认填入指定 Element 的索引绑定及绘制范围。
派生 Context 可在调用基类实现后处理自己的额外输出。它收到的 Batch 上下文引用仅在调用期间有效。
调用方必须在下次 Add 前完成本条命令的 Finalize；返回的命令引用不保证在下一次 Add 或容器修改后有效。

`DynamicMeshPassDrawListContext` 使用拥有命令值的 `Commands` 列表，无缓存、去重或跨帧失效管理。
Processor 生成的命令始终追加到其 Context。
调用方每次渲染新建 Context，或在不再使用旧输出后自行清空。成功生成零条命令也是合法结果。

`MeshDrawCommand` 拥有顶点/索引绑定描述及绘制范围，不保存 Scene、Primitive、MeshBatch 或
VertexInput 的引用，也不保存 shader 参数。构建完成后修改这些 CPU 几何描述不会改变已生成的命令。
PSO 与 GPU Buffer 仍为借用，调用方必须维持它们有效，直至所有命令录制和 GPU 使用结束，并遵守
[RHI command epoch 与资源所有权](render-rhi.md)契约。当前没有静态命令缓存或失效管理。

shader 参数的存储、更新、校验和绑定暂由调用方组织；runtime 不提供 push constant 或
`ShaderParameterSet` 的逐 Draw 绑定契约，也没有完整材质系统。
实际 shader/PSO 必须与 Batch 的拓扑、顶点布局及参数声明匹配，这由调用方保证。
runtime 不检查布局/流对应关系、PSO/Buffer 有效性、Vertex/Index usage、缓冲范围、
索引宽度与偏移，也不读取索引内容或反射 shader。调用方按上述单实例约定录制 RHI 绘制。

runtime 不提供绘制命令校验或录制接口。调用方读取 `MeshDrawCommand`，自行检查 GPU 资源和绘制范围，
使用 RHI 绑定 PSO、几何缓冲、shader 参数并绘制。viewport/scissor、Pass 开闭、附件、barrier、
队列提交、等待和资源销毁均由调用方控制。

[教学样例](../guide/build-test.md#mesh-drawing-pipeline-教学样例)在 `sample.cpp` 内用私有的 `BuildMeshDrawCommandList`
遍历候选 Primitive/Batch，检查 Scene 句柄，失效项记录错误并跳过。
两个派生 Processor 筛选后调用 `pipeline.cpp` 内私有的 `BuildMeshDrawCommands`，
检查顶点布局/流、准备 PSO/顶点绑定，并展开全部非空 Element；无效 Batch 不生成命令，已有输出保留。
由样例自己的 `SampleDrawListContext` 保存 `PreparedDraw`、准备参数并输出教学日志，
随后自行用 RHI 录制带参数的 Draw。它的参数错误标志只属于样例，不是 runtime 的统一构建状态。
保留中文步骤注释和 UE 5.8.4 对照。输出 Context 的 CPU 契约测试位于 `modules/runtime/tests/test_mesh_drawing_pipeline.cpp`，
测试替身和观测数据全部由测试自身持有。

## 环境

`Environment::Create(backend, validation, appName)` 创建所选后端的 DXGI factory 或 Vulkan instance，
不创建 GPU device。`appName` 传入 Vulkan instance 描述，默认值为 `RadRay`。
validation 开启时配置 D3D12 诊断上下文和 debug layer，或 Vulkan debug layer 与 synchronization validation。

Environment 拥有 D3D12 factory，或负责 Vulkan environment 的初始化与关闭；记录创建线程、backend 和 validation 设置。
`IsOwnerThread` 提供线程身份查询，不提供自动调度或并发保护。后端可通过
`RADRAY_ENABLE_D3D12` / `RADRAY_ENABLE_VULKAN` 禁用；请求未启用的后端会失败并记录日志。

## 设备与队列

`DeviceContext::Create(environment, host)` 借用环境，优先选择高性能适配器，创建 GPU Device 和一条 Direct 队列，
启用外部资源扩展。提供可选 `ExternalDeviceIdentity` 时尝试匹配宿主设备；没有匹配项则记录警告并使用优选设备，
不保证返回的设备身份一定等于 host。底层互操作契约见 [RHI 与后端](render-rhi.md)。

DeviceContext 也提供接收 `shared_ptr<render::Device>` 与非空 Direct 队列指针的构造函数。
调用方须保证队列属于传入设备，且底层环境寿命满足设备要求。
DeviceContext 持有共享 Device owner，队列指针从设备借用；`GetDevice` / `GetQueue` 返回借用引用。
`SupportsIndirect` 实时读取 RHI 的 `IndirectDraw` 和 `IndirectFirstInstance` 能力，不依赖后端内部头。

## 生命周期与失败

Environment 必须晚于 DeviceContext 及所有依赖它的 GPU 资源销毁。应用应先完成 GPU 工作、释放资源，
再销毁 DeviceContext，最后销毁 Environment。DeviceContext 析构会等待其 Direct 队列，然后释放 Device owner。
如果外部仍持有同一 Device 的共享引用，Environment 也必须继续存活。

调用方在环境的创建线程串行组织生命周期，RHI 的 CPU 调用遵守其串行化契约。模块不提供帧槽、
自动资源退役或帧提交循环；应用直接使用 RHI 录制绘制命令，并安排 Pass、资源同步和队列提交。

可失败工厂返回 `Nullable<unique_ptr<T>>`，失败处记录日志，调用方检查返回值；不通过接口传递错误字符串。
Vulkan instance 和 D3D12 factory 的底层环境约束仍由 RHI 管理。
