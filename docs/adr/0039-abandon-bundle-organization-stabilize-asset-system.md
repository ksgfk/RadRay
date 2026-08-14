# ADR-0039 放弃 per-bundle 组织，先稳定 asset 系统

状态: 生效
日期: 2026-08
影响: `modules/runtime` 的 `AssetDatabase` / `AssetManager`、asset 身份登记形态、`docs/architecture/asset-database.md`、`docs/architecture/asset-system.md`

## 背景

ADR-0036 引入 per-bundle 组织（含 `bundle.xml` 的目录是 bundle 内资产身份的唯一权威），
ADR-0037 以 DOM 常驻保证写回保序，ADR-0038 把运行时存储换成 LMDB，但把「per-bundle 组织
与 bundle 身份」明确留待后续轮次。实际推进中，bundle 组织带来的概念——bundle 目录、bundle
名权威、嵌套检测、per-bundle 清单、`FindByPath(bundleName, …)`、`SaveBundle`——始终没有任何
消费方：运行时加载走 `AssetManager` 的 slot 表，身份登记只需要一套可靠的 GUID → 元数据
存取。bundle 与 XML 清单/快照在当前阶段是纯负担而非刚需。必须决定 asset 系统当前阶段的
方向：继续补完 bundle 组织，还是砍掉它、先把 asset 系统本身做稳定。

## 决策

**放弃 ADR-0036 的 per-bundle 组织；asset 身份登记收敛为纯 GUID 键序的 LMDB 库，当前阶段
先稳定 asset 系统（`AssetManager` 生命周期 + `AssetDatabase` 的 LMDB 存取）。**

- 不再有 bundle 目录、`bundle.xml`、bundle 名权威、嵌套检测、per-bundle 清单权威这些概念，
  也不存在 `FindByPath(bundleName, …)` / `SaveBundle` 等代码路径。ADR-0036 的 bundle 组织
  部分作废。
- asset 身份登记收敛为 ADR-0038 的简化形态：一个 LMDB 库（environment）、一张 `assets` 表、
  纯 GUID 键序。`path` 降级为 value header 里的元数据（相对工程根的规范化路径），本轮不
  校验唯一性、无 path→guid 反查。
- 保留 ADR-0036 的身份规则：GUID 是永久身份（`AddEntry` 一次分配、永不改变），path 是
  可变元数据（移动/重命名 = 人改 path），AssetId 双轨并存（入库 GUID + 散文件路径哈希）
  互不迁移。
- 当前阶段目标是稳定 asset 系统：`AssetManager` 的生命周期/加载去重/延迟销毁
  （ADR-0007/0009）与 `AssetDatabase` 的 LMDB 存取先做到可靠可测。XML 落盘快照、加载桥接
  （`LoadFromDatabase`）、bundle 组织等留待有真实消费方后再设计，届时另立 ADR。

## 放弃的方案及代价

- **继续补完 per-bundle 组织**：保留 ADR-0036 的完整形态，但 bundle 目录、清单、嵌套检测、
  保序写回这一整套在没有任何消费方的阶段是纯成本；为它继续付出概念与测试会拖慢 asset 系统
  本身（生命周期、加载去重）的收敛。
- **保留 XML 落盘快照（ADR-0038 的「XML 降级为序列化格式」）**：快照形态（根元素、字段、
  触发时机）都还没定，且它的原始动机是 bundle 清单的 merge 友好落盘——bundle 放弃后这条
  动机消失，快照留到有持久化需求时再定。
- **把身份登记并入 `AssetManager`**：单点更少，但 `AssetManager` 职责是生命周期/加载，
  身份登记是独立的开发时设施；分开才能各自稳定，混在一起会互相拖累。
- **为未定的跨资产引用提前设计 bundle**：引用消费方还不存在，提前定 bundle 会把
  「目录名权威」等规则绑死到尚无验证的形状上。

## 必须保持为真

- 不存在 bundle 目录、`bundle.xml`、bundle 名权威、嵌套检测、per-bundle 清单权威的任何
  代码路径；`AssetDatabase` 不包含 `FindByPath(bundleName, …)` / `SaveBundle`。
- asset 身份登记 = 一个 LMDB 库的 `assets` 表，纯 GUID 键序；`path` 只是 value header 元数据，
  不校验唯一性、无 path→guid 反查。
- GUID 仍是永久身份：入库资产只在 `AddEntry` 时 `Guid::NewGuid()` 一次分配；任何代码路径都
  不得因移动、重命名、重扫而改变已有 GUID。
- AssetId 双轨并存互不迁移：入库资产走 GUID，散文件走 `MakeAssetIdFromPath`。
- `AssetDatabase` 不接触 `AssetManager`，加载桥接留待后续轮次。
