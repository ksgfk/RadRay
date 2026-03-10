# FrameSnapshot 详细设计

## 1. 目标

`FrameSnapshot` 是逻辑线程与渲染线程之间唯一正式的数据交接对象。它的职责不是表达“如何渲染”，而是表达“本帧有哪些可供渲染系统消费的事实”。

本文档用于明确 `FrameSnapshot` 的：

- 设计目标
- 生命周期与线程边界
- 数据组成
- 内存与所有权规则
- 发布与消费协议
- MVP 字段范围
- 后续扩展方向

## 2. 核心原则

`FrameSnapshot` 必须满足以下原则：

1. 只读
2. 完整
3. 自包含
4. 可跨线程安全传递
5. 不包含 RHI 对象
6. 不依赖逻辑线程后续状态

进一步解释如下。

### 2.1 只读

一旦快照发布，逻辑线程和渲染线程都不允许修改其内容。渲染线程只能读取快照并据此构建本帧渲染图。

### 2.2 完整

快照必须在发布前构造完整。渲染线程不能看到半构造状态，也不能依赖逻辑线程后续补充内容。

### 2.3 自包含

快照中的字段可以引用快照内部内存，但不能引用逻辑世界中的临时容器、ECS 组件内存或会在发布后失效的数据。

### 2.4 可跨线程安全传递

快照发布后，渲染线程必须能够在没有额外读锁的情况下直接消费。

### 2.5 不包含 RHI 对象

快照只描述渲染输入，不应出现 `CommandBuffer`、`Texture*`、`Buffer*`、`DescriptorSet*` 等 GPU 执行层对象。

### 2.6 不依赖逻辑线程后续状态

快照中不能保存“回头去 ECS 查一下”的句柄语义。允许保存稳定资源句柄，但不允许在渲染线程通过这些句柄重新访问逻辑世界组件。

## 3. FrameSnapshot 在系统中的位置

单帧链路如下：

1. `Simulation` 更新世界
2. `Extract` 从世界状态生成 `FrameSnapshot`
3. `FrameSnapshotQueue` 发布快照
4. `Render Thread` 获取快照
5. `Render Pipeline` 基于快照构建 `RenderGraph`
6. `RenderGraph` 编译并执行

因此，`FrameSnapshot` 是逻辑世界的渲染投影，而不是世界本身。

## 4. 生命周期

### 4.1 创建时机

`FrameSnapshot` 在逻辑线程的 Extract 阶段构造。

### 4.2 发布时机

只有当快照内所有数组、视图和元数据都已填充完成后，才允许发布到 `FrameSnapshotQueue`。

### 4.3 消费时机

渲染线程从队列中拿到快照后，可以在整个渲染构图和执行准备阶段持续读取。

### 4.4 释放时机

当渲染线程完成对快照的 CPU 侧消费后，该快照槽位才可被逻辑线程重用。

注意：

- 快照本身不需要一直活到 GPU 执行完成
- 只要渲染线程已经将所需数据转化为 `RenderGraph` 执行计划和 frame-local 上传数据，快照槽位即可释放

## 5. 与 GPU Inflight Frame 的关系

`FrameSnapshot` 生命周期与 GPU inflight frame 生命周期必须解耦。

两者关系如下：

- `FrameSnapshot` 用于 CPU 线程之间的数据交接
- `GPU Inflight Frame` 用于 GPU 命令、上传缓冲、描述符和 fence 管理

不能把二者绑定成同一对象，原因如下：

- 渲染线程可能很快消费完快照，但 GPU 仍未执行完该帧命令
- 若强行绑定，将导致快照队列被 GPU 完成速度拖住
- 这会放大逻辑线程阻塞，破坏交接吞吐

因此建议：

- `FrameSnapshotQueue` 只管理 CPU 快照可见性
- `RenderFrameContext` 管理 GPU inflight 资源

## 6. 数据边界

`FrameSnapshot` 中允许出现的数据：

- 相机渲染参数
- 可见或候选可见的网格实例
- 灯光参数
- 渲染视图请求
- 每帧资源上传请求
- 调试绘制请求
- UI 渲染输入

`FrameSnapshot` 中不允许出现的数据：

- ECS 组件裸指针
- 游戏对象裸指针
- 会失效的 `std::string_view`
- 指向逻辑线程局部容器的 `span`
- 底层 GPU 资源对象
- 图编译结果
- barrier 或 command list

## 7. MVP 的结构划分

MVP 建议拆成“头 + 多个数据数组”的布局，而不是一个巨大的嵌套对象图。

建议结构：

```cpp
struct FrameSnapshotHeader;
struct CameraRenderData;
struct VisibleMeshBatch;
struct LightRenderData;
struct UploadRequest;
struct RenderViewRequest;
struct DebugDrawRequest;
struct UiRenderData;

struct FrameSnapshot {
    FrameSnapshotHeader Header;
    span<const CameraRenderData> Cameras;
    span<const VisibleMeshBatch> MeshBatches;
    span<const LightRenderData> Lights;
    span<const UploadRequest> Uploads;
    span<const RenderViewRequest> Views;
    span<const DebugDrawRequest> DebugDraws;
    span<const UiRenderData> UIs;
};
```

原则：

- 头部放帧级元数据
- 变长数据全部平铺存放
- 尽量使用 POD 风格结构
- 避免深层嵌套指针图

## 8. FrameSnapshotHeader

建议：

```cpp
struct FrameSnapshotHeader {
    uint64_t FrameId;
    uint64_t SimulationTick;
    double CpuTimeSeconds;
    uint32_t ViewCount;
    uint32_t CameraCount;
    uint32_t MeshBatchCount;
    uint32_t LightCount;
    uint32_t UploadCount;
    uint32_t DebugDrawCount;
    uint32_t UiCount;
    uint32_t Flags;
};
```

字段说明：

- `FrameId`
  逻辑发布编号，单调递增

- `SimulationTick`
  对应的仿真 tick，可用于调试与回放

- `CpuTimeSeconds`
  快照生成时的逻辑时钟戳

- `Count` 系列
  便于调试、校验和统计

- `Flags`
  帧级特性开关，例如暂停渲染、调试视图、冻结裁剪等

## 9. CameraRenderData

第一版建议不要只支持单相机，哪怕渲染流程当前只消费主视图，也建议在结构上预留多视图能力。

```cpp
struct CameraRenderData {
    uint32_t CameraId;
    uint32_t ViewId;
    uint32_t Flags;
    uint32_t LayerMask;
    float NearPlane;
    float FarPlane;
    float VerticalFov;
    float AspectRatio;
    float Exposure;
    Eigen::Matrix4f View;
    Eigen::Matrix4f Proj;
    Eigen::Matrix4f ViewProj;
    Eigen::Matrix4f InvView;
    Eigen::Matrix4f InvProj;
    Eigen::Vector3f WorldPosition;
    uint32_t OutputWidth;
    uint32_t OutputHeight;
};
```

设计要求：

- 直接保存渲染所需矩阵，不要求渲染线程再回头推导
- 保存输出分辨率，支持后续多分辨率 view
- `ViewId` 用于把 camera 与 render view request 对应起来

## 10. VisibleMeshBatch

这里的命名故意不叫 `RenderableObject`，因为渲染线程需要的是可直接进入裁剪、排序、合批流程的结构，而不是游戏对象抽象。

```cpp
struct VisibleMeshBatch {
    uint64_t InstanceId;
    uint32_t ViewMask;
    uint32_t Flags;
    MeshHandle Mesh;
    MaterialHandle Material;
    uint32_t SubmeshIndex;
    uint32_t TransformIndex;
    Eigen::Matrix4f LocalToWorld;
    Eigen::Matrix4f PrevLocalToWorld;
    BoundingBox WorldBounds;
    uint32_t SortKeyHigh;
    uint32_t SortKeyLow;
};
```

字段设计意图：

- `InstanceId`
  稳定实例标识，用于调试和历史匹配

- `ViewMask`
  说明该对象可能在哪些 view 中参与渲染

- `Flags`
  例如 opaque、alpha test、cast shadow、receive shadow、motion vector

- `Mesh` / `Material`
  只保存稳定资源句柄，不保存 GPU 对象

- `PrevLocalToWorld`
  为未来运动矢量、TAA、插值调试留口

- `WorldBounds`
  渲染线程做裁剪用

- `SortKey`
  可以在 Extract 阶段预生成粗粒度排序键，减少渲染线程重复工作

MVP 如果不做多 view，也可以先简化 `ViewMask`，但建议保留字段。

## 11. LightRenderData

第一版建议先支持方向光、点光和聚光，统一用一个结构表达。

```cpp
enum class LightType : uint32_t {
    Directional,
    Point,
    Spot,
};

struct LightRenderData {
    uint64_t LightId;
    LightType Type;
    uint32_t Flags;
    Eigen::Vector3f Position;
    float Range;
    Eigen::Vector3f Direction;
    float SpotAngleScale;
    Eigen::Vector3f Color;
    float Intensity;
    uint32_t ShadowPolicy;
    uint32_t ShadowMapSize;
};
```

设计要求：

- 统一为渲染友好格式
- 不保留逻辑层复杂层级关系
- `ShadowPolicy` 只表达需求，不表达具体 shadow pass 实现

## 12. UploadRequest

`UploadRequest` 用于描述本帧需要提交到 GPU 的资源数据，不等于立即上传命令。

```cpp
enum class UploadTargetType : uint32_t {
    Buffer,
    Texture,
};

struct UploadRequest {
    UploadTargetType TargetType;
    uint32_t Flags;
    ResourceAssetHandle Asset;
    uint64_t TargetOffset;
    uint64_t DataSize;
    const byte* Data;
};
```

设计要求：

- 用于表达“有哪些内容需要上传”
- 不直接包含 staging buffer 或命令信息
- `Data` 指向的内存必须属于快照自身的稳定存储区，不能指向逻辑线程临时容器

如果后续要支持分块纹理上传，可再扩展子资源区间描述。

## 13. RenderViewRequest

`RenderViewRequest` 用于描述本帧想渲染哪些视图，而不是默认只有一个主屏输出。

```cpp
enum class RenderViewType : uint32_t {
    MainColor,
    Shadow,
    Reflection,
    Probe,
    EditorViewport,
};

struct RenderViewRequest {
    uint32_t ViewId;
    RenderViewType Type;
    uint32_t CameraId;
    uint32_t OutputWidth;
    uint32_t OutputHeight;
    uint32_t Flags;
};
```

MVP 即使只做主屏输出，也建议从结构上区分 `Camera` 与 `View`：

- 一个 camera 可以派生多个 view
- 一个 view 决定一条渲染输出请求

## 14. DebugDrawRequest

调试绘制不要直接绕过渲染系统，应作为快照输入的一部分。

```cpp
enum class DebugDrawType : uint32_t {
    Line,
    Box,
    Sphere,
    Frustum,
};

struct DebugDrawRequest {
    DebugDrawType Type;
    uint32_t Flags;
    Eigen::Vector4f Params0;
    Eigen::Vector4f Params1;
    Eigen::Vector4f Color;
};
```

好处：

- 调试绘制与正常渲染同样受线程边界约束
- 不会引入逻辑线程直接操作渲染器的后门

## 15. UiRenderData

即使 UI 系统未来可能独立演化，MVP 也可以先让其作为快照的一部分。

```cpp
struct UiRenderData {
    uint32_t ViewId;
    uint32_t Flags;
    span<const byte> Payload;
};
```

这里的 `Payload` 只表示“已被某个 UI 前端整理好的渲染输入”。具体编码方式可以后续再细化。

## 16. 内存布局建议

`FrameSnapshot` 不建议在发布后仍依赖多处分散堆分配。推荐采用“单个 SnapshotArena + 头部 span”模式。

### 16.1 Builder 阶段

逻辑线程使用 `FrameSnapshotBuilder`：

```cpp
class FrameSnapshotBuilder {
public:
    void Reset();
    CameraRenderData& AddCamera();
    VisibleMeshBatch& AddMeshBatch();
    LightRenderData& AddLight();
    UploadRequest& AddUpload();
    RenderViewRequest& AddView();
    FrameSnapshot Finalize();
};
```

### 16.2 Finalize 阶段

`Finalize()` 的职责：

- 固定所有数组长度
- 将 builder 内部容器整理到稳定内存
- 生成 `span`
- 填充 Header 统计字段
- 返回只读 `FrameSnapshot`

### 16.3 推荐实现方向

可考虑：

- 每个 snapshot slot 持有一个可复用 arena
- builder 写入 arena 中的线性内存
- finalize 后将不同类型数组转为 `span`

好处：

- 内存连续
- 分配开销可控
- 易于复用
- 更适合跨线程发布

## 17. 所有权规则

必须明确以下所有权关系。

### 17.1 Snapshot 拥有其内容

快照中的数组、字符串、上传字节流、UI payload 都必须由快照槽位自己拥有。

### 17.2 Resource Handle 不拥有资源

如 `MeshHandle`、`MaterialHandle`、`ResourceAssetHandle` 只作为稳定引用键，不拥有资源本体。

### 17.3 Snapshot 不拥有逻辑对象

快照是提取后的副本或轻量投影，不负责维护逻辑对象生命周期。

## 18. 发布与消费协议

建议 `FrameSnapshotQueue` 使用固定大小 ring buffer。

每个槽位建议具有如下状态：

- `Free`
- `Building`
- `Published`
- `Reading`

推荐协议：

1. 逻辑线程申请一个 `Free` 槽位
2. 将槽位置为 `Building`
3. 构造快照
4. `Finalize`
5. 原子地将槽位置为 `Published`
6. 渲染线程获取最新 `Published` 槽位
7. 将其置为 `Reading`
8. 消费完成后置回 `Free`

约束：

- 渲染线程绝不能读 `Building`
- 逻辑线程绝不能覆盖 `Reading`

## 19. 丢帧策略

如果渲染线程处理落后于逻辑线程，允许丢弃旧的 `Published` 快照。

原则：

- 优先显示最新完整快照
- 不追求每个仿真帧都被渲染
- 不允许渲染半完成快照

这意味着：

- `FrameId` 可能跳变
- 但渲染结果始终对应某个完整逻辑时刻

## 20. 校验规则

在 `Finalize()` 或调试构建中，建议加入以下校验：

1. `Header.Count` 与实际数组长度一致
2. 所有 `span` 指向当前槽位拥有的内存
3. 所有 `UploadRequest::Data` 指向有效快照区间
4. `CameraId` / `ViewId` 引用关系合法
5. `MeshHandle` / `MaterialHandle` 非空
6. `WorldBounds` 有效
7. 输出尺寸非零

这些检查越早做，后面的 graph 和 RHI 越稳定。

## 21. MVP 推荐的最小字段集

如果要先快速落地第一版代码，建议最小字段集如下：

```cpp
struct FrameSnapshot {
    FrameSnapshotHeader Header;
    span<const CameraRenderData> Cameras;
    span<const VisibleMeshBatch> MeshBatches;
    span<const LightRenderData> Lights;
    span<const UploadRequest> Uploads;
    span<const RenderViewRequest> Views;
};
```

先不做：

- `DebugDrawRequest`
- `UiRenderData`
- 多相机高级参数
- 复杂动态材质参数块

等链路稳定后再扩展。

## 22. 后续扩展方向

`FrameSnapshot` 稳定后，可以逐步增加以下能力：

- 动态材质参数块
- Skinning/Animation 提取结果
- 粒子发射器渲染输入
- 地形块渲染输入
- 实例化簇数据
- 运动矢量历史匹配数据
- Ray Tracing 场景构建输入
- 编辑器 gizmo 与 overlay 数据

扩展原则不变：

- 只表达输入事实
- 不表达执行命令
- 不引入 RHI 对象

## 23. 结论

`FrameSnapshot` 的本质是渲染系统的 CPU 输入包。它既不是 ECS 的镜像，也不是渲染命令流，而是介于两者之间的一层稳定边界。

一个设计正确的 `FrameSnapshot` 应做到：

- 逻辑层可以放心发布
- 渲染层可以无锁消费
- Render Graph 可以直接基于它构图
- 资源上传、裁剪、排序、批处理都能围绕它组织

只要 `FrameSnapshot` 这层边界设计稳，后续线程模型、Render Graph、Temporal 资源和更复杂的实时渲染能力都可以持续迭代，而不需要回头打穿逻辑层与渲染层之间的隔离。
