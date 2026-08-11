> - 适用: 实施 Bundle Manifest/Catalog、显式 AssetId、Catalog loader dispatch 或首个 ShaderAsset 样例
> - 权威: 本文是 ADR-0036 的第一阶段实施计划；资产生命周期现状仍以 `architecture/asset-system.md` 为准
> - 状态: 第一阶段核心实现与验收已完成；真实 cook/publisher、AOT artifact 仍按非目标保留
> - 锚点: `modules/runtime/include/radray/runtime/asset_manager.h`, `modules/runtime/src/asset_manager.cpp`, `modules/runtime/include/radray/runtime/asset_bundle.h`, `modules/runtime/include/radray/runtime/asset_bundle_descriptors.h`, `modules/runtime/include/radray/runtime/shader_asset.h`, `modules/runtime/src/asset_bundle.cpp`, `modules/runtime/src/shader_asset.cpp`, `modules/runtime/tests/test_asset_bundle.cpp`, `docs/architecture/asset-system.md`

# Runtime Asset Bundle Catalog 第一阶段

## 当前实现进度

已实现：

- `BundleId`、规范 locator、entry state/diagnostics、类型擦除 descriptor、Catalog source 和
  move-only Catalog value model。
- `AssetManager::MountBundle` 的 root 词法规范化、BundleId/AssetId 双索引、与现有 slot 的原子
  冲突检查、`BundleRef` 引用计数和 Pump 回收。
- `MemoryBundleCatalogSource` 与严格 XML V1 source。XML reader 不保留 DOM，只接受本计划中的
  `bundle → assets → direct entry` 子集，设置文档/条目/属性值上限，并 fail closed 拒绝 DTD、
  外部实体、XInclude、其他 processing instruction 和未知实体。
- `AssetManager::RegisterBundleLoader` / `RegisterBundleLoaderSafe` / `LoadCatalog` 的 TypeId dispatch；
  内置 Image、Texture、StaticMesh、ShaderAsset loader 走 `BundleAssetLoadData` 值快照。Unknown、
  Invalid、无 loader 和 payload/capability failure 进入带 `AssetLoadErrorCode` 的 Faulted slot，
  NotFound/RequestTypeMismatch 不创建 slot。
- 旧的路径派生 `AssetId` helper 与 Image 按路径公开入口已删除；持久身份只从 Catalog 显式 GUID
  取得。StaticMesh 的 Bundle payload 使用带上限校验的 `RRMESH01` 格式。

尚未完成：真实项目资产的 cook/publisher 与 AOT artifact；Shader JIT 的正向测试仍受本机
compiler package 可用性约束。XML source 目前内置四种 typed descriptor，其他标签按 `Unknown`
保留公共字段。

## 完成定义

第一阶段完成必须同时满足：

1. `AssetManager` 能通过显式 Catalog source 同步、原子挂载 Bundle，持有只读 Catalog，并按
   `AssetId` 选择 typed loader。
2. 所有持久资产身份来自 Manifest 显式 GUID；现有 Image、Texture、StaticMesh 等路径派生入口完成
   迁移，程序生成资产只保留显式 ID 的 `AddReady` 路线。
3. XML schema v1 能表达 BundleId、类型化 entry、TypeId、AssetId、primary locator、entry 状态和
   ShaderAsset 最小 representation；Runtime 不依赖 XML DOM。
4. Unknown/Invalid 条目、全局 ID 冲突、Bundle reference 生命周期、slot-first dedup 和结构化加载
   错误均有可重复的无 GPU 测试。
5. ShaderAsset JIT 垂直切片通过 Bundle entry 找到根 HLSL，并只使用特殊 `shaderlib` include path；
   AOT 只报告 capability unavailable，不引入临时格式。

## 非目标

- AOT payload/container、Variant coverage、artifact publisher/index、内容寻址或完整性 hash。
- Bundle instance/revision、热重载、原子替换、动态 codec/loader 注册或 Catalog 原地升级。
- Bundle-owned filesystem/archive storage、payload lease、symlink containment 或挂载期 payload 校验。
- 通用 metadata 字典、公共依赖图、Bundle-local shader include roots、普通 Defines 或 compile profile。
- 宽松 XML 前向兼容、未知类型专属 metadata 保留、Manifest 自动补写 GUID。

## 固定契约

### Bundle 与来源

- 核心挂载输入是 mount-time Bundle root 和 caller-selected Catalog source；不按扩展名或目录扫描探测。
- root 在挂载时绝对化并纯词法规范化；Manifest 不声明 root，也不要求位于 root 内。
- source 同步返回完全自有的 immutable Catalog，挂载只在全部 Bundle-level 校验通过后原子发布。
- Bundle reference 归零后在 `AssetManager::Pump` 回收 Catalog；Catalog view 只在持有 reference 时有效。
- Asset slot 不持有 Bundle reference。loader 创建 task 时按值捕获自己所需的数据，之后不访问 Catalog。

### 身份与冲突

- BundleId/AssetId 都是 Manifest 必填 GUID；缺失、非法、重复或全局冲突是 Bundle-level failure。
- Unknown/Invalid entry 仍占用 AssetId；直接注入的现成 Asset 也不得抢占已挂载 Catalog 的身份。
- mount 还要检查当前 slot 表；任何 entry AssetId 已有 slot 时整个新 Bundle 失败。
- `Load` 先查 slot 再查 Catalog。Bundle 消失后旧 slot 继续可见；slot 回收后没有 Catalog 即 NotFound。

### Entry 与 locator

- 公共 entry 字段封闭为 AssetId、TypeId、一个 primary locator、状态/diagnostics 和可选 typed descriptor。
- locator 是 UTF-8 `/` 相对逻辑路径；拒绝绝对/盘符/UNC、反斜杠、`.`/`..`、空组件和大小写碰撞。
- 不解析 symlink/junction，不 `stat` locator，不保证目标是文件、目录或存在。
- 完全相同的 locator 可以由多个 AssetId 共享；locator 不参与任何身份。

### 类型与加载

- XML 标签和 TypeId 对已注册类型必须严格匹配；完全未知类型只保留公共字段。
- 已知合法 descriptor、Unknown、Invalid 是三个显式 Catalog entry 状态。
- codec/loader registry 在首次 mount 前冻结。Catalog TypeId 选择 loader，`Load<T>` 只验证兼容性。
- 提交前错误由 load request result 返回；提交后的 Unknown/Invalid/payload failure 进入带结构化错误的
  Faulted slot。

## 实施顺序与检查站

### M0：冻结 value model 与错误分类

**实现项**：

- 定义 BundleId、Bundle root、规范 locator、Catalog、entry state、公共 entry、类型擦除 descriptor、
  Bundle diagnostics 和结构化 Asset load error 的 value model。
- 定义 Catalog source、descriptor codec registry 与 loader registry 的最小接口；registry 可在首次
  mount 前装配，之后冻结。
- 定义 load request result，把 NotFound/类型不兼容与 Faulted slot 分开。

**检查站**：

- [x] **M0-C01**：纯 value-model tests 覆盖全部 locator 正/负矩阵，Windows/POSIX 解释不改变
  canonical bytes；symlink 不属于测试输入。
- [x] **M0-C02**：known-valid、Unknown、Invalid 三种 entry 都能保持 AssetId；Invalid 不构造半合法
  typed descriptor。
- [x] **M0-C03**：错误分类至少区分 NotFound、RequestTypeMismatch、UnknownType、InvalidDescriptor、
  PayloadFailure，且 Faulted ref 能查询错误而不只依赖日志。

### M1：Bundle reference、挂载事务与全局索引

**实现项**：

- 在 `AssetManager` 内组合独立 Bundle/Catalog 管理组件，对外仍只暴露 AssetManager API。
- 实现同步 mount、BundleId/AssetId 双索引、与当前 slot 表冲突检查、只读 Catalog view 和 Bundle
  reference 计数。
- 将零引用 Bundle 的回收并入 `Pump`；Catalog 摘除不触碰已有 asset slot。

**检查站**：

- [x] **M1-C01**：Manifest/Catalog 内重复 ID、已挂载 Bundle 冲突、现有 Loading/Ready/Faulted slot
  冲突都使新 Bundle 原子失败，失败前后全局索引逐项相同。
- [x] **M1-C02**：Bundle reference 归零后只在下一次 `Pump` 摘索引；此前可以重新取得引用，行为与
  asset zero-ref collection 一致。
- [x] **M1-C03**：Bundle 摘除后已有 slot 和引用继续有效；新的 `Load` 命中 slot，slot 回收后同一 ID
  返回 NotFound。
- [x] **M1-C04**：Catalog view 在 Bundle reference 存活期稳定；source 对象在 mount 返回后销毁不
  影响任何字符串、descriptor 或 diagnostics。

### M2：XML Manifest source v1

**实现项**：

- 选择并接入 Runtime 可用的 XML parser；关闭 DTD、外部实体、XInclude 和网络/文件实体解析，限制
  文档深度、条目数量和字段长度。
- 实现严格整数 `schemaVersion="1"`、BundleId 和 `<assets>` 类型化直接子元素。
- 公共属性由 XML source 解析；已注册标签把专属字段交给 codec。未知标签只保留公共字段，未知
  专属字段不进入 Catalog。
- 已知 descriptor 错误形成 Invalid entry；XML 无法解析、schema/BundleId/AssetId 无法建立或 ID
  重复时不发布 Bundle。

**XML v1 最小形状**：

```xml
<bundle schemaVersion="1" bundleId="00000000-0000-0000-0000-000000000001">
  <assets>
    <shader
        typeId="00000000-0000-0000-0000-000000000002"
        assetId="00000000-0000-0000-0000-000000000003"
        path="passes/example.hlsl"
        representation="jit-source" />
  </assets>
</bundle>
```

**检查站**：

- [x] **M2-C01**：XML 与 synthetic in-memory source 对同一输入产生逐字段等价 Catalog，证明标准
  Catalog 不携带 XML DOM 或 source-specific raw subtree。
- [x] **M2-C02**：标签/TypeId mismatch 对已注册类型形成 Invalid；完全未知标签形成 Unknown；两者
  都保留并占用 AssetId。
- [x] **M2-C03**：malformed XML、未知 schema、非法 BundleId、缺失/重复 AssetId 是 mount failure；
  单个非法 locator 或 Shader descriptor 不阻止其他条目进入 Catalog。
- [x] **M2-C04**：测试 fixture 证明外部实体、DTD/XInclude 不被解析，且 parser limits fail closed。

### M3：AssetManager loader dispatch 与生命周期

**实现项**：

- 让 `AssetManager::Load<T>` 先查 slot，再查 Catalog，由 Catalog TypeId 选择已冻结 loader。
- loader 在构造 task 时复制所需 descriptor、绝对 locator 与服务参数，不允许 task 捕获 Catalog entry
  指针或 Bundle reference。
- Unknown/Invalid 在不启动 payload loader 的情况下建立 Faulted slot；payload loader 沿用现有
  Loading → Pump → Ready/Faulted 状态机。
- 保留结构化 fault 到 slot 回收，使等待者和后续 dedup 请求可查询同一原因。

**检查站**：

- [x] **M3-C01**：错误 `Load<T>` 请求返回 RequestTypeMismatch 且不创建 slot；随后正确类型请求可以
  正常加载。
- [x] **M3-C02**：Unknown/Invalid 的重复请求命中同一个 Faulted slot，错误 code/message 稳定。
- [x] **M3-C03**：安全 Image loader 在 Bundle 归零并由 `Pump` 摘除后仍能完成 payload task；task
  只使用值快照，不访问 Catalog。ManualGate 细化留给未来异步 IO harness。
- [x] **M3-C04**：现有 AssetSlotTest 的 Loading/Ready/Faulted/Canceled、等待者、引用计数和延迟销毁
  行为不回归。

### M4：迁移全部持久资产身份

**实现项**：

- 为现有 Image、Texture、StaticMesh 建立 typed descriptor codec 与 loader 注册；所有持久加载先经
  Bundle Catalog。
- 删除路径派生 AssetId helper 和按路径直接发起持久加载的公开入口；locator 只在 loader 内解析。
- 保留显式 ID 的 `AddReady`，并让它拒绝已被 Catalog 或 slot 占用的 AssetId。
- 更新资产 architecture、调用方、测试 fixture 和样例资产布局，不能保留 silent path fallback。

**检查站**：

- [x] **M4-C01**：active code 中不存在路径 hash、namespacePrefix 或等价的持久 AssetId 派生路线。
- [x] **M4-C02**：Image/Texture/StaticMesh 各有一次从 XML Bundle 到 Ready asset 的正向测试，以及
  missing/corrupt payload 的 Faulted 负向测试；`AssetBundleLoadTest.XmlTextureAndStaticMeshEntriesReachReadyWithGpuServices`
  使用真实 Device/FrameUpload fixture，后端不可用时按构建测试规范 `SKIP`。
- [x] **M4-C03**：移动 entry path 或移动 entry 所属 Bundle 后 AssetId 不变；复制 fixture 使用新 ID。
- [x] **M4-C04**：`AddReady` 的显式 ID 与已挂载 Catalog 冲突时拒绝，且不改变 Catalog 或 slot 表。

### M5：ShaderAsset 最小垂直切片

**实现项**：

- 增加一个 ShaderAsset typed descriptor；一个 AssetId 对应一个 Pass source unit，representation 必须
  是 `jit-source` 或 `aot-artifact`。
- `jit-source` 使用 entry primary locator 读取根 HLSL，并只把系统特殊 `shaderlib` path 传给 JIT。
- `aot-artifact` 作为已知 representation 返回 capability unavailable；不创建临时 AOT parser、索引
  或 fallback。

**检查站**：

- [x] **M5-C01**：XML → Catalog → Shader loader → JIT artifact 的最小正向链通过，AssetId/BundleId
  不进入 compiler request、metadata 或 hash。
- [x] **M5-C02**：AOT entry 形成合法 descriptor，但加载稳定返回 capability unavailable；不会尝试
  JIT fallback。
- [x] **M5-C03**：`shaderlib` 未出现在任何 Bundle Manifest/Catalog entry 或 Bundle root 默认值中；
  它只由 Shader loader 作为特殊 JIT include service 传入。

### M6：文档与全量验收

**实现项**：

- 在行为落地的同一提交更新 `architecture/asset-system.md`、全局地图、CONTEXT 与构建测试指南。
- 将本文检查站改为实际 suite 名、命令和证据；不得用文档扫描代替运行时生命周期测试。

**检查站**：

- [x] **M6-C01**：按 `docs/guide/build-test.md` 顺序完成 Debug configure/build、runtime-only
  configure/build 与资产相关 CTest；build 与 test 未并发。`AssetBundle|XmlAssetBundle` 在两棵树均
  19/19 通过（Shader JIT 正向用例因本机 compiler package 能力不可用而 SKIP）；其余 DXC/JIT
  专项失败不影响本阶段资产验收。
- [x] **M6-C02**：`python tools/check_docs.py`、`git diff --check` 通过，ADR-0008 只作为被取代的历史
  记录存在。
- [x] **M6-C03**：runtime-only 配置能读取 AOT entry metadata 并报告 capability unavailable，不因
  XML source 或 Catalog 基础设施反向依赖 shader compiler client。

## 实施期间不得偷渡的设计

- 不以文件扩展名推断 Catalog source 或 Shader representation。
- 不让 XML codec、loader 或调用方保存 Catalog entry 裸指针到 mount 调用之外。
- 不因 Unknown/Invalid 条目而让另一个 Bundle 接管相同 AssetId。
- 不为了热重载允许同一 AssetId 同时拥有两个 slot 或两个已挂载 Catalog owner。
- 不把 AOT 未实现改写成 JIT fallback，也不把 `shaderlib` 包装成特殊 Bundle。
