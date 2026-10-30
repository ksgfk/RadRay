> - 适用: 创建 runtime 环境与设备，组织材质与 Scene 绘制输入，扩展 MeshPassProcessor 并构建网格绘制命令
> - 权威: 本文维护 runtime 当前接口边界与所有权；底层资源契约见 RHI 文档
> - 锚点: `modules/runtime/include/radray/runtime/`, `modules/runtime/src/`, `modules/runtime/CMakeLists.txt`, `modules/core/include/radray/sparse_set.h`

# Rendering runtime

`RadRay::runtime` 是 C++20 静态库，实际 target 为 `radrayruntime`，公共依赖 `RadRay::render`，
并通过它依赖 core。公共类型位于 `namespace radray`，RHI 类型位于 `radray::render`。
构建开关与包消费见[构建与测试](../guide/build-test.md#runtime-构建)。

当前模块包含 `gpu_env.h/.cpp` 中的 `Environment`、`gpu_device.h/.cpp` 中的 `DeviceContext`，
以及材质定义、实例参数、渲染快照，Scene 的 CPU 绘制输入数据、MeshDrawCommand、Processor 接口与命令输出 Context。
候选遍历、Element 命令构建、具体 Pass 的筛选、shader 参数的 GPU 布局与绑定、PSO 选择与 RHI 录制由调用方实现。
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
| `MaterialRenderProxy` | `material_render_proxy.h/.cpp` | 借用 MaterialResource，拥有已解析的 CPU 参数快照；见下文材质核心 |

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
索引宽度为 4 字节，立方体本地坐标范围为 [-0.5, 0.5]。木头和金属实例由调用方准备：

```cpp
#include <radray/runtime/scene.h>
#include <radray/runtime/mesh_section.h>

// scene must stop referencing cubeVertices and cubeIndices before they are destroyed.
void AddCube(Scene& scene, const VertexInput& cubeVertices, render::Buffer& cubeIndices,
             const MaterialInstance& wood, const MaterialInstance& metal) {
    const array<SparseSetHandle, 2> materials{
        scene.Materials.Emplace(),  // wood
        scene.Materials.Emplace()   // metal
    };
    scene.Materials.Get(materials[0]).Update(wood);
    scene.Materials.Get(materials[1]).Update(metal);
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

## 材质核心

材质描述表面使用的 shader、固定性质和可变参数；具体 Pass 结合这些信息、几何与本次渲染状态决定如何绘制。
runtime 当前保留四个角色，每个类型有同名 `.h/.cpp`：

| 类型 | 职责 | UE 5.8.4 对照（职责近似，不是完整复刻） |
|---|---|---|
| `MaterialResource` | 材质性质，以及已加载 shader/布局的具名程序列表 | `FMaterial` / `FMaterialResource` 的渲染性质与 shader 资源职责 |
| `Material` | 借用一份 Resource，拥有参数名称、类型、顺序和默认值 | `UMaterial` 的用户侧定义职责；本例没有材质图 |
| `MaterialInstance` | 借用一个 Material，按声明索引保存可选参数覆盖；未覆盖时使用默认值 | `UMaterialInstance` / `UMaterialInstanceDynamic` 的动态参数职责 |
| `MaterialRenderProxy` | 借用 Resource，拥有同步时解析后的完整参数副本 | `FMaterialRenderProxy` / `FMaterialInstanceRenderProxy` 的渲染侧访问职责 |

```text
应用持有 MaterialResource（性质、可用 shader 程序）
          ↑ 借用
应用持有 Material（参数声明与默认值）
          ↑ 借用
应用持有 MaterialInstance（参数覆盖）
          │ 显式 Proxy.Update(instance)，解析并复制参数
Scene.Materials → MaterialRenderProxy（借用 Resource + 自有参数快照）
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
Update 与读取由调用方串行组织，当前没有渲染线程、变更通知、脏标记或自动上传。

Resource 及其借用的 Shader、PipelineLayout、TextureView、Sampler 仍由外部持有。
Resource 必须在被 Material/Proxy/样例 PSO 表引用期间地址稳定且内容不变；具名程序名称须唯一。
`FindProgram` / `GetProgram` 只做查找，不校验 shader 阶段、布局兼容性或 GPU 对象有效性。
资源参数仅复制指针，不增加引用计数；外部所有者须维持资源有效直至相关 CPU/GPU 使用结束。

### 材质与 Pass 的边界

`MaterialProperties` 包含 `MaterialTagSet Tags` 和 `MaterialRenderState RenderState`。
标签表达材质语义，状态提供具体 Pass 可沿用或覆盖的默认值。runtime 不内置标签词汇、
从标签推导状态的规则或自动状态组合 helper；标签不会自动生成 shader 功能。
旧 `MaterialBlendMode` 已移除，样例改用 `Surface=Opaque/Transparent` 等应用标签；旧 `TwoSided` 改为 `CullMode::None`。
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
具名程序列表只是当前可用程序的集合；它不是 UE 的完整 shader map，也不表示材质自动参加同名 Pass。

PSO 还需要顶点输入、附件格式和经 Pass 确定的最终状态。runtime 材质不持有 PSO，不安排 Pass，
也不定义参数在 uniform buffer、descriptor 或 push constant 中的 GPU 布局。
`MeshDrawCommand` 继续仅持有 PSO、几何绑定和绘制范围，不重新加入 shader 参数。
修改动态参数不要求重新创建 Resource/PSO，但本例已生成的 `PreparedDraw` 拥有参数副本，
因此要重新生成样例输出才能看到新值；这不等价于 UE 的缓存命令 + 稳定 uniform buffer 绑定。

样例的 M1～M5 注释串起资源准备、定义/实例、Proxy 同步、PSO 准备和参数适配。
八个实例使用三份参数声明相同的 Material/Resource：普通不透明、双面描边不透明和 alpha 混合透明材质；
共享加载的 shader，由样例准备各 Pass 的六个 PSO。具体场景、筛选矩阵和状态见
[教学样例](../guide/build-test.md#mesh-drawing-pipeline-教学样例)。
Depth Pass 沿用材质剔除与深度偏移，显式选择 Less、开启深度测试/写入、关闭模板；
Color Pass 沿用材质颜色、深度和模板状态。双面材质使用 Always/Replace 写入模板参考值 7。
Outline Pass 筛选描边标签，以放大几何和 NotEqual 7 绘制金色轮廓，不改写深度/模板；
Transparent Pass 筛选透明标签，使用材质 alpha 混合、深度测试和关闭深度写入的状态。
附件采用 D32_FLOAT_S8_UINT，参考值由样例 `PreparedDraw` 保存并在 Draw 前录制，
不进入 runtime MeshDrawCommand；生命周期见 [RHI 模板参考值](render-rhi.md#模板参考值)。
颜色默认值/覆盖和 Brightness 都从 Proxy 获取，准备阶段解析参数名称，绘制阶段使用索引。
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

本地 UE 5.8.4 源码路径相对 `Engine/Source/Runtime/`：

- `Engine/Public/Materials/Material.h`、`MaterialInstance.h`、`MaterialInstanceDynamic.h`：用户定义和实例。
- `Engine/Public/MaterialShared.h`：`FMaterial`、`FMaterialResource`、`FMaterialShaderMap`。
- `Engine/Public/Materials/MaterialRenderProxy.h` 与 `Engine/Private/Materials/MaterialInstanceSupport.h`：
  `FMaterialRenderProxy`、`FMaterialInstanceRenderProxy`；UE 同时维护 UniformExpressionCache。
- `Engine/Private/Materials/MaterialInstance.cpp`：`GetParameterValue` 的父级回退和 `SetMIParameterValue` 渲染命令。
- `Renderer/Private/ShaderBaseClasses.cpp`：`FMaterialShader::GetShaderBindings` 绑定材质 uniform buffer；
  `Renderer/Private/DepthRendering.cpp`：Processor 结合材质性质选择 shader 和绘制状态。

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

runtime 材质管理 CPU 参数默认值、覆盖和快照；shader 参数的 GPU 存储、上传、布局校验和绑定由调用方组织。
runtime 不提供 push constant 或 `ShaderParameterSet` 的逐 Draw 绑定契约，也没有完整材质编译系统。
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
