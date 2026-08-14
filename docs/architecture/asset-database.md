> - 适用: AssetDatabase 的现状契约
> - 权威: 本文是开发时资产身份登记的唯一现状说明；决策 rationale 见
>   `adr/0038-asset-metadata-in-lmdb.md`（LMDB 存储）与
>   `adr/0039-abandon-bundle-organization-stabilize-asset-system.md`（放弃 per-bundle 组织，
>   身份规则保留自 ADR-0036），生命周期与加载调度仍归 `architecture/asset-system.md`
> - 锚点: `modules/runtime/include/radray/runtime/asset_database.h`, `modules/runtime/src/asset_database.cpp`, `modules/core/include/radray/lmdb.h`, `CMakeLists.txt`

# 开发时资产身份登记（LMDB 存储与 AssetDatabase）

`AssetDatabase` 是开发时资产身份的登记门面。**运行时 asset 元数据的唯一权威是 LMDB 的
`assets` 表**（key = `AssetId`，value = header + data 段）。本轮**无 XML 落盘**：资产来源
不绑定 bundle，`AddEntry` 逐个登记、`Resolve` 按 GUID 解析。`AssetDatabase` 不接触
`AssetManager`，加载桥接待后续轮次重建。

## path 口径

`path` 是相对工程根、词法规范化后的路径：`/` 分隔、无 `\`、无盘符（含 `:`）、
非绝对路径（无开头 `/`）、无 `..`、无 `.` 段、无空段、无尾斜杠。它只是 value header 里的
元数据，**本轮不校验唯一性**（库按 GUID 键序，无 path→guid 反查）。

`AddEntry` 对输入宽容：接受 `\` 与 `/` 混用、重复分隔符与 `.` 段的可解析 relPath，
内部规范化成存储形态再写入；绝对路径、盘符与 `..` 仍硬拒绝。

## AssetId 双轨与身份规则

- 入库资产：GUID 由 `AddEntry` 调用 `Guid::NewGuid()` 一次分配，写入后永不改变。
- 散文件（shaderlib、测试资源）：继续 `MakeAssetIdFromPath` 路径哈希（ADR-0008），不迁移。
- 移动/重命名 = 人改 path，GUID 不变；不存在任何自动改 GUID 的代码路径。
- 两轨共用 `AssetManager` 的单一 slot 表，互不迁移。

## LMDB 运行时存储（ADR-0038）

一个 `AssetDatabase` = 一个 LMDB 库（environment），落盘为调用方传入 `storePath` 的单文件
（`MDB_NOSUBDIR`）。析构只关库、不删文件，数据持久到下次打开。库内一张 `assets` 命名表：

- **key** = `AssetId`（16 字节 GUID）。
- **value** = header + data 段：

```text
header = [u32 headerLen][u32 typeLen][type][u32 pathLen][path][u32 dataLen]
value  = header + dataLen 字节的 data 段
```

四个长度字段均小端 `u32`，`headerLen` 含自身。header 的 `type` / `path` 是存储层
机器可读、可查询的固定字段；**data 段对存储层完全 opaque**，编码由资产 loader 自行决定
（本轮 `AddEntry` 登记的条目 data 段为空）。

运行时查询：`Resolve` 读 LMDB 的 `assets` 表（一次 `O(log N)` 键查找 + 反序列化）。

## AssetDatabase API

```cpp
// 构造打开 storePath 处的 LMDB 工作库并记录 assetRoot (归一为绝对路径)。任一步失败抛
// std::runtime_error。不可拷贝/移动。
AssetDatabase(const std::filesystem::path& assetRoot, const std::filesystem::path& storePath);

struct ResolvedAsset {
    std::filesystem::path AbsolutePath;  // assetRoot / path
    string Type;                          // 资产类型
    vector<byte> Data;                    // 不透明 data 段 (拥有式拷贝, 本轮为空)
};
std::optional<ResolvedAsset> Resolve(const AssetId& id);              // 读 LMDB, 非 const
std::optional<AssetId> AddEntry(std::string_view relPath, std::string_view type);
```

`Resolve` 读 LMDB 因而非 `const`。`ResolvedAsset` 的 `Type` / `Data` 是拥有式拷贝，不依赖
任何 LMDB 事务的存活期。`AddEntry` 不校验 path 唯一性，重复 path 会各自分配新 GUID；失败
经 `RADRAY_ERR_LOG` 记日志并返回 `nullopt`。

## 依赖

- **LMDB**（`third_party/lmdb`，OLDAP-2.8 许可，`project_manifest.json` 声明、
  `tools/fetch_third_party.py` 拉取）由根 `CMakeLists.txt` 自建 `lmdb` 目标（编译纯 C 源
  `mdb.c` + `midl.c` + `module.c`，上游只随附 Makefile）。`radraycore` 以 `radray/lmdb.h` 薄封装
  （`LmdbEnvironment` / `LmdbTransaction` / `LmdbCursor`，公开头只前置声明 `MDB_*` 类型），
  PRIVATE 链接 `lmdb`；详见 `architecture/core-facilities.md`。
