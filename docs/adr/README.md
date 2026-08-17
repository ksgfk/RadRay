> - 适用: 想知道某个设计"为什么不是另一种写法"
> - 权威: 本目录是决策与其放弃方案的唯一记录。**只追加，不修订**
> - 锚点: 无（本目录不描述代码现状）

# 决策记录（ADR）

## 这个目录存在的理由

代码注释里最长的那些段落，内容几乎都是"为什么选 X 不选 Y，以及曾经那条路的代价"。
这类信息价值高但**永不需要更新**——它是带日期的历史。放在代码里会让每次读函数都被迫重读一遍；
放在 `architecture/` 会让描述现状的文档掺进过去时叙述，然后开始时态混乱。

所以按**腐坏速度**分层：

- `guide/` 与 `architecture/` 描述现状，改代码就要改它们。
- `adr/` 记录决策，一旦写下就冻结。若决策被推翻，**新增**一条标记 `取代 ADR-XXXX` 的记录，
  并把旧记录的状态改为 `已被 ADR-YYYY 取代`（这是唯一允许的修订）。

## 怎么用

不要通读。按文件名 Glob 定位：

```
docs/adr/*.md
```

文件名格式 `NNNN-短横线标题.md`，编号只增不复用。

## 固定结构

每条 ADR 用同一套小节，便于快速跳读：

```markdown
# ADR-NNNN 标题

状态: 生效 | 已被 ADR-YYYY 取代
日期: YYYY-MM
影响: <受此决策约束的文件/子系统>

## 背景
是什么迫使必须做决策。

## 决策
一句话结论 + 必要的机制说明。

## 放弃的方案及代价
曾经走过或认真考虑过的路，以及它为什么不行。**这是本文档最有价值的部分。**

## 必须保持为真
违反即回归。写成可检查的条目。
```

## 现有记录

| 编号 | 标题 | 状态 |
|---|---|---|
| [0001](0001-gtest-discovery-pre-test.md) | gtest 测试发现钉死 PRE_TEST | 生效 |
| [0002](0002-shader-three-layer-split.md) | shader 系统拆成格式层 / 对象层 / 资产层 | 已被 ADR-0016 取代 |
| [0003](0003-manifest-is-abi-authority.md) | manifest 是 ABI 权威，反射只做核对 | 已被 ADR-0016 取代 |
| [0004](0004-content-addressed-shader-artifacts.md) | AOT 产物内容寻址，且与 manifest 同处一地 | 已被 ADR-0016 取代 |
| [0005](0005-keyword-groups-declared-in-hlsl.md) | keyword 组在 HLSL 里用 #pragma 声明 | 已被 ADR-0016 取代 |
| [0006](0006-shader-types-layer-boundary.md) | shader_types.h 的收录标准是"是不是 manifest 数据" | 已被 ADR-0016 取代 |
| [0007](0007-asset-lifetime-refcount-only.md) | 资产生命周期只由引用计数决定 | 生效 |
| [0008](0008-asset-id-path-normalization.md) | AssetId 由归一化路径派生 | 生效 |
| [0009](0009-deferred-destroy-hands-over-suspension.md) | 延迟销毁交出挂起点，不交对象 | 生效 |
| [0010](0010-rhi-ownership-model.md) | RHI 里 Device 共享，其余对象独占 | 生效 |
| [0011](0011-backend-selection-by-descriptor.md) | 后端由 descriptor variant 选定，不做运行期回退 | 生效 |
| [0012](0012-explicit-resource-state-transitions.md) | 资源状态转换全部显式，RHI 不跟踪状态 | 生效 |
| [0013](0013-vertex-stage-interface-projection.md) | vertex-stage artifact 保留最小输入接口投影 | 已被 ADR-0016 取代 |
| [0014](0014-cpp-trace-is-shader-source-of-truth.md) | shader 源真相是 C++ trace，绑定由 trace 产出 | 已被 ADR-0016 取代 |
| [0015](0015-variants-are-cpp-parameters.md) | 变体是 C++ 函数参数，烘焙集合也写在 C++ 里 | 已被 ADR-0016 取代 |
| [0016](0016-hlsl-and-radray-dxc-are-shader-authority.md) | HLSL 与 forked RadRay DXC 是 shader 权威 | 生效 |
| [0017](0017-runtime-lambert-sphere-example.md) | runtime Lambert sphere example 的接入边界 | 生效 |
| [0018](0018-filesystem-backed-shader-compilation.md) | filesystem-backed shader compilation 与 ABI/schema 断代 | 部分被 ADR-0019 取代 |
| [0019](0019-dxc-default-filesystem-include-search.md) | 使用 DXC 默认 filesystem include search | 已被 ADR-0020 取代 |
| [0020](0020-caller-supplied-filesystem-include-paths.md) | caller-supplied filesystem include paths | 部分被 ADR-0021 取代 |
| [0021](0021-jit-owns-immutable-include-path.md) | ShaderJit owning immutable include path | 已被 ADR-0022 取代 |
| [0022](0022-jit-owns-immutable-include-path-list.md) | ShaderJit owning immutable include path list | 生效 |
| [0023](0023-ordered-include-paths-follow-dxc-shadowing.md) | ordered include paths follow DXC shadowing | 生效 |
| [0024](0024-include-path-list-is-separate-borrowed-abi-input.md) | include path list is a separate borrowed ABI input | 生效 |
| [0025](0025-jit-keeps-convenience-error-surface.md) | JIT keeps a stateless convenience error surface | 生效 |
| [0026](0026-empty-include-path-list-is-valid.md) | empty include path list is valid | 生效 |
| [0027](0027-jit-include-path-list-is-explicit-construction-input.md) | JIT include path list is an explicit construction input | 生效 |
| [0028](0028-jit-owns-include-path-list-by-value.md) | JIT owns include path list by value | 生效 |
| [0029](0029-caller-stabilizes-include-tree-during-compile.md) | caller stabilizes include tree during compile | 生效 |
| [0030](0030-root-source-remains-memory-backed.md) | root source remains memory-backed | 生效 |
| [0031](0031-default-include-handler-per-invocation.md) | default include handler per invocation | 生效 |
| [0032](0032-discovery-include-validation-before-contract-scan.md) | discovery 先由 DXC 验证 include，再进行 root-only contract scan | 已被 ADR-0034 取代 |
| [0033](0033-include-path-abi-view-validation.md) | include path ABI view 的编码与输入校验 | 生效 |
| [0034](0034-clang-dxc-compiler-pipeline-is-the-only-shader-semantic-authority.md) | Clang/DXC compiler pipeline 是唯一 shader 语义权威 | 生效 |
| [0035](0035-optional-explicit-dxil-root-signature-and-rhi-fallback.md) | Optional Explicit DXIL Root Signature 与 D3D12 RHI fallback | 生效 |
| [0036](0036-per-bundle-manifest-is-asset-identity-authority.md) | per-bundle 单清单是资产身份权威 | 部分被 ADR-0039 取代 |
| [0037](0037-manifest-dom-is-backing-store.md) | 清单 DOM 常驻作为后备存储 | 已被 ADR-0038 取代 |
| [0038](0038-asset-metadata-in-lmdb.md) | asset 元数据改用 LMDB 存储 | 已被 ADR-0040 取代 |
| [0039](0039-abandon-bundle-organization-stabilize-asset-system.md) | 放弃 per-bundle 组织，先稳定 asset 系统 | 部分被 ADR-0041 取代 |
| [0040](0040-single-text-manifest-is-asset-identity-authority.md) | 单文本 JSON 清单是资产身份权威 | 生效 |
| [0041](0041-load-bridging-belongs-to-asset-manager.md) | 加载桥接归 AssetManager，资产来源经 IAssetSource 反转 | 生效 |
| [0042](0042-importers-are-interfaces-with-typed-settings.md) | 导入器是虚接口，导入设置强类型化 | 生效 |
