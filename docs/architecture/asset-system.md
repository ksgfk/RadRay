> - 适用: 资产生命周期、引用计数、加载去重或延迟 GPU 销毁
> - 权威: 本文是 runtime 资产系统的唯一说明；帧边界与上传见 [frame-and-gpu](frame-and-gpu.md)，开发时身份登记与 AssetDatabase 见 [asset-database](asset-database.md)
> - 锚点: `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/src/asset_manager.cpp`, `modules/runtime/include/radray/runtime/asset.h`, `modules/runtime/include/radray/runtime/asset_source.h`, `modules/runtime/include/radray/runtime/texture_asset.h`, `modules/runtime/include/radray/runtime/static_mesh.h`, `modules/runtime/src/static_mesh.cpp`

# 资产系统

`AssetManager` 是资产 slot、异步加载结果和引用计数的唯一拥有者。调用方持有
`StreamingAssetRef<T>`，不直接拥有 slot，也不能绕过引用计数强制释放资产。

## 生命周期

```text
Load request / source task → AssetSlot::Loading → AssetManager::Pump → Ready
                                               ↓
                              最后一份 StreamingAssetRef 归零
                                               ↓
                                  下一次 Pump 调用 Asset::OnUnload
```

引用归零后由 `Pump` 统一销毁，不在引用析构时就地删除 slot，避免连锁释放引起递归析构和
遍历期间的迭代器失效。强制卸载会使资产交出的内部指针失效，因此不提供绕过引用计数的入口，
也不另设一层内容引用来补救；释放时机由最后一个显式 owner 决定。

引用从非零降至零时，将 slot 放入 manager 的侵入式候选队列；每个 slot 至多排队一次。
`Pump` 只访问这些候选，不扫描全部常驻资产。弹出时再次检查引用，期间重新取得的引用会阻止销毁，
以后再次归零仍可入队。先从 ID 表摘除旧 slot，再调用 `OnUnload`，允许重入加载同 ID；依赖链释放
产生的新候选超过入口尾部截止时留到下一次 Pump；不递归清库。加载结果也先冻结 ready owner，再派发等待者；回调取消其他等待者后，以记录身份和序号重新验证。
关停对仍被错误持有的 slot 保留强制卸载诊断路径，它不属于普通帧回收。

加载去重按 `AssetId` 进行。dedup 命中时不会重新执行 loader，因此带 options 的 loader
必须在发起请求前检查参数；不能把一次请求的共享设施指针寄希望于第二次命中时更新。

加载有两种入口，共用同一张 slot 表：显式 `Load(AssetLoadRequest)` 直接提交 task；
`Load(AssetId)` / `Load<T>(path)` 经可选 `IAssetSource` 取得 task。source 未装配或未命中时返回
无效引用，不创建 faulted slot。`AssetDatabase` 是当前 source 实现，细节见
[asset-database](asset-database.md)。

`AssetLoadResult::Success` 只携带 `unique_ptr<Asset>`；`AssetSlot` 只保存实际对象，不另存声明类型
或对象 GUID，也不做两份类型事实的交叉校验。空对象的 success 仍作为加载失败提交。

`StreamingAssetRef<T>` 是实际 `Asset` 对象上的 RTTI 视图，内部仍由 `StreamingAssetRefAny`
参与计数。`T` 可以是任意完整类类型，不要求继承 `Asset` 或声明 GUID；`Get()` 以指针形式
`dynamic_cast`，并返回 `Nullable<T*>`。`StreamingAssetRefAny::Get()` 同样返回
`Nullable<Asset*>`。精确类型判断必须先确认对象存在，再对对象使用 `typeid`。

Loading 时最终对象未知，`Load<T>`、`Find<T>`、`Wait<T>` 和 `CastTo<T>` 都可以先建立接口或
其他类视图。若最终 Ready 对象不能转换为 `T`，既有视图继续持有同一 slot 并观察到终态，
但其 `Get()` 为空且 `IsReady()` 为 false；Ready 后新做的不匹配 `CastTo<T>` 返回无效引用。
每次访问至多执行一次必要转换，当前不缓存调整后的子对象指针。

引用的复制、查询、移动和析构都只在 AssetManager 所在的 game thread 进行，计数不是原子的。
引用必须在 `AssetManager` 之前销毁；slot 随 manager 释放，之后不能再查询引用状态。

`StreamingAssetRefAny::GetAssetId()` 与 `StreamingAssetRef<T>::GetAssetId()` 按值返回资产 ID；
无效引用返回 `AssetId{}`。返回值独立于 slot 的生命周期，不借用 slot 或共享静态对象。

## AssetId

```cpp
AssetId MakeAssetIdFromPath(std::string_view namespacePrefix, const std::filesystem::path& path);
```

namespace prefix 隔离资产类型；同一路径在不同资产类型下必须产生不同 ID。路径归一化
使用 `weakly_canonical`，失败时依次退到 `absolute + lexically_normal` 和纯词法归一化，
再以 `generic_string` 作为哈希输入；Windows 下转小写，POSIX 下保留大小写。
归一化避免同一文件的不同路径写法落入重复 slot；类型 namespace 防止一份文件的不同资产表示
相互冲突。这是散文件身份规则，不把物理路径提升为 shader compiler 的逻辑 source identity。

AssetId 双轨并存（[asset-database](asset-database.md)）：入库资产以 `AssetDatabase` 登记的
GUID 为身份（一次分配、永不改变），散文件继续走这里的路径哈希；两轨共用 `AssetManager`
的单 slot 表，互不迁移。本地资产位于被忽略的顶层 `assets/`，通过源码仓库外的渠道分发；
shaderlib 与显式测试资源可使用路径哈希轨。

## GPU 使用者保活与保守迁移

零引用只在 S0 的候选批次卸载，Release 不就地调用任意 OnUnload。
StaticMesh 的合法 GPU 使用由 SceneWriter 绑定/退休 owner 或 GpuSystem::RetainForFrameGT 覆盖；
零引用后 OnUnload 直接释放 GpuMesh，不额外等一个无关 flight。sections 与 GPU view 共享同一个 owner。
独立 SceneWriter 同样遵守绑定协议；手工 draw 在 GT writable 阶段提交 keep-alive，RT 不访问非原子 ref。

已提交上传即使加载失败、请求取消或等待协程停止，资源也必须由框架 per-flight owner 保持到真实 completion。
用户 Wait 只提供完成通知，取消不代表 GPU 完成。原始 RHI 资源可以直接作为 RetainForFrameGT payload，
不必包装为 Asset。终止前必须 drain 已发布使用，未发布 owner 只能 terminal abandon。

TextureAsset 的外部 SRV 使用目前没有完整 producer 契约，保留 DeferDestroy 的保守等待作为明确迁移项。
新 GPU 资产应优先建立全部使用者 owner；未完成审计的旧类型继续整包 DeferDestroy，不用 AlreadySafe 标志跳过等待。
DeferDestroy 缺少 IWaitFrameProcessor 时保存 payload，等设施安装并 Pump 后再调度，绝不 log 后立即释放。
Application 始终把 FrameTimeline 接到 AssetManager。等待何时恢复见[帧边界等待](frame-and-gpu.md#帧边界等待)。
该保守路径只有 AssetManager 终止时才能取消内部等待，调用者必须事先完成 GPU drain。

## 现有资产

| 类型 | 内容 | 对外裸指针 |
|---|---|---|
| `ImageAsset` | CPU 像素数据 | 无 |
| `TextureAsset` | device-local texture、默认 SRV 和子 view 缓存 | `TextureView*` |
| `StaticMesh` | CPU mesh，以及共享的 `StaticMeshRenderData`（GPU mesh、sections、局部 bounds） | `const StaticMeshRenderData&`、`const GpuMesh&` |

返回资产内部裸指针的 API 必须在文档和调用方中同时说明持有 `StreamingAssetRef` 的要求。
StaticMeshComponent 保存 mesh ref，RenderSystem 中每个 SceneWriter 在 GT 为场景活跃资产持续持有类型无关的引用，
最后解绑后转入删除/改绑帧的退休列表，直到该帧真实 GPU completion 才释放；生命周期与借用规则见 [render-framework](render-framework.md#交付退出与资产保活)。
其他录制方须自行保存 owners 到 GPU 完成且 GT 可以安全释放的时刻。
render thread 不访问非原子的 refs；TextureAsset 的 GetOrCreateSrv/view cache 由调用方串行访问。
回收沿用零引用队列，不引入另一套资产引用计数。

`TextureAsset` 与 `StaticMesh` 均由构造方提供完整数据；两者的退休差异见上文。
内置网格/纹理 GPU 加载暂时移除，importer 返回明确失败，后续设计范围见
[资产 GPU 上传待设计](frame-and-gpu.md#资产-gpu-上传待设计)。

## 关停顺序

```text
WorldManager（全部 World）→ RenderSystem → AssetManager → FrameTimeline → AssetDatabase → GpuSystem
```

WorldManager 先销毁各 World，拆除组件与 asset ref，RenderSystem 释放常驻及退休资产引用、shader/program 与 RHI 缓存，AssetManager 再处理
剩余 slot 和延迟 payload。FrameTimeline 在 AssetManager 之后销毁，使在飞等待先收束。AssetDatabase 必须活过在飞 task，最后 GpuSystem 销毁 device。
完整时序见[关停顺序](frame-and-gpu.md#关停顺序)。
关停时仍有存活引用会记录错误并继续卸载，避免把后续 GPU 资源释放变成悬垂访问。

## 新增资产类型

1. 继承 `Asset` 并实现 `OnUnload`；创建和加载结果仍受 `Asset` 基类约束。
2. 只有其他协议确实需要独立稳定类型标识时，才为 `RuntimeTypeTrait<T>` 生成新 GUID；对象查询
   不需要 GUID，也不声明基类图。
3. 散文件写独占 namespace 的 `Make...AssetId`；入库类型实现 `AssetImporter` 并使用 manifest GUID。
4. 为每个 GPU 使用者建立完整 owner/fence 覆盖；未迁移的保守路径整包 `DeferDestroy`；纯 CPU 数据留给析构。
5. 在 `modules/runtime/tests/` 增加生命周期和 RTTI 视图测试，能不用 GPU 就不要创建 GPU。

## 测试

`AssetSlotTest` 覆盖引用计数唯一权威下的 slot 状态转换、加载去重和延迟回收边界，也用无 GUID
测试类覆盖基类、横向接口、多继承指针调整、虚继承、const 与不匹配视图。生产 `ImageAsset`
由静态库内部构造、最终测试程序查询的用例负责覆盖链接单元边界。
`test_asset_slot.cpp` 的 `ManualGate` 用于让异步 task 停在明确的恢复点；必须等待 gate，
不能直接拷贝 awaiter。它也覆盖 `IAssetSource` 的 ID/path 桥接；manifest 与 importer settings
由 `AssetDatabaseTest` 覆盖。
