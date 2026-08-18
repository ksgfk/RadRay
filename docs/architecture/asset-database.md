> - 适用: 开发时资产身份登记、`assets.json` schema、importer/settings、按路径加载或 `Refresh`
> - 权威: 本文是当前 JSON `AssetDatabase` 的唯一现状说明；资产 slot 与引用生命周期见 `architecture/asset-system.md`
> - 锚点: `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/include/radray/runtime/asset_source.h`, `modules/runtime/include/radray/runtime/texture_asset.h`, `modules/runtime/include/radray/runtime/static_mesh.h`, `modules/runtime/src/asset_database.cpp`, `modules/runtime/src/texture_asset.cpp`, `modules/runtime/src/static_mesh.cpp`, `modules/runtime/src/application.cpp`, `examples/example_lambert_sphere/example_lambert_sphere.cpp`

# 开发时资产数据库

`AssetDatabase` 把 `<AssetRoot>/assets.json` 全量读入普通 C++ 结构并维护两张内存索引：

```text
AssetId (manifest GUID) → AssetEntry
lowercase canonical path → AssetId
```

这份 JSON 是一个资产根内的身份与导入元数据权威；没有 LMDB、本地数据库或导入产物缓存。
仓库当前把整个 `assets/` 目录列入 `.gitignore`，没有跟踪项目级 manifest 或资产文件；调用方若要
消费 GUID 轨，必须另行提供资产根。`AssetRoot` 由装配方通过 `ApplicationRuntimeDescriptor`
传入，空路径表示不启用数据库。
清单不存在时 `Open` 返回空库，之后可以 `AddEntry` + `Save` 建立；不存在的资产根也可由首次
`Save` 创建，但 `Refresh` 要求根目录已经存在。

## Manifest Schema

当前版本固定为 1：

```json
{
  "version": 1,
  "assets": [
    {
      "guid": "d21480ee-f0c8-4fb1-9a9d-cf9147b10842",
      "path": "wall.png",
      "type": "texture",
      "settings": {"srgb": true, "generateMips": true}
    }
  ]
}
```

根对象只能有 `version` 与 `assets`。条目的 `guid`、`path`、`type` 是必需字符串，`settings`
可选。GUID 读取使用 `Guid::TryParse`，接受 N/D/B/P 格式但拒绝空 GUID；写出始终是小写 D 格式。
`version != 1` 不做迁移，直接拒绝打开。

`Save` 按 `path` 排序全量重写清单，不保留条目顺序或额外根内容。它先完整写入同目录临时文件，
再原子替换 `assets.json`，写失败不会先截断身份权威。已解析的 settings 由强类型对象重新序列化；
未注册 type、无 settings 形状的 type、或 settings 解码失败时，原始 JSON **值片段**保存在
`RawSettings`，包括内部空白与数字拼写，并在保存时逐字写回。默认 typed settings 解码器也拒绝
未知或重复字段，使旧版本工具面对未来 schema 时走 RawSettings，而不是静默删字段。

`RawSettings` 由独立的语法跳读器按字节区间截取，因为 yyjson 的 `JsonValue` 是解析后的 view，
恢复不出原始空白与数字拼写。这条保真依赖 `ReadTextFile` 按二进制读取：文本模式会把 CRLF 折成
LF，跨行 settings 因此被改写并以 LF 存回，在 CRLF 工作树里表现为整段无意义 diff。

## Path 口径

存储形态是资产根相对、`/` 分隔、词法规范化的非空路径：

- 禁止 `\`、`:`、绝对根、`..`、`.`、空段与尾斜杠。
- path 必须是合法 UTF-8。唯一性与查询在 Windows 上按 invariant Unicode lowercase key 判断；
  其他平台至少固定 ASCII 大小写折叠，不受进程 locale 影响。
- `AddEntry`、`SetPath` 与 path 查询的输入较宽容：接受 `\`、重复分隔符和 `.` 段，先转成
  canonical 形态；绝对路径、盘符和任何 `..` 仍拒绝。
- `SetPath` 只改 path，GUID 永不改变；`RemoveEntry` 才移除身份。

`Find` 返回指向表内 `AssetEntry` 的指针。`unordered_map` rehash 不使它失效，删除对应条目会。
`ResolvePath(entry)` 只做 `AssetRoot / entry.Path`，不会重新分配身份。

## 错误分级

`Open` 的结构错误返回 `nullptr` 并填写 `outError`，不以异常表达预期失败：

- JSON 语法或根 schema 错误、未知版本；
- 必需字段缺失或类型错误；
- GUID 无效、为空或重复；
- path 非 canonical，或按大小写不敏感口径重复。

内容缺损只记 warning，条目仍进入索引：

- 源文件不存在，或存在性无法判定（权限、路径过长等 IO 错误，与"确实不存在"分别记录）；
- `type` 没有注册 importer；
- settings 无法按该 importer 的强类型形状解码。

这使“先提交清单、后补文件”和新旧工具版本交错仍可工作，同时不放过任何身份歧义。

## Importer 与 Settings

`AssetImporter` 是由数据库独占的虚接口，声明稳定的 type 名、`Refresh` 认领的扩展名、settings
工厂和加载入口。依赖在 importer 构造时注入，`AssetLoadContext` 只有绝对路径与 settings 指针。

`AssetImportSettings` 只暴露 `GetTypeInfo()` 作为类型判定出口，向下转换一律经 `IsA`。它不像
`Asset` 那样另设 `GetTypeId()`——那里的 `GetTypeId` 有存在理由（`AssetManager` 用它与 loader
声明的 `TypeInfo` 交叉校验），settings 这侧没有对应校验点，多一个出口只会变成每个子类都要覆写
却无人读取的平行事实。

有 settings 的 importer 继承 `TypedAssetImporter<TSettings>`。它在普通（非协程）的 `Load`
中同步检查并复制 context，再把 `std::filesystem::path` 与 `TSettings` 按值传给真正的惰性 task。
因此 task 挂起后不会回查可能已被修改或删除的 `AssetEntry`。自定义 `AssetImporter::Load` 也必须
保持这条同步读取契约。

默认 importer：

| type | 扩展名 | settings | 加载路径 |
|---|---|---|---|
| `texture` | `.png`, `.jpg`, `.jpeg` | `TextureImportSettings{Srgb, GenerateMips}` | 解码 RGBA8；可在 CPU 生成完整 mip 链（sRGB 在 linear 空间过滤）；经 `FrameUploadScheduler` 上传为 `TextureAsset` |
| `mesh` | `.obj` | 无 | `WavefrontObjReader` → `TriangleMesh` → `MeshResource` → `LoadStaticMesh` |

OBJ importer 只覆盖当前 reader 能表达的单文件三角面模型；不导入材质、子资产或跨资产引用。

## 加载桥接

依赖方向是：

```text
AssetManager → IAssetSource ← AssetDatabase
```

`AssetManager::Load(AssetId)` 向 source 请求 `task<AssetLoadResult>`，随后仍进入原有的单 slot 表；
`Load<T>(relPath)` 先经 `ResolveId` 找 manifest GUID。source 未装配、路径/ID 未登记或 type 无
importer 时记 error 并返回无效引用，不 abort。原有 `Load(AssetLoadRequest)` 保持不变，供散文件
和显式 loader 使用。

`Refresh` 递归扫描资产根：扩展名被 importer 认领且尚未登记的文件调用 `AddEntry`；已登记但
文件缺失的条目与 GUID 原样保留。它不自动 `Save`，连续扫描同一路径不会重分配 GUID。

## 装配与关停

`Application` 在 `GpuSystem` 与 `AssetManager` 已创建后构造默认 importer 并调用 `Open`。
数据库是可选设施，不进入 `ServiceRegistry`；`Wire()` 后由 `SetAssetSource` 手工注入。
打开失败只记 error，应用仍保留显式 `AssetLoadRequest` 路径。

关停顺序固定为：

```text
World → RenderSystem → AssetManager → AssetDatabase → GpuSystem
```

数据库持有 importer 与 settings，必须活过 manager 对在飞加载 task 的取消和收束；GPU 上传依赖
又要求 `GpuSystem` 最后销毁。`example_lambert_sphere` 以 `assets/` 为资产根，通过
`Load<TextureAsset>("wall.png")` 持有贴图引用并绑定到 Lambert 球面；只有调用方提供包含该条目的
manifest 与源贴图时这条端到端路径才可运行，clean clone 当前会在该加载点失败退出。

## 测试

`AssetDatabaseTest` 覆盖 schema/path 硬失败、GUID 格式、双索引、强类型与原始 settings、排序
保存、重开一致性和 `Refresh` GUID 稳定性。`AssetSlotTest` 覆盖 `IAssetSource` 的 ID/path 加载、
source 缺失和 slot 去重；两组均不需要 GPU。example 的 D3D12/Vulkan 运行用于验证真实上传与绑定。
该手工运行目前要求外部准备未跟踪的 `assets/assets.json` 与 `wall.png`。
