> - 适用: 资产生命周期、引用计数、加载去重或延迟 GPU 销毁
> - 权威: 本文是 runtime 资产系统的唯一说明；帧边界与上传见 `architecture/frame-and-gpu.md`，开发时身份登记与 AssetDatabase 见 `architecture/asset-database.md`
> - 锚点: `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/src/asset_manager.cpp`, `modules/runtime/include/radray/runtime/asset.h`, `modules/runtime/include/radray/runtime/texture_asset.h`

# 资产系统

`AssetManager` 是资产 slot、异步加载结果和引用计数的唯一拥有者。调用方持有
`StreamingAssetRef<T>`，不直接拥有 slot，也不能绕过引用计数强制释放资产。

## 生命周期

```text
Load request / source task → AssetSlot::Loading → AssetManager::Pump → Loaded
                                               ↓
                              最后一份 StreamingAssetRef 归零
                                               ↓
                                  下一次 Pump 调用 Asset::OnUnload
```

加载去重按 `AssetId` 进行。dedup 命中时不会重新执行 loader，因此带 options 的 loader
必须在发起请求前检查参数；不能把一次请求的共享设施指针寄希望于第二次命中时更新。

加载有两种入口，共用同一张 slot 表：显式 `Load(AssetLoadRequest)` 直接提交 task；
`Load(AssetId)` / `Load<T>(path)` 经可选 `IAssetSource` 取得 task。source 未装配或未命中时返回
无效引用，不创建 faulted slot。`AssetDatabase` 是当前 source 实现，细节见
`architecture/asset-database.md`。

`StreamingAssetRef<T>` 是类型安全的视图，内部仍由 `StreamingAssetRefAny` 参与计数。
引用必须在 `AssetManager` 之前销毁；slot 随 manager 释放，之后不能再查询引用状态。

## AssetId

```cpp
AssetId MakeAssetIdFromPath(std::string_view namespacePrefix, const std::filesystem::path& path);
```

namespace prefix 隔离资产类型；同一路径在不同资产类型下必须产生不同 ID。路径归一化
使用 `weakly_canonical`，失败时依次退到 `absolute + lexically_normal` 和纯词法归一化，
再以 `generic_string` 作为哈希输入；Windows 下转小写，POSIX 下保留大小写。

AssetId 双轨并存（`architecture/asset-database.md`）：入库资产以 `AssetDatabase` 登记的
GUID 为身份（一次分配、永不改变），散文件继续走这里的路径哈希；两轨共用 `AssetManager`
的单 slot 表，互不迁移。`assets/assets.json` 与 `example_lambert_sphere` 已实际消费 GUID 轨；
shaderlib 与显式测试资源继续使用路径哈希轨。

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
例如 SceneProxy 自己保存 mesh ref，材质快照保存 texture ref 加描述值，不能只保存裸 view。

## 关停顺序

```text
World → RenderSystem → AssetManager → AssetDatabase → GpuSystem
```

World 先拆除 proxy 与 asset ref，RenderSystem 释放 render-side 对象，AssetManager 再处理
剩余 slot 和延迟 payload；AssetDatabase 必须活过在飞 task，最后 GpuSystem 销毁 device。
关停时仍有存活引用会记录错误并继续卸载，避免把后续 GPU 资源释放变成悬垂访问。

## 新增资产类型

1. 继承 `Asset`，实现 `OnUnload` 与 `GetTypeId`。
2. 为 `RuntimeTypeTrait<T>` 生成全新的 GUID，并声明 `Asset` 基类。
3. 散文件写独占 namespace 的 `Make...AssetId`；入库类型实现 `AssetImporter` 并使用 manifest GUID。
4. GPU 对象在 `OnUnload` 中整包交给 `DeferDestroy`；纯 CPU 数据留给析构。
5. 在 `modules/runtime/tests/` 增加生命周期测试，能不用 GPU 就不要创建 GPU。

## 测试

`AssetSlotTest` 覆盖引用计数唯一权威下的 slot 状态转换、加载去重和延迟回收边界。
`test_asset_slot.cpp` 的 `ManualGate` 用于让异步 task 停在明确的恢复点；必须等待 gate，
不能直接拷贝 awaiter。它也覆盖 `IAssetSource` 的 ID/path 桥接；manifest 与 importer settings
由 `AssetDatabaseTest` 覆盖。
