# ADR-0008 AssetId 由归一化路径派生

状态: 已被 ADR-0036 取代
日期: 2026-07
影响: `MakeAssetIdFromPath`（`modules/runtime/src/asset.cpp`）；全部 `MakeXxxAssetId`

## 背景

`AssetId` 是落盘/去重缓存的 key，跨进程有效。绝大多数资产的天然身份是它的文件路径。

需要保证两个方向：**同一份文件必须得到同一个 id，不同文件必须得到不同 id。**

## 决策

`MakeAssetIdFromPath(namespacePrefix, path)`。

**`namespacePrefix` 做资产类型的命名空间隔离**（`"shader"` / `"image"` / ...）：同一路径
在不同资产类型下必须得到不同 id，否则一份 `*.png` 既当 `ImageAsset` 又当 `TextureAsset`
时会撞进同一个 slot。

**路径先归一化再哈希。这是正确性要求，不是优化。** 归一化口径：

1. `weakly_canonical` —— 消掉 `.` / `..` 并解 symlink。与 shader 源码身份的计算口径一致
   （那里同样用 `weakly_canonical`），故两侧对"同一个文件"的判断不会分叉。
   失败时（盘符不可用、权限不足）退到 `absolute + lexically_normal`，再失败退到纯词法
   归一化——**兜底必须是确定的**，不能让 id 依赖于当时的 IO 结果。
2. `generic_string` —— 分隔符统一为 `/`。
3. Windows 下转小写。NTFS 路径大小写不敏感，而 `weakly_canonical` **不**做这层归一化，
   于是 `"C:/Foo/x"` 与 `"c:/foo/x"` 仍是两个 id。**POSIX 下刻意不转**：那里大小写是
   显著的，转了会把两个真实不同的文件合并。

## 放弃的方案及代价

- **不归一化，直接哈希原始路径字符串**。`"a/../b/x"` 与 `"b/x"` 得到两个 id 却指同一个
  文件，于是同一份 manifest 被建成两个资产，各自持有一套 `PipelineLayout` 与字节码缓存。
  症状是"shader 编了两遍、layout 缓存命中率莫名减半"，**且没有任何报错**。
- **只做词法归一化（`lexically_normal`），不解 symlink**。与 shader 源码身份的口径分叉：
  同一个文件在两侧被判为不同，AOT 产物查找会莫名失效。
- **兜底时抛异常或返回失败**。`AssetId` 的计算必须是纯函数——同一路径在任何时刻都要给出
  同一个 id。让它依赖 IO 成功与否会让 id 变得不确定。
- **不做命名空间隔离，靠资产类型在 slot 里区分**。slot 表按 id 索引，撞了就是撞了，
  第二个类型永远拿不到自己的槽位。
- **POSIX 下也转小写**（为了跨平台一致）。会把 `Foo.png` 与 `foo.png` 这两个在 Linux 上
  真实共存的文件合并成一个资产。

## 必须保持为真

- 所有 `MakeXxxAssetId` 都经 `MakeAssetIdFromPath`，各自用独占的 namespace 前缀。
- 归一化的三步顺序不变，且 `weakly_canonical` 失败时的兜底链是确定的。
- 小写转换只在 Windows 上做。
- 与 shader 源码身份（`ComputeShaderSourceIdentity`）保持同一套路径归一化口径。
- 注意"同一个文件得到同一个 id"说的是同一份文件的多种写法（`"a/../b/x"` 与 `"b/x"`）。
  源码树与输出目录里的两份真实副本得到不同 id 是**正确的**，不要试图归一化掉。
