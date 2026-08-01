# ADR-0007 资产生命周期只由引用计数决定

状态: 生效
日期: 2026-07
影响: `modules/runtime/include/radray/runtime/asset.h`、`asset_manager.h`；全部 `Asset` 派生类；`PipelineStateCache`

## 背景

资产普遍向外交出指向自身内部的指针：`ShaderAsset` 交出 `ShaderPassProgram*`，
`TextureAsset` 交出 `render::TextureView*`，`StaticMesh` 交出 `GpuMesh::DrawData*`。
这些指针会被长期缓存——PSO 缓存条目、写进描述符的 view、SceneProxy 的 draw args。

原本 `AssetManager::Unload` 可以单方面销毁槽位，无视引用计数。于是所有这些指针都可能悬垂。

## 决策

**引用计数是资产生命周期的唯一权威。** 没有 `Unload`，没有 `CollectUnreferenced`，
没有闲置缓存。`AssetManager` 不提供任何"无视引用计数销毁资产"的入口。
关卡切换与热重载靠放开引用完成。

由此："持有一份 `StreamingAssetRef`"就足以保证资产交出的内部指针不悬垂，依赖者无需注册
任何失效回调。

**销毁时刻对齐到 `Pump`**，而不是就地发生在引用归零处。这仍然是"归零即销毁"，
不是保留策略。

**没有 `AssetState::Unloaded`**：只要还有一个引用指向 slot，slot 就一定存在，
"已卸载"这个状态没有观察者。

## 放弃的方案及代价

- **保留强制卸载 + 每种资产拆成"槽位 + 引用计数的不可变内容"两层**（曾经的实现）。
  即 `ShaderContent` / `TextureContent` / `StaticMeshContent`。依赖者要同时持有 ref 与
  content 两份引用才安全。那一层的全部存在理由就是防住强制卸载；强制卸载消失后它变成
  纯粹的双重间接与两倍样板，故已合并回资产自身。
  `PipelineStateCache::GraphicsEntry` 也随之从三个成员降到一个 `StreamingAssetRefAny`。
- **保留强制卸载 + 让依赖者注册失效回调**。漏注册回调是不可检测的错误：代码编译通过、
  测试通过，只在卸载时崩。且回调本身要处理"回调期间又有人卸载"的重入。
- **就地销毁（引用归零处直接 delete）**。会让 `~StreamingAssetRefAny`（`noexcept` 路径）
  跑任意代码：资产析构放开它自己持有的 `StreamingAssetRef` 成员会递归销毁别的 slot；
  遍历资产表时放开一个引用会当场令迭代器失效。
- **保留闲置缓存**（引用归零后留一段时间再销毁）。会让"什么时候真的没了"不可预测，
  且与"引用计数是唯一权威"直接冲突——缓存本身就是一份隐式引用。

## 必须保持为真

- `AssetManager` 的公开接口里没有 `Unload` / `Destroy` / `Collect` 之类的入口。
- `AssetState` 里没有 `Unloaded`。
- 资产可以放心向外交出内部指针，前提是接口文档写明"持有一份 `StreamingAssetRef`
  即保证不悬垂"。
- 任何长期缓存资产内部指针的地方必须同时持一份 `StreamingAssetRefAny`，
  且该成员声明在被保护的对象**之前**（析构逆序）。
- 资产不需要提供"内容层"来防御卸载。若发现某个资产又长出了 Content 那一层，
  先确认是不是强制卸载被重新引入了。
