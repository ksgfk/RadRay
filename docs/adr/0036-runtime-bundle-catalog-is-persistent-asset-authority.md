# ADR-0036 Runtime Bundle Catalog 是持久资产权威

状态: 生效
日期: 2026-08
影响: `AssetManager`、全部持久资产 loader、AssetId 生成、Bundle Manifest/Catalog、ShaderAsset

## 背景

当前持久资产以物理路径派生 `AssetId`，loader 由调用方按路径组装。该模型让资产改名、移动或重新
分组时身份随位置变化，也没有一个 Runtime 可以直接挂载、枚举和诊断的持久资产目录。为每个源文件
增加 sidecar metadata 可以保存稳定 GUID，但会产生大量小文件，并把资产组织边界退化为单文件。

新的持久化格式需要同时满足：一个 Bundle 集中描述多个资产；Manifest 编码可替换；路径严格是
locator 而不是身份；部分条目损坏或类型不可用时仍可挂载和诊断；现有 `AssetManager` 的 slot、异步
终态与引用计数继续是资产生命周期唯一权威。ShaderAsset 只作为第一条垂直样例，不能反向把特殊
`shaderlib` JIT 根纳入资产系统。

## 决策

### 1. Bundle 是 Runtime 元数据目录，不是 storage owner

Runtime 通过 `AssetManager` 同步、原子地挂载 Bundle。挂载调用显式提供纯词法绝对化的 Bundle root
与一个 Catalog source；source 可以来自 XML、未来二进制、内存或其他介质，Manifest 本身不必位于
Bundle root。Manifest 不声明 root，只保存 `BundleId` 和相对 locator。V1 只实现严格版本化的 XML
source，未知 schema version 直接失败。

Catalog source 完整解析后交付不可变、编码无关的 Bundle Catalog，随后可以销毁。Bundle 只拥有
Catalog 元数据，不拥有或保活 locator 指向的目录、文件、映射或 archive storage；挂载不访问任何
payload。loader 在提交异步 task 前自行复制所需 descriptor 数据和 locator，之后不得借用 Bundle
或 Catalog。

### 2. BundleId 与 AssetId 都是显式持久 GUID

`BundleId` 表示逻辑 Bundle，`AssetId` 表示逻辑 Asset。两者都不从路径、内容或彼此派生；资产改名、
移动到其他 Bundle，或从 JIT 表示发布为 AOT 表示时保留身份。复制资产由制作工具生成新 AssetId。
Runtime 只读这些 ID，不补写缺失值。Bundle 不另设 instance id、revision 或可排序版本。

所有持久化、可加载资产必须来自已挂载 Bundle，ADR-0008 的路径派生身份因此被整体取代。程序生成、
测试或外部系统已经构造好的 Asset 仍可通过显式 AssetId 走 `AddReady`；不得以路径重新派生 ID。

### 3. Catalog entry 使用封闭公共字段和 typed descriptor

每个 entry 必须有 `AssetId`、稳定类型 GUID、一个 Bundle-local primary locator，以及可选的不可变
typed descriptor。XML 在 `<assets>` 下使用 `<shader>`、`<texture>` 等类型化直接子元素，同时写
人类可读标签与 `typeId`；已注册类型必须验证两者严格匹配。公共层不提供动态 metadata 字典，资产
依赖也属于 typed descriptor/loader。

已知且合法的条目为 `Valid`；未注册类型为 `Unknown`，只保留公共字段并丢弃专属 metadata；能够
识别 AssetId 但路径或已知 descriptor 非法的条目为 `Invalid`，保留结构化 diagnostics。三种状态
都进入全局索引并占用 AssetId。无法建立 BundleId/AssetId、schema 不支持、ID 重复，或与已挂载
Catalog/现有 slot 冲突时，整个新 Bundle 挂载失败；其余条目级错误不阻止 Bundle 挂载。

locator 使用 UTF-8 与 `/`，必须非空且相对；拒绝绝对路径、盘符、UNC、反斜杠、`.`、`..`、空组件
与大小写碰撞。检查只保证词法边界，不解析 symlink、junction 或其他 reparse point，也不保证目标
存在或属于某种文件类型。多个 AssetId 可以共享完全相同的 locator。

### 4. AssetManager 拥有公共职责，内部组件处理 Catalog

Bundle 挂载、Catalog 全局索引、类型 codec/loader dispatch 与按 AssetId 加载仍是 `AssetManager`
对外职责；实现内部使用独立组件，不形成第二个业务入口。Descriptor codec 与 loader registry 在
首次挂载前冻结。Bundle reference 以引用计数保持 Catalog 可查询，最后一份引用归零后在 `Pump`
中摘除 Catalog 与全局索引；Asset slot 不持有 Bundle reference。

Bundle 卸载不影响已经存在的 Loading、Ready、Faulted 或 Canceled slot。`Load` 先查 slot，再查
Catalog，所以 Catalog 消失后仍能命中存活资产；没有 slot 时才需要已挂载 Bundle。重新挂载包含
同一 AssetId 的 Bundle 前，旧 slot 必须已经回收。

Catalog TypeId 决定真实 loader，`Load<T>` 的 `T` 只做兼容断言。NotFound、调用方类型不兼容等
提交前错误不创建 slot；Unknown、Invalid 与实际 loader/payload 失败创建按 AssetId 去重的 Faulted
slot。Faulted slot 保存可查询的结构化错误与 diagnostics，而不是只写日志。

### 5. ShaderAsset 只冻结最小表示边界

一个 ShaderAsset 对应一个 Pass source unit，descriptor 必须从 `jit-source` 与 `aot-artifact` 中选择
恰好一种表示。V1 只实现 JIT source loader；AOT 是已知但 capability unavailable 的表示，不在本
决策中定义 payload、Variant coverage 或发布容器。JIT 只使用系统提供的特殊 `shaderlib` include
路径；`shaderlib` 本身不进入 Bundle 或资产系统。普通 Defines、Bundle-local includes 和 AOT
编排全部延期。

## 放弃的方案及代价

- **每资产一个 sidecar metadata**：移动单文件方便、版本控制冲突粒度小，但产生大量小文件，且不
  提供明确的 Bundle 挂载、诊断与所有权边界。
- **继续从路径派生 AssetId**：无需保存 GUID，但重命名和跨 Bundle 移动会改变身份，持久引用无法
  保持稳定。
- **让 XML 成为 Catalog 内部模型**：实现快，但未来二进制来源、loader 和 Runtime 查询都会泄漏
  XML DOM/字段语义。选择 source adapter 的代价是必须维护统一 Catalog value model。
- **未知类型使整个 Bundle 失败或被静默跳过**：前者让裁剪 Runtime 无法检查 Bundle，后者让声明过
  的 AssetId 被其他 Bundle 接管。保留 Unknown entry 的代价是无法验证未知标签与 TypeId 的关系，
  且其专属 metadata 必须丢弃。
- **Bundle 拥有 payload storage 或 Asset slot 持有 Bundle reference**：能把 IO 生命周期与挂载绑定，
  但会把 Catalog 元数据职责扩张为通用文件/archive ownership，并阻止加载提交后独立卸载 Bundle。
- **JIT/AOT 同时存在并运行时 fallback**：部署能力会改变同一 AssetId 的行为。互斥表示要求发布工具
  明确选择，但运行时语义稳定。

## 必须保持为真

- 持久资产的 AssetId 和 BundleId 都来自 Manifest 中的显式 GUID，不从路径或内容派生。
- 所有持久化 loader 先经已挂载 Catalog；`AddReady` 只接受调用方提供的显式 AssetId。
- Catalog source 交付后可以销毁，Catalog/loader 不依赖 XML 或其他来源对象的生命周期。
- 挂载不访问 payload；异步 loader task 不借用 Bundle、Catalog entry 或 descriptor 内存。
- locator 校验只实施统一词法规则，不宣称物理 containment 或 storage ownership。
- Unknown 与 Invalid entry 保留 AssetId 并参与全局冲突检查；无法建立身份时挂载原子失败。
- 任一新 Catalog AssetId 与已挂载 Catalog 或现有 slot 冲突时，整个新 Bundle 不发布。
- Bundle reference 归零只移除 Catalog，不取消或破坏已有 asset slot；Load 始终先查 slot。
- 类型不兼容的 `Load<T>` 请求不创建 Faulted slot；条目或 loader 自身失败才进入资产终态。
- ShaderAsset 的 JIT/AOT 表示互斥，特殊 `shaderlib` 根永不成为 Bundle entry 或 Bundle root。
