> - 适用: SceneDraw 自动实例化、业务集合与 native 帧内绘制
> - 权威: 当前公共接口用法；生命周期与兼容规则见 render-framework
> - 锚点: `modules/runtime/include/radray/runtime/components/instanced_static_mesh_component.h`, `modules/runtime/include/radray/runtime/render_framework/raster_draw.h`, `benchmarks/bench_material_scene/`, `examples/scene_sync/scene_sync_instanced.hlsl`

# 实例绘制

先让 pass 使用 VisibleSlots 契约，再决定自动合并或显式集合。可运行样例为
`scene_sync`、`framework_stress --mode=draw` 和 `bench_material_scene`；旧 direct shader 仍支持单实例路径。

## Pass 与 shader

```cpp
MaterialPass pass;
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
只活一帧的数据用 PrepareRasterInstances，输入 CPU 矩阵、可见 slot 与可选 RasterFrameParameters；
Prepared 状态的 RasterInstanceData 提供 Objects、VisibleSlots、Parameters 与 Visible 范围。
将这些绑定填入 RasterMaterialValues，借用不可变 CPU 材质和其 MaterialGpuState，再调用 RasterDraw 的准备/录制入口。

CPU 字节及 schema 在 Prepare 内复制，返回后可释放输入数组。外部 GPU buffer 重载验证 device/use/range/stride/alignment，
调用方通过既有 frame owner 或资产退休机制保活到实际完成。两种入口都不接管 mesh、material 或外部 buffer 的所有权。
多个 subrange 用各自 VisibleBase、Instances 和 FirstInstance=0；不能把业务序号直接当 sparse GPU slot。
检查 NoWork/Unsupported/InvalidData/AllocationFailure，不把不支持显式实例化的 shader 静默重复绘制 N 次。

## 压测与验收

`bench_material_scene` 真正创建 GPU mesh/material、同步 World/Scene、准备和录制 draw，覆盖 F=1/2/3/4；原有 F2 名称保持兼容。
自动路径包含 N=1k/10k/100k、M=1/100/独立材质、V=1/3 和 dirty=0/1/10/100%；
Direct/Manual/ManualTint 提供单 draw 对照、集合变换与参数流更新，Churn/UniqueAssetChurn 覆盖三种生命周期时序。
运行命令见[性能测量](performance.md#真实材质和实例化压测)，2026-10-13 的前后实测见[历史验收报告](../history/2026-10-13-material-instancing-performance.md)。
更完整的所有权、排序、容量和缓存契约见[Runtime 架构](../architecture/render-framework.md#pipeline材质与绘制)。
