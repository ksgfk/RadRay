> - 适用: 新增资产类型；排查资产泄漏、悬垂或"卸载了但对象还在"；改生命周期
> - 权威: 本文是资产生命周期与引用语义的唯一说明。GPU 帧边界与延迟销毁的实现侧见 `architecture/frame-and-gpu.md`
> - 锚点: `modules/runtime/include/radray/runtime/asset.h`, `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/include/radray/runtime/pipeline_layout_cache.h`, `modules/runtime/src/asset_manager.cpp`

# 资产系统

## 一条不变量决定了整个设计

> **引用计数是资产生命周期的唯一权威。**

没有 `Unload`，没有 `CollectUnreferenced`，没有闲置缓存。`AssetManager` 不提供任何
"无视引用计数销毁资产"的入口。关卡切换与热重载靠放开引用完成。

这条不变量买到的是一整类问题的消失。资产普遍向外交出指向自身内部的指针——
`ShaderAsset` 交出 `ShaderPassProgram*`，`TextureAsset` 交出 `render::TextureView*`，
`StaticMesh` 交出 `GpuMesh::DrawData*`，而这些指针会被长期缓存（PSO 缓存条目、写进描述符
的 view、SceneProxy 的 draw args）。只要"持有一份 `StreamingAssetRef`"就足以保证这些指针
不悬垂，依赖者便无需注册任何失效回调——而漏注册回调是不可检测的错误。

完整推导与被删掉的那条路见 [ADR-0007](../adr/0007-asset-lifetime-refcount-only.md)。

## 销毁时刻对齐到 Pump

引用归零后资产**立即**被销毁，但那个"立即"对齐到 `AssetManager::Pump` 这一确定时刻，
而不是就地发生在引用归零处。

就地销毁会让 `~StreamingAssetRefAny`（一条 `noexcept` 路径）跑任意代码：资产析构会放开它
自己持有的 `StreamingAssetRef` 成员，从而递归销毁别的 slot；而遍历资产表时放开一个引用会
当场令迭代器失效。对齐到 Pump 后两个问题一并消失。

这仍然是"归零即销毁"，不是保留策略——只是动作对齐到帧内一个固定点。`_collecting` 那个标志
是递归保护：收集零引用槽位时销毁资产会放开它持有的引用，从而令更多 slot 归零。

## 状态机

```
Loading ──┬─> Ready       加载协程成功，Object 已构造
          ├─> Faulted     加载失败
          └─> Canceled    加载被取消
```

**没有 `Unloaded` 态**：只要还有一个 `StreamingAssetRef` 指向 slot，slot 就一定存在，
"已卸载"这个状态没有观察者。从前它存在是因为 `Unload` 能无视引用计数销毁槽位。

三个终态（Ready / Faulted / Canceled）都是 `IsCompleted()`。`IsValid()` 只表示引用非空，
四种状态都算 valid；默认构造与 `Reset()` 后 invalid。

Faulted 槽位不会永久占住 id：没人持有时它会在下一次 Pump 被销毁，id 随之释放。

## 引用类型

| 类型 | 用途 |
|---|---|
| `StreamingAssetRefAny` | 类型擦除引用。同时表达加载状态与 ready 后的资产访问 |
| `StreamingAssetRef<T>` | 类型安全视图。`Ready` 且实例 is-a T 时 `Get()` 才非空 |

表示是 `manager + slot` 裸指针。slot 是 `unordered_map` 里的 `unique_ptr` 元素，地址稳定，
而 `RefCount > 0` 保证它不被销毁——故**不需要 generation 校验，也不需要每次访问都按 id
查一次哈希表**（`Get()` 在渲染热路径上：PSO 缓存、SceneProxy）。

**单线程。** 拷贝 / 移动 / 析构 / 状态查询 / 资产访问全部只能在拥有 `AssetManager` 的
线程（主线程）进行——`RefCount` 是普通整数，且增减要触碰 manager 的表。资产**内部数据**
被各系统怎么跨线程使用不属资产系统管辖，但引用本身不跨线程。

**必须全部死在 `AssetManager` 之前。** slot 随 manager 一同释放，之后 `_slot` 悬垂，
连 `IsValid()` 都答不出来。关停顺序见下文。

`operator==` 比较的是**是否指向同一个槽位**，这是 id 去重的可观测形式。刻意**不**比较
`AssetId`——两个无效引用的 id 都是空，那样会相等。

`CastTo<T>()` 在 `Ready` 之前不能拒绝（最终类型未知，那正是 streaming 引用要表达的
"还没到"）；`Ready` 之后类型不符则返回空引用。

## 等待

`co_await ref` 得到 `bool`：`true` = 已到终态，`false` = **等待者自己**被取消
（不是资产加载失败）。恢复点在 `Pump` 把加载结果提交、槽位进入终态之后。

`AssetWaitAwaitable` **持有 ref 的副本**，这是必须的：等待期间它是槽位的一个引用持有者，
于是槽位不会因"外部都放手了"而在 Pump 里被回收——那会让等待记录指向一个已销毁的 slot。

`await_suspend` 模板化以拿到 promise：取消所需的 stop token 只能从 promise 的 env 里取，
而 `coroutine_handle<>` 已经把它擦除了（见 `GetCoroutineStopToken`）。

`AssetManager::Wait(ref)` 是薄转发，直接 `co_await ref` 等价；它额外把"等待者被取消"
转成对当前 task 的 stop 传播。

## 加载

```cpp
AssetLoadRequest request;
request.Id = MakeAssetIdFromPath("myasset", path);
request.Task = MyLoadCoroutine(...);   // task<AssetLoadResult>
auto ref = assetManager.Load<MyAsset>(std::move(request));
```

`AssetManager` 只消费统一的 `task<AssetLoadResult>`，loader 的参数形状完全由调用方决定。
按 id 去重：命中在飞或已就绪的 slot 直接复用。

**在飞期间 manager 自持一份引用**（`_activeLoads`）：加载期间外部引用可能全部消失，
但槽位要活到协程跑完。

**dedup 命中时协程帧一次都不 resume。** 这条对所有带 options 的 loader 都成立：第二次
调用携带的参数会被静默丢弃。所以带共享设施指针的 loader 必须在**发起加载之前**校验，
并在 dedup 命中时核对（`ShaderAsset` 就这么做，见 `architecture/shader-pipeline.md`）。

## AssetId

```cpp
AssetId MakeAssetIdFromPath(std::string_view namespacePrefix, const std::filesystem::path& path);
```

`namespacePrefix` 做资产类型的命名空间隔离（`"shader"` / `"image"` / ...）：同一路径在
不同资产类型下必须得到不同 id，否则一份 `*.png` 既当 `ImageAsset` 又当 `TextureAsset` 时
会撞进同一个 slot。

**路径先归一化再哈希，这是正确性要求而非优化。** 未归一化时 `"a/../b/x"` 与 `"b/x"` 得到
两个 id 却指同一个文件，于是同一份 manifest 被建成两个资产，各自持有一套 `PipelineLayout`
与字节码缓存——表现为"shader 编了两遍、layout 缓存命中率莫名减半"，且没有任何报错。

归一化口径（改动前先读 [ADR-0008](../adr/0008-asset-id-path-normalization.md)）：

1. `weakly_canonical` —— 消掉 `.` / `..` 并解 symlink，与 shader 源码身份的口径一致。
   失败时退到 `absolute + lexically_normal`，再失败退到纯词法归一化。
2. `generic_string` —— 分隔符统一为 `/`。
3. Windows 下转小写；POSIX 下**刻意不转**。

## 延迟销毁

引用归零后资产立即销毁，但资产内部的 GPU 对象可能仍被 GPU 读取。`Asset::OnUnload` 是
把这类数据交出去延迟销毁的时机。

```cpp
void MyAsset::OnUnload(AssetManager& manager) override {
    manager.DeferDestroy([tex = std::move(_texture), view = std::move(_view)]() mutable {
        // 空 body。析构顺序由捕获声明顺序表达：view 先于 tex
    });
}
```

`OnUnload` 的**职责是"交出需要延迟销毁的数据"，不是"释放资源"**。纯 CPU 数据无需在此
处理——析构函数会做，且那才是唯一正确的地方。不放进析构函数的理由是析构里拿不到 manager，
而"交给谁延迟"这件事必须由 manager 回答（它持有 `IWaitFrameProcessor`）。

**纯 CPU 资产的 `OnUnload` 就是个空函数**（`ImageAsset` 即如此）：

```cpp
void MyCpuAsset::OnUnload(AssetManager& manager) override { (void)manager; }
```

`DeferDestroy` 只给 `Asset::OnUnload` 用。非资产的持有者要延迟销毁 GPU 对象，
直接 `co_await IWaitFrameProcessor::Wait()`，见 `architecture/frame-and-gpu.md`。

`DeferDestroy` **整包交出，不逐个交**。payload 是一个可移动的可调用对象，销毁顺序由
payload 内部的捕获/成员声明顺序**显式表达**。逐对象交出的接口（从前的
`IRenderResourceRecycler::RecycleRenderResource`）把销毁顺序寄托在队列语义上，
那是一条无法在类型上表达、也无法在 review 中看见的隐式契约。

**一帧一个协程帧**：同一次 Pump 内的全部 payload 攒成一批，共用一个等待帧边界的协程。
故大量资产同时归零不会产生大量协程。

wait processor 未装配时会立即销毁 payload 并记 error log。装配见下文。

## 关停顺序

```
World → RenderSystem → AssetManager → GpuSystem
```

理由：

- 资产要到 `AssetManager` 析构时才放开最后一份引用，所以 `PipelineLayoutCache`
  （宿主是 `RenderSystem`）会**先于**它的持有者销毁。这是常规路径而非异常路径，
  缓存的析构会把残留条目的所有权交还给它们自己。
- `AssetManager` 必须在 `GpuSystem` 之前销毁：GPU 资源必须在 device 之前交出。

`AssetManager` 析构时若仍有存活引用，**记 error log 并照样卸载，不 abort**。此刻两种结果
都坏，而泄漏比悬垂更难查，且关停期 abort 会掩盖真正的首因（通常是某个 system 忘了 reset）。

`AssetManager` 关停时先 `PumpLoadResults` 再清 `_activeLoads`——否则 pending 结果永远不会
跑 `OnUnload`。刻意不用 `Pump`（那会 spawn 一个立刻被取消的协程）。

## 服务装配

`AssetManager` 需要注入 `IWaitFrameProcessor`。走 `ServiceRegistry` 的三阶段：

```cpp
template <> struct ServiceTraits<AssetManager> {
    static constexpr auto Inject = std::tuple{&AssetManager::SetWaitFrameProcessor};
};
```

`ServiceRegistry` 是非侵入、无单例、trait 驱动的三阶段装配（实例化 → 装配 → 初始化）。
装配发生时全部实例已存在，故互相持有引用（`WindowManager` ↔ `GpuSystem`）天然可解。
setter 形参是基类时，只要来源类型用 `RuntimeTypeTrait<T>::Bases` 声明了该基类，
`Add` 会自动登记基类别名。详见 `service_registry.h` 的用法示例。

## PipelineLayoutCache：一个不占引用计数的缓存

`PipelineLayout` 只由 binding 布局决定，与 keyword variant / 后端 target 无关。规模化后
大量 pass 的布局逐字节相同（最常见的是"一个 CBuffer + 一张贴图"），一个 pass 一份
root signature 纯属浪费。故按内容去重，pass 之间、资产之间都共享。

**缓存拥有条目但不占引用计数。** 计数归零即从表里摘除并销毁——没有"留个墓碑等着被发现"
的中间态，故同样内容再取一次是干净的 miss。

**缓存允许先死于持有者，这是常规路径。** 见上文关停顺序。缓存析构时把残留条目的所有权
交还给它们自己（release + 切断反向指针），此后 layout 自持，最后一份引用归零时自毁。
`SharedPipelineLayout::IsCached()`（即 `_cache == nullptr` 的反面）就是"已脱离缓存"这一
状态的全部。

**layout 不需要延迟销毁。** 后端 PSO 建成后仍存着 `PipelineLayout` 裸指针（D3D12 的
`GraphicsPsoD3D12` 存 `RootSigD3D12*` 并在每次 bind 时解引用），所以 layout 不能在 PSO
还活着时消失。这由引用计数保证：`PipelineStateCache` 的每个条目透过 `StreamingAssetRefAny`
保住 `ShaderPassProgram`，后者持有一份 `SharedPipelineLayout` 引用。故资产卸载放开自己
那份是安全的。

### key 的归一化

语义相同的布局必须得到相等的 key，否则 manifest 的书写方式会凭空劈开缓存条目。
归一化做三件事，每一件都对应后端建 layout 时的既有行为：

1. 合并 `GroupIndex` 相同的 parameter set（两个后端都把同组的 entry 拼在一起）；
2. 组内 entry 按 `Binding` 排序（两个后端都排，且都拒绝重复 binding，故序是全序）；
3. 组按 `GroupIndex` 排序。

**刻意保留空组**：空组是可观测的。D3D12 会为它留一个 parameter group，
`CreateShaderParameterSet` 能查到；Vulkan 的 `setLayoutCount` 取 `max(GroupIndex)+1`，
一个末尾空组会改变 set layout 的个数。

两处容易踩的实现细节：

- **哈希逐字段算，不 `memcmp`**。`Entry` 含 `optional<SamplerDescriptor>`，填充字节会让
  两个 `operator==` 相等的对象拿到不同的散列值。
- **移动构造/赋值必须自己写**。默认移动会留下"空 vector + 原 `_hash`"的源，那个源与一个
  真正的空 key 内容相等却散列不等，直接违反 `unordered_map` 的 `equal => same hash` 契约。
  移动后把源的散列值重算（此时源已空，常数开销）。

注意这条与 `RenderPassRegistry` 相反：那里**不做归一化**，因为附件下标就是渲染目标槽位。
见 `architecture/render-rhi.md`。

## PSO 缓存条目只需一份引用

`PipelineStateCache::GraphicsEntry` 只持一个 `StreamingAssetRefAny Ref`，就同时保住了
资产、`ShaderPassProgram` 和 `PipelineLayout`。因为引用计数是唯一权威，没有任何入口能在
计数非零时销毁槽位。

从前那两个独立成员（`shared_ptr<void> Content` / `IntrusivePtr<SharedPipelineLayout> Layout`）
都是为了防御"无视计数的强制卸载"而存在，那条路已经不存在了。

**声明顺序有意义**：`Ref` 必须在 `Object` 之前，析构逆序保证 PSO 先死，之后才放开资产引用。

## 现有资产类型

| 类型 | 内容 | 交出的内部指针 |
|---|---|---|
| `ImageAsset` | 纯 CPU 像素（`ImageData`） | — |
| `TextureAsset` | device-local `render::Texture` + 默认 SRV + 子 view 缓存 | `render::TextureView*` |
| `ShaderAsset` | 一份 manifest，N 个 `ShaderPassProgram` | `ShaderPassProgram*` |
| `StaticMesh` | CPU `MeshResource` + sections/bounds + `GpuMesh` | `GpuMesh::DrawData*` |

`TextureAsset` 的 view 缓存与"资产不可变"不矛盾：缓存是纯派生数据——同一个
`TextureSubViewDesc` 永远得到同一个 view，填充顺序不改变任何观察结果。
同 `ShaderPassProgram` 惰性编译字节码。因此绑定点拿到的 view 指针在**持有一份
`StreamingAssetRef` 期间**永不悬垂，材质快照只需存 "ref + 描述值"，零裸指针。

## 新增一个资产类型

1. 继承 `Asset`，实现 `OnUnload` 与 `GetTypeId`。
2. 特化 `RuntimeTypeTrait<MyAsset>`，给一个**新生成的** GUID，`Bases = std::tuple<Asset>`：

   ```cpp
   template <>
   struct RuntimeTypeTrait<MyAsset> {
       static constexpr RuntimeTypeId value{0x1234abcd, 0x5678, 0x9012, /* ...16 字节... */};
       using Bases = std::tuple<Asset>;
   };
   ```

   `GetTypeId()` 返回 `runtime_type_id_v<MyAsset>`。**GUID 必须是新生成的**——复用别人的会让
   `CastTo<T>` 认错类型。
3. 写 `MakeMyAssetId`（走 `MakeAssetIdFromPath`，给一个独占的 namespace 前缀）。
4. 写 `CreateMyAsset`（同步，构造即完整）与 `LoadMyAsset`（经 `AssetManager::Load`）。
5. 若持有 GPU 对象，在 `OnUnload` 里用 `DeferDestroy` 整包交出。纯 CPU 数据不要碰。
6. 加测试。`test_asset_slot.cpp` 里的 fake asset 让槽位语义测试无需 GPU。

最接近的抄写对象：纯 CPU 的看 `image_asset.h`，持 GPU 对象的看 `texture_asset.h`。

## 测试

| 套件 | 覆盖 |
|---|---|
| `AssetSlotTest` | 引用计数唯一权威下的槽位生命周期规则。用 fake asset，无需 GPU |
| `PipelineLayoutKeyTest` | 内容去重、`unordered_map` key 契约 |
| `PipelineLayoutCacheTest` | 引用计数两端 |
| `PipelineLayoutCacheShutdownTest` | 缓存先死的关停路径 |
| `PipelineStateCacheTest` | 每个 PSO key 维度都参与身份 |

`test_asset_slot.cpp` 里的 `ManualGate` 是必要的：`async_scope::spawn` 会**同步**跑完
只有 `co_return` 的 task，所以测试需要一个能挂住协程的闸门。必须写
`co_await gate.Wait()`——对 gate 直接 `operator co_await` 会拷贝它。
