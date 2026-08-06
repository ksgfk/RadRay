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
