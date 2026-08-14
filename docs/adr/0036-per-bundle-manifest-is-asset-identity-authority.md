# ADR-0036 per-bundle 单清单是资产身份权威

状态: 部分被 ADR-0039 取代
日期: 2026-08
影响: `modules/runtime` 资产持久化（`AssetBundleManifest` / `AssetDatabase`）、`bundle.xml` schema、AssetId 生成规则、`docs/architecture/asset-system.md`

## 背景

runtime 资产系统此前只有内存身份：`AssetId` 由 `MakeAssetIdFromPath` 从路径哈希派生
（ADR-0008），路径变了身份就变，无法支撑"移动文件仍是同一资产"和日后序列化的跨资产引用。
需要一套开发时持久化身份设施。业界主流是 Unity 式 per-file `.meta`：每个资产文件旁边落一个
元文件记录 GUID 与导入参数。必须决定持久化身份记录在哪、粒度多大、谁是权威。

## 决策

**资产按 bundle（资产根下含 `bundle.xml` 的目录）组织；每个 bundle 一份 XML 清单，
是 bundle 内所有资产 GUID、类型与相对路径的唯一权威。**

- 格式选 XML 的动机是 **git merge 冲突集中且易解**：所有身份变更集中在一个文本文件里，
  两人各加一个资产的 diff 是各自追加一行，冲突形态平凡（两侧都保留即可）。
- 目录名是 bundle 名的唯一权威，XML 内不写 name 属性——不留可以不一致的冗余状态。
  bundle 名 = 单一资产根下的相对目录路径。禁止 bundle 嵌套，发现即 Mount 硬失败。
- 条目元素名即资产类型（`<image guid="..." path="..."/>`）；清单层的身份契约只覆盖元素名、
  `guid`、`path` 三样，其余属性与子节点原样保留。子节点的语义归各资产 loader，
  值的编码形态遵循统一约定（见 ADR-0037）。
- **GUID 是永久身份，path 是可变元数据**：GUID 由 `Guid::NewGuid()` 在登记时一次分配，
  永不改变；移动/重命名 = 修改清单里的 `path`。不存在自动改 GUID 的代码路径。
- AssetId 双轨并存：入库资产以清单 GUID 为准；未入库散文件（shaderlib、测试资源）
  继续走 ADR-0008 的路径哈希，互不迁移。
- 错误分两级：结构性错误（嵌套 bundle、GUID 跨 bundle 重复、path bundle 内重复、
  非法 path 形态、坏 XML、version 不识别）Mount 整体硬失败；内容性缺损（type 无注册
  loader、文件缺失）warning 放行。

## 放弃的方案及代价

- **Unity 式 per-file meta**：并发修改几乎不冲突、移动文件时 meta 跟着走，但元文件散落
  全树、目录里一半是 `.meta`、批量重组资产时 diff 噪音大。单人开发下冲突频率本来就低，
  选单清单把 diff 集中换来了可读的变更历史；代价是清单成为并发修改的冲突热点，
  多人并行时要靠追加式写回（ADR-0037）压低冲突概率。
- **JSON 清单（现成 yyjson，零新依赖）**：功能等价，但 JSON 无注释、对手工维护的清单
  不友好，且逐行 merge 的鲁棒性不如 XML 元素级结构。为此引入 pugixml 一个小依赖。
- **XML 内记录 bundle name**：目录改名忘改 XML 就是一处不一致；删掉属性整类问题不存在。
- **路径哈希继续当唯一身份**：零新设施，但移动文件即换身份，跨资产引用永远做不了。
- **移动时自动内容哈希匹配保 GUID**：省去人改 path，但初版没有任何序列化引用消费方，
  为它付出内容哈希基建不值；身份规则"人改 path、GUID 不变"已覆盖需求。
- **多资产根**：现在只有一个根的真实场景；多根需要额外定义跨根 bundle 名冲突规则，
  会把"相对目录路径即 bundle 名"这条简单规则复杂化，等真实案例出现再设计。

## 必须保持为真

- 入库资产的 GUID 只在 `AddEntry` 登记时生成一次；任何代码路径都不得因移动、重命名、
  重新扫描而改变已有 GUID。
- 目录名是 bundle 名权威；`bundle.xml` 内不存在 name 属性。
- 清单层的身份契约只认条目的元素名、`guid`、`path`；Mount 不校验、不重写条目子节点。
  子节点的基础值编码约定与读写 helper 归 ADR-0037 管辖。
- 嵌套 bundle、GUID 跨 bundle 重复、path bundle 内重复（大小写不敏感）必须令 Mount
  整体硬失败且索引为空；未注册 type 与文件缺失只记 warning。
- path 存储形态：`/` 分隔、无绝对路径、无 `..`、无开头 `./`；违反即结构性错误。
- GUID 写回固定 D 格式（小写、无花括号）；读取用 `Guid::TryParse` 宽容 N/D/B/P，
  不使用会抛异常的 `Guid::Parse`。
- 散文件的 `MakeAssetIdFromPath` 轨道不迁移进 bundle 管辖。
