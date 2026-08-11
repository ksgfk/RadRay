> - 适用: 资产生命周期、引用计数、加载去重或延迟 GPU 销毁
> - 权威: 本文是 runtime 资产系统的唯一说明；帧边界与上传见 `architecture/frame-and-gpu.md`
> - 锚点: `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/src/asset_manager.cpp`, `modules/runtime/include/radray/runtime/asset_bundle.h`, `modules/runtime/include/radray/runtime/asset_bundle_descriptors.h`, `modules/runtime/include/radray/runtime/shader_asset.h`, `modules/runtime/src/asset_bundle.cpp`, `modules/runtime/src/shader_asset.cpp`, `modules/runtime/include/radray/runtime/asset.h`, `modules/runtime/include/radray/runtime/texture_asset.h`

# 资产系统

`AssetManager` 是资产 slot、异步加载结果和引用计数的唯一拥有者。调用方持有
`StreamingAssetRef<T>`，不直接拥有 slot，也不能绕过引用计数强制释放资产。

## 生命周期

```text
Load request → AssetSlot::Loading → AssetManager::Pump → Loaded
                                               ↓
                              最后一份 StreamingAssetRef 归零
                                               ↓
                                  下一次 Pump 调用 Asset::OnUnload
```

加载去重按 `AssetId` 进行。dedup 命中时不会重新执行 loader，因此带 options 的 loader
必须在发起请求前检查参数；不能把一次请求的共享设施指针寄希望于第二次命中时更新。

`StreamingAssetRef<T>` 是类型安全的视图，内部仍由 `StreamingAssetRefAny` 参与计数。
引用必须在 `AssetManager` 之前销毁；slot 随 manager 释放，之后不能再查询引用状态。

## AssetId

`AssetId` 是 Manifest/Catalog 中的显式 `Guid`。Runtime 不从路径、内容或 BundleId 派生持久身份；
移动 locator、移动所属 Bundle 或切换 ShaderAsset 的 JIT/AOT 表示都不改变 ID。程序生成、测试和
外部系统已经构造好的资产仍可通过显式 ID 调用 `AddReady`，但不能重新引入路径 hash。

ADR-0036 的 Bundle/Catalog value model、严格 XML V1 source、同步挂载、`BundleRef`、typed
descriptor/loader dispatch 和结构化 Fault 已实现。Image、Texture、StaticMesh 与 ShaderAsset
均有 XML typed descriptor；Image/Texture 使用编码图片 payload，StaticMesh 使用受限的 `RRMESH01`
payload，ShaderAsset 的 AOT descriptor 在当前 runtime 返回 `CapabilityUnavailable`。

### Bundle Catalog（第一阶段）

`AssetManager::MountBundle(root, source)` 接收调用方选择的同步 `BundleCatalogSource`，将 root
绝对化并纯词法规范化，然后在所有 BundleId/AssetId 冲突检查通过后一次性发布 Catalog。Bundle
不拥有 payload storage；`BundleRef` 只保活 Catalog 和 root context，Catalog view 只在引用存活
期间有效。引用归零后由下一次 `AssetManager::Pump` 摘除 BundleId/AssetId 索引，不影响已有
Asset slot。

`MemoryBundleCatalogSource` 用于 synthetic source；`XmlBundleCatalogSource` 是当前 XML V1
实现。XML reader 是严格、有大小/条目/属性长度上限的无 DOM 子集：只接受 `bundle/schemaVersion=1`
和 `assets` 直接子元素，拒绝 DTD、外部实体、XInclude、其他 processing instruction 以及不支持
的实体。未知类型只保留 entry 公共字段并标为 `Unknown`；路径、TypeId 或 typed descriptor 错误
标为 `Invalid`，BundleId/AssetId 缺失、非法或重复则不发布 Catalog。该 reader 不是通用 XML API，
Catalog 仍不携带 XML DOM。

`RegisterBundleLoader`/`RegisterBundleLoaderSafe` 必须在首次 mount 前完成，按 entry `TypeId` 选择
loader；首次 mount 后注册表冻结。内置类型在 `AssetManager` 构造时注册。`LoadCatalog` 先查已有
slot，再查 Catalog。NotFound 和 RequestTypeMismatch 不创建 slot；Unknown、Invalid 和没有 loader
的 entry 创建 Faulted slot，并通过 `StreamingAssetRefAny::GetErrorCode/GetErrorMessage` 保留可查询
错误。内置 loader 走 `BundleAssetLoadData` 值快照，复制 locator、root 和 descriptor 后才创建 task，
不会把 Catalog entry 或 BundleRef 保存到异步任务中。

内置 descriptor/loader 目前覆盖 `ImageAsset`、`TextureAsset`、`StaticMesh` 和 `ShaderAsset`。Image
与 Texture 的 locator 指向 PNG/JPEG 编码字节；Texture/StaticMesh 需要 `GpuSystem` 装配的
`FrameUploadScheduler`。StaticMesh 的最小 Bundle payload 是小端 `RRMESH01`：8 字节 magic、
`version=1`、bin/primitive 数量、UTF-8 name、每个 bin 的 `uint64` 字节数与内容，随后是 primitive
及其 vertex/index 描述；loader 对总字节、数量、字符串和每个 buffer range 做上限与自洽性检查，
再复用既有 GPU 上传路径。ShaderAsset 的 `jit-source` 读取 locator 指向的 HLSL，JIT 服务由调用方
传入预配置的特殊 `shaderlib` include path；`aot-artifact` 只解码元数据表示并返回
`CapabilityUnavailable`，不做 JIT fallback。

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
World → RenderSystem → AssetManager → GpuSystem
```

World 先拆除 proxy 与 asset ref，RenderSystem 释放 render-side 对象，AssetManager 再处理
剩余 slot 和延迟 payload，最后 GpuSystem 销毁 device。关停时仍有存活引用会记录错误并继续
卸载，避免把后续 GPU 资源释放变成悬垂访问。

## 新增资产类型

1. 继承 `Asset`，实现 `OnUnload` 与 `GetTypeId`。
2. 为 `RuntimeTypeTrait<T>` 生成全新的 GUID，并声明 `Asset` 基类。
3. 写独占 namespace 的 `Make...AssetId` 和同步/异步 loader。
4. GPU 对象在 `OnUnload` 中整包交给 `DeferDestroy`；纯 CPU 数据留给析构。
5. 在 `modules/runtime/tests/` 增加生命周期测试，能不用 GPU 就不要创建 GPU。

## 测试

`AssetSlotTest` 覆盖引用计数唯一权威下的 slot 状态转换、加载去重和延迟回收边界。
`test_asset_slot.cpp` 的 `ManualGate` 用于让异步 task 停在明确的恢复点；必须等待 gate，
不能直接拷贝 awaiter。
