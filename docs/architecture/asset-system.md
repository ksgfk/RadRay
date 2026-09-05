> - 适用: 资产生命周期、引用计数、加载去重或延迟 GPU 销毁
> - 权威: 本文是 runtime 资产系统的唯一说明；帧边界与上传见 [frame-and-gpu](frame-and-gpu.md)，开发时身份登记与 AssetDatabase 见 [asset-database](asset-database.md)
> - 锚点: `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/src/asset_manager.cpp`, `modules/runtime/include/radray/runtime/asset.h`, `modules/runtime/include/radray/runtime/asset_source.h`, `modules/runtime/include/radray/runtime/texture_asset.h`, `modules/runtime/include/radray/runtime/static_mesh.h`, `modules/runtime/src/static_mesh.cpp`, `modules/runtime/include/radray/runtime/render_framework/static_mesh_scene_proxy.h`

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
的单 slot 表，互不迁移。`example_lambert_sphere` 通过 `AssetManager` 消费 GUID 轨，但其运行资产
属于被忽略的顶层 `assets/`，由源码仓库之外的渠道分发。shaderlib 与显式测试资源继续使用
路径哈希轨。

## 延迟销毁

资产的 `OnUnload` 只负责把仍可能被 GPU 使用的对象交给 `AssetManager::DeferDestroy`：

```cpp
void MyAsset::OnUnload(AssetManager& manager) override {
    manager.DeferDestroy([resource = std::move(_resource)]() mutable {});
}
```

纯 CPU 资产不需要延迟销毁。GPU payload 整包交出，成员声明或捕获顺序表达销毁顺序；
非资产持有者应等待 `IWaitFrameProcessor` 的帧边界，而不是调用资产回收接口。

## 现有资产

| 类型 | 内容 | 对外裸指针 |
|---|---|---|
| `ImageAsset` | CPU 像素数据 | 无 |
| `TextureAsset` | device-local texture、默认 SRV 和子 view 缓存 | `TextureView*` |
| `StaticMesh` | CPU mesh、sections、bounds 和 GPU mesh | `GpuMesh::DrawData*` |

返回资产内部裸指针的 API 必须在文档和调用方中同时说明持有 `StreamingAssetRef` 的要求。
SceneProxy 保存 mesh ref，Material authoring 保存 texture ref 加描述值。PrepareFrame 通过
proxy `CollectAssetReferences` 和 Material `BuildRenderData` 把 owners 追加到 RenderSystem 的
当前 flight retained vector；pipeline input 只保存 geometry/texture raw pointers 和复制值。
render thread 不访问 refs，TextureAsset 的 GetOrCreateSrv/view cache 由 render thread 串行访问。
当前 flight GPU 完成且 game thread 取得该 flight 后清理 retained refs，随后 Pump 按原有零引用
规则回收资产；不引入显式 Unload、release message 或另一套引用计数。

OBJ `MeshImporter` 在 GPU 上传前为每个 `MeshPrimitive` 建一个覆盖完整 index range 的默认 section，
并从 `POSITION0` 计算 local bounds；任一步不自洽都使加载失败。`StaticMeshSceneProxy` 自持一份
`StreamingAssetRef<StaticMesh>`，所以它暴露的 section `MeshDrawArgs::Geometry` 在 proxy 生命周期内
稳定，组件重建 render state 时旧 proxy 与其引用一起释放。

## 关停顺序

```text
World → RenderSystem → AssetManager → AssetDatabase → GpuSystem
```

World 先拆除 proxy 与 asset ref，RenderSystem 释放 render-side 对象，AssetManager 再处理
剩余 slot 和延迟 payload；AssetDatabase 必须活过在飞 task，最后 GpuSystem 销毁 device。
关停时仍有存活引用会记录错误并继续卸载，避免把后续 GPU 资源释放变成悬垂访问。

## 新增资产类型

1. 继承 `Asset` 并实现 `OnUnload`；创建和加载结果仍受 `Asset` 基类约束。
2. 只有其他协议确实需要独立稳定类型标识时，才为 `RuntimeTypeTrait<T>` 生成新 GUID；对象查询
   不需要 GUID，也不声明基类图。
3. 散文件写独占 namespace 的 `Make...AssetId`；入库类型实现 `AssetImporter` 并使用 manifest GUID。
4. GPU 对象在 `OnUnload` 中整包交给 `DeferDestroy`；纯 CPU 数据留给析构。
5. 在 `modules/runtime/tests/` 增加生命周期和 RTTI 视图测试，能不用 GPU 就不要创建 GPU。

## 测试

`AssetSlotTest` 覆盖引用计数唯一权威下的 slot 状态转换、加载去重和延迟回收边界，也用无 GUID
测试类覆盖基类、横向接口、多继承指针调整、虚继承、const 与不匹配视图。生产 `ImageAsset`
由静态库内部构造、最终测试程序查询的用例负责覆盖链接单元边界。
`test_asset_slot.cpp` 的 `ManualGate` 用于让异步 task 停在明确的恢复点；必须等待 gate，
不能直接拷贝 awaiter。它也覆盖 `IAssetSource` 的 ID/path 桥接；manifest 与 importer settings
由 `AssetDatabaseTest` 覆盖。
