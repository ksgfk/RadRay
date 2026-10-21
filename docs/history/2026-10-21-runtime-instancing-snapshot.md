> - 适用: 查阅 runtime 移除前的 instancing 设计与操作
> - 权威: 2026-10-21 归档快照；不是当前契约或本次验证结果
> - 锚点: `docs/history/2026-10-21-runtime-archive.md`

状态：已归档。日期为本次归档日期。基线：`751aca4e6a70203e6a62f9de67893b33463b94fc`，文件树等同原 HEAD `ed23aef93b779082d3efab221bbe52d9f30051f0`。
原文件（相对 docs 根）：`guide/instancing.md`。正文中的“当前”、命令、测试与能力均限定于此基线；共享说明保留上下文，不覆盖当前长期文档。
原源码锚点（通过归档基线查阅）：`modules/runtime/include/radray/runtime/components/instanced_static_mesh_component.h`, `modules/runtime/include/radray/runtime/render_framework/renderer.h`, `benchmarks/bench_material_scene/`, `examples/scene_sync/scene_sync_instanced.hlsl`

# 实例绘制

先让 pass 使用 VisibleSlots 契约，再决定自动合并或显式集合。可运行样例为
`scene_sync`、`framework_stress --mode=draw` 和 `bench_material_scene`；旧 direct shader 仍支持单实例路径。

## Pass 与 shader

```cpp
MaterialTemplateDescriptor pass;
pass.Name = "Unlit";
pass.Program.SourceName = "scene_sync_instanced.hlsl";
pass.Inputs = {{"Objects", MaterialInputSource::SceneObjects},
               {"ViewData", MaterialInputSource::ViewConstants},
               {"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots}};
pass.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
pass.AllowAutomaticInstancing = true;
pass.PushConstant = "DrawData";
pass.SceneIndexField = "VisibleBase";
```

shader 使用 `uint slot = VisibleInstanceSlots[DrawData.VisibleBase + instanceId]`，再按 slot 读 Objects/InstanceParameters。
SceneDraw 总是录制 FirstInstance=0；像素阶段需要 slot 时用 nointerpolation varying 传递。
VisibleBase 必须是反射中真实的 uint 标量字段，不必位于 push block 的第一字节。
若 block 还有其他字段，用完整 ScenePushValues 设置 draw-uniform 默认值；VisibleBase 在每批准备时被覆盖。
材质固定输入与 scene/view 输入分组可复用持久默认绑定；group 编号没有硬编码语义。

普通 StaticMeshComponent 配置这样的 Material 即可自动合批。不透明/alpha-test pass 只有显式 opt-in 才允许合并；
Blend/PreserveOrder 保持顺序。SortGroup/Order 定义稳定升序域。不同 section 范围、材质版本或 winding 会拆批。

## 一个组件管理实例

```cpp
auto* instances = actor->AddComponent<InstancedStaticMeshComponent>();
instances->SetStaticMesh(mesh);
instances->SetMaterials({material});
vector<InstanceInit> initial(100000);
vector<InstanceHandle> handles(initial.size());
// 给 initial[i].LocalToComponent 填写相对于组件的有限 affine 矩阵。
if (!instances->AddInstances(initial, handles)) return;
const InstanceUpdate changed{handles[50000], movedMatrix};
instances->UpdateInstances(std::span{&changed, 1});
instances->RemoveInstances(std::span{handles}.subspan(100, 10));
```

所有操作在业务拥有线程、Collect 之外执行。Add 立即返回 handle；删除/压缩/渲染断连重连不改变其余 handle。
GetShapeId 只查询当前渲染连接，首次 capture 前或断连时为空，不能用它替代业务 handle。
参数用 SetInstanceParameters(handles, layout, packedBytes)；字节顺序与 handle span 相同，长度必须等于 count×stride。
同一流的 schema 不变，新加入行默认零字节。整个集合共享 mesh/material 配置；按实例差异使用参数流。
自动合批开关可以关闭，集合仍显式实例化，并使用独立 batch domain。DirectObject pass 对显式集合返回不支持。
父组件移动需要更新全部最终矩阵；一组件仍产生每实例必要的 Shape/矩阵/参数，无法消除 O(N) 存储和剔除。

## Native producer

持久实例通过 SceneWriter 的 CreateShapes/SetStaticMeshes/SetTransforms/SetMaterials/SetInstanceParameters span 接口交付。
帧内数据通过 FrameBuilder::StoreData 复制矩阵和参数 bytes，返回 ResourceId；AddView 拥有可见 slot 数组。
RendererDrawValues 指定 Objects、带完整 InstanceParameterLayout 的 Parameters；AddDraw 保存几何、push、实例数量与输入快照。
所有请求收集后 Compile，再以 FrameDrawId 录制。输入 CPU 数组可在复制后释放。

外部 buffer 使用 Renderer::ImportBuffer/CreateBufferRange，owner 保留到 RequestResourceExit 确认。
框架常量/storage 使用 Renderer 自身 arena，不能把会被 flight reset 重写的 buffer 切片挂进持久缓存。
多个 subrange 由 draw 的 VisibleBase、Instances 和 FirstInstance=0 表达；业务 handle 不等于 sparse GPU slot。
检查注册、AddDraw、Compile 和 Record 的结果，失败仍须完成已接纳工作及输出收尾。

## 压测与验收

`bench_material_scene` 真正创建 GPU mesh/material、同步 World/Scene、准备和录制 draw，覆盖 F=1/2/3/4；原有 F2 名称保持兼容。
自动路径包含 N=1k/10k/100k、M=1/100/独立材质、V=1/3 和 dirty=0/1/10/100%；
Direct/Manual/ManualTint 提供单 draw 对照、集合变换与参数流更新，Churn/UniqueAssetChurn 覆盖三种生命周期时序。
运行命令见[性能测量](2026-10-21-runtime-performance-snapshot.md#真实材质和实例化压测)，2026-10-13 的前后实测见[历史验收报告](2026-10-13-material-instancing-performance.md)。
更完整的所有权、排序、容量和缓存契约见[Runtime 架构](2026-10-21-runtime-render-framework-snapshot.md#pipeline材质与绘制)。
