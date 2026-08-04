# ADR-0013 vertex-stage artifact 保留最小输入接口投影

状态: 生效
日期: 2026-08
影响: `ShaderVertexInterface`、shader artifact blob、DXIL/SPIR-V 反射、Vulkan device extension、`ShaderProgramVariant`

## 背景

后续把 primitive vertex layout 接到 PSO 时，需要知道**实际编译出的 vertex stage**要求哪些输入。
manifest `VertexInput` 描述作者提供给当前 PSO 的完整布局，但它不是某份编译产物的精确输入接口；
category、优化、stage variant 投影、source/entry point、shader model、pass defines 与 toolchain 都可能
改变实际 artifact。

把完整反射长期保存又会制造第二份资源绑定 ABI。完整反射包含大量与 primitive 连接无关的数据，
且不能替代 manifest 给出的 residency、immutable sampler、vertex format / slot / offset / stride 等
作者决策。[ADR-0003](0003-manifest-is-abi-authority.md) 因而只被收窄，不被推翻。

DXC 的两个输出也不对称。实测未使用的 vertex 参数在 DXIL input signature 中仍存在，
`ReadWriteMask == 0`；SPIR-V 反射基于 active interface，优化后会删除该参数。矩阵与数组在 DXIL
中又被展开为多个连续 `SemanticIndex`，在 SPIR-V 中仍保留聚合类型。这些差异不能靠一份
pass 级接口抹平。

## 决策

**每份精确 vertex-stage artifact 保留规范化的最小输入接口投影，不保存完整反射。**

`ShaderVertexInterface` 只含参数序列；每个参数只保留 `Semantic`、`SemanticIndex`、
`BackendLocation`、`ScalarType`、`BitWidth` 与 `ComponentCount`。它随 AOT blob 保存，JIT 也从同一份
精确编译结果提取，最终挂在 `ShaderBytecode` 上供 `ShaderProgramVariant` 只读访问。
vertex stage 的 optional 必须 engaged（允许参数为空），其他 stage 必须为 `nullopt`。
接口不进入 `index.json`，也不得跨 category、优化设置或 variant 投影借用、合并。

### SPIR-V semantic 与扩展依赖

SPIR-V 编译使用 `-fspv-reflect`，优先读取 `SpirvStageIo::HlslSemantic` 作为权威 semantic；
字段为空时，只允许从 `OpName` 对应的 `Name` 中剥掉严格前缀 `in.var.`。两个来源都不可用即提取
失败，`BuiltIn` 输入不进入接口。semantic 拆分尾部 index 后按 ASCII 大写规范化；SPIR-V 没有
独立 semantic-index 字段，无尾数字时 index 为 0。

`-fspv-reflect` 实测会同时写入 `OpExtension "SPV_GOOGLE_hlsl_functionality1"` 与
`OpExtension "SPV_GOOGLE_user_type"`。Vulkan 后端必须把对应的
`VK_GOOGLE_hlsl_functionality1`、`VK_GOOGLE_user_type` 都加入 device enabled extensions。
缺少任一扩展时 device 创建失败：所有仓库 SPIR-V 都无条件带这两条声明，创建一个随后无法合法
消费 shader module 的 device 不是可用降级。也不能把“驱动支持但创建 device 时未声明”当作
可用路径。

这里明确选择“启用 `-fspv-reflect`，取得权威 HLSL semantic”，而不是只依赖名字 fallback。
代价是引入上述 Vulkan extension 依赖，并改变全部 SPIR-V 字节码；已有产物必须重新 cook。
名字 fallback 仍保留，用于反射字段缺失时恢复 semantic。

保留 `Name` fallback 意味着**不得 strip SPIR-V `OpName`**。DXIL 提取依赖独立 reflection output，
vertex cook 即使关闭资源 ABI 校验也必须保留并读取它，**不得添加 `-Qstrip_reflect`**。

### 跨 category 一致性

只有 pass、source/entry point、vertex-stage variant 投影、`IsOptimize`、`ShaderModel`、pass defines
和 toolchain 全部相同的 DXIL/SPIR-V artifact 才能比较。比较键严格为
`(Semantic, SemanticIndex, ScalarType, ComponentCount)`，并同时断言：

```
SPIR-V ⊆ DXIL
{ p ∈ DXIL | p.ReadWriteMask != 0 } ⊆ SPIR-V
```

第一条容纳 SPIR-V active-interface DCE；它单独存在时几乎恒真，无法发现“实际使用的参数被
SPIR-V 提取漏掉”。第二条把实测的不对称反向约束起来。`ReadWriteMask` 只供该测试判断 DXIL
参数是否实际使用，不写进最小投影。两条都是一致性测试，不用于合并接口、生成 artifact key，
也不禁止只 cook 单一 category。

### location、bit width 与聚合类型

`BackendLocation` 按 category 分派语义：DXIL 保存 `HlslSignatureParameterDesc::Register`，
SPIR-V 保存 `OpDecorate Location`。两者处在不同编号空间；有 DCE 或聚合展开时数值可以不同，
所以不跨 category 比较，也不参与 artifact identity。location 允许空洞，`0, 1, 3` 必须原样保存，
不得重新编号。SPIR-V 内部仍要求 location 唯一。

`BitWidth` 只作后端事实的诊断，不进入跨 category 比较、primitive format 兼容判断或 artifact key。
实测在当前不传 `-enable-16bit-types` 的编译参数下，同一个 `min16float2` 输入在 DXIL 反射为
16-bit float，在 SPIR-V 中却是 32-bit `OpTypeFloat`；它们不是同一项可强制相等的逻辑 ABI。
本轮可序列化位宽限定为 16/32/64；SPIR-V `Int8` / `UInt8` stage input 明确拒绝。
`BackendLocation` 与 `BitWidth` 仍写入 payload，并由 `ContentHash` 做完整性保护。

SPIR-V 提取显式拒绝 `Columns > 1` 的矩阵和 `ArraySize != 0` 的数组。DXIL 侧无法做同样拒绝：
DXC 会把矩阵/数组静默展开成多个连续 `SemanticIndex`，其反射形状与作者手写多个 semantic 完全
相同，已没有信息可区分。因此只 cook DXIL 的配置不受该保护；这是已知且接受的洞，不用猜测式
规则拒绝合法的手写接口。

### blob 与当前消费边界

blob v1 不另存 `hasVertexInterface` bool。`stage` 是接口存在性的唯一真相：vertex payload 无条件
写 `parameterCount`（可为 0）及参数，非 vertex payload 不写接口段，然后都写嵌套 bytecode。
避免第二个 bool 就避免了“stage 与 bool 冲突”这一整类无意义损坏状态。

`ContentHash` 覆盖 header 的 `key` / `stage` / `category` 和完整 payload 序列化字节，因而同时保护
接口 metadata 与 bytecode，也能发现关键 header 被篡改。index 中的 `BytecodeHash` 仍只覆盖原始
bytecode，两个 hash 不合并。v1 首次部署与后续版本递增规则见
[ADR-0004](0004-content-addressed-shader-artifacts.md)。

本轮只生成、持久化、恢复并暴露投影，**不连接 primitive**。当前 PSO 仍消费 manifest
`VertexInput` → `ShaderVertexInputStorage`；未来才由 `ShaderVertexInterface + PrimitiveVertexLayout`
解析出 PSO vertex input。

### 异常遗留

本轮不把异常控制流扩大到调用层。SPIRV-Cross 适配层只把可恢复的 `CompilerError` 转成失败，
其他异常不在调用点吞掉。`spvc.cpp:198` 与 `spvc.cpp:371` 两处局部
`throw spirv_cross::CompilerError` 暂时保留，作为独立后续清理项；本 ADR 不把它们确立为新代码范式。

## 放弃的方案及代价

- **保存完整 reflection，运行时以它代替 manifest**。会复制资源绑定 ABI，却仍缺作者决定的
  vertex format、buffer layout 与 residency；两份真相无法可靠同步。
- **把接口放在 pass 上，所有 artifact 共用**。忽略 category、优化和 stage variant 投影造成的
  精确产物差异，缓存命中后可能拿到另一份字节码的接口。
- **跨 category 只断言 `SPIR-V ⊆ DXIL`**。DXIL 保留未使用参数、SPIR-V 删除它们，这条单向关系
  几乎天然成立，漏提取实际使用参数也可能通过。
- **把 DXIL Register 与 SPIR-V Location 当成统一 location 并压紧重排**。两者编号空间不同；
  重排还会销毁 DCE 留下的真实 location 空洞。
- **要求两个后端的 `BitWidth` 相等**。`min16float` 的实测反例说明它们记录的是不同物理结果，
  强行相等会拒绝当前正常编译路径。
- **DXIL 也按连续 semantic 猜测并拒绝矩阵/数组**。展开结果与合法手写声明不可区分，规则必然
  误伤；接受只 cook DXIL 时存在保护洞比制造假阳性更可控。
- **只依赖 `HlslSemantic` 并 strip 所有名字**。会移除既定 fallback，使旧工具链输出或缺字段的
  合法反射无法提取。
- **不加 `-fspv-reflect`，永远只剥 `in.var.` 名字**。能避免 Vulkan extension 依赖，但放弃了
  DXC 提供的权威 HLSL semantic；本轮接受扩展与重烘成本，不采用该退路。
- **payload 再存一个接口存在 bool**。它与 `stage` 重复，reader 反而要处理二者矛盾的状态。
- **本轮同时改 PSO/primitive 连接**。会把“产物能可靠携带接口”与“如何解析 mesh layout”两个
  独立问题绑在一次迁移中，无法分别验证。

## 必须保持为真

- `ShaderVertexInterface` 属于一份精确 vertex-stage artifact；vertex 必有（可为空），其他 stage
  必无，不上提为 pass 级数据，也不写入 `index.json`。
- SPIR-V semantic 优先 `HlslSemantic`，只以严格 `in.var.` 名字作 fallback；不得 strip `OpName`。
- 使用 `-fspv-reflect` 的 SPIR-V module 必须与实际启用的 `VK_GOOGLE_hlsl_functionality1`、
  `VK_GOOGLE_user_type` 配套；缺少任一扩展时拒绝创建 Vulkan device；
  DXIL vertex 提取不得 strip reflection。
- 同身份的跨 category 测试同时保持两条子集断言，且比较键不含 `BackendLocation` / `BitWidth`。
- location 空洞原样保存且不重编号；`BitWidth` 只作诊断，二者不影响 artifact key。
- SPIR-V 拒绝矩阵/数组；不得声称 DXIL 侧能可靠识别展开前形状，DXIL-only 保护洞保持显式。
- blob 不保存 `hasVertexInterface`；`ContentHash` 覆盖 `key` / `stage` / `category` 与完整 payload，
  `BytecodeHash` 只覆盖原始 bytecode。
- 当前 PSO 仍以 manifest `VertexInput` 为输入；在 primitive 连接完成前，不得让最小投影悄悄参与
  vertex layout 或 PSO key。
- `spvc.cpp` 的两处局部 `throw` 是已登记遗留，不得把“本轮已清除全部异常控制流”写成现状。
