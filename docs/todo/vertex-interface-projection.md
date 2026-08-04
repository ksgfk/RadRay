> - 适用: 实施「vertex-stage artifact 保留最小输入接口投影」这一轮改动
> - 权威: 本文保留实施清单；长期设计约束以 ADR-0013 与 shader pipeline 架构文档为准
> - 状态: 已实施（2026-08）。公共 projection 的后续方向已由 `backend-specialized-shader-lanes.md` 取代
> - 锚点: `modules/shader/include/radray/shader/shader_manifest.h`, `modules/shader/src/shader_manifest.cpp`,
>   `modules/shader/src/shader_reflection_map.h`, `modules/shader/src/spvc.cpp`,
>   `modules/runtime/include/radray/runtime/shader_program.h`, `modules/shader/src/dxc.cpp`

# vertex-stage 输入接口投影

## 目标闭环

```
DXC 编译
  -> DXIL/SPIR-V 反射
  -> ShaderVertexInterface
  -> blob payload
  -> AOT/JIT ShaderBytecode
  -> ShaderProgramVariant 只读访问
```

当前渲染路径不变，仍是 `manifest VertexInput -> ShaderVertexInputStorage -> PSO`。
新接口本轮不参与渲染，只保证未来连接 primitive 时数据已完整可靠。

---

## 0. 开工前的阻断项

### 0.1 `-fspv-reflect` 需要 Vulkan 侧配套改动（**必须先解决**）

已裁决给 SPIR-V 加 `-fspv-reflect`，让 `SpirvStageIo::HlslSemantic` 有权威来源。
实测该 flag 会往模块里加两行：

```
OpExtension "SPV_GOOGLE_hlsl_functionality1"
OpExtension "SPV_GOOGLE_user_type"
```

而 `vulkan_impl.cpp:3272` 起的 device extension 列表原先没有对应的
`VK_GOOGLE_hlsl_functionality1` / `VK_GOOGLE_user_type`。
本机 `vulkaninfo` 确认 RTX 3060 / Vulkan 1.4.325 驱动支持这两个扩展（`VK_GOOGLE_hlsl_functionality1`
与 `VK_GOOGLE_user_type` 都在列），但"驱动支持"不等于"未声明也合法"：
`vkCreateShaderModule` 要求模块用到的每个 `OpExtension` 都由已启用的 device extension 覆盖，
否则是 validation error（部分驱动会放过，validation layer 会报）。本仓库启用了
`VK_LAYER_KHRONOS_validation`（`vulkan_impl.cpp:2976`），所以会被抓到。

**必须先做**：在 `vulkan_impl.cpp` 的 device extension 列表加上
`VK_GOOGLE_HLSL_FUNCTIONALITY_1_EXTENSION_NAME` 与 `VK_GOOGLE_USER_TYPE_EXTENSION_NAME`。
两者都是所有仓库 SPIR-V 的硬依赖，缺少任一扩展时拒绝创建 Vulkan device。

**然后**才能在 `_BuildCompileArgs`（`dxc.cpp:911`）里加 `-fspv-reflect`。

**若不愿动 Vulkan 后端**：退回"不加 flag"方案，`HlslSemantic` 永远为空，剥 `in.var.`
前缀成为唯一生效路径，并保留"不得 strip SPIR-V OpName"这条约束。这个退路是完整可行的，
提取代码的优先级逻辑两种方案下都一样，只是其中一条分支永不命中。

> 决策点：动 Vulkan 后端（加扩展 + 加 flag），还是退回不加 flag。
> 加 flag 会改变全部 SPIR-V 字节码 → 已烘产物失效，但本轮已要求重烘，正好一起。

### 0.2 清理孤儿产物

`build_debug/_build/Debug/` 里有 `test_vertex_iface_probe.exe` 与
`test_vertex_iface_probe2.exe`，仓库里没有对应源文件，CMakeLists 里也没注册。
这是此前试探留下的孤儿（`build**` 被 gitignore，所以 `git status` 干净）。
开工前删掉，否则 `ctest` 的 PRE_TEST discovery 可能撞上它们。

### 0.3 先写探针测试，再写提取逻辑

在实现提取之前，先用手工构造的 `HlslShaderDesc` / `SpirvShaderDesc` 把下面三种形状
钉成测试（都是实测过的真实 DXC 行为，见第 1 节）：

- 未使用的顶点参数：DXIL 保留（`ReadWriteMask == 0`），SPIR-V 删除
- `float3x4`：DXIL 展开成 4 个连续 `SemanticIndex`，SPIR-V 是一个 `mat3v4float`
- `float4[2]`：DXIL 展开成 2 个 `SemanticIndex`，SPIR-V 是一个 `ArraySize=2`

否则会先写完提取再发现两边不对称，然后返工提取逻辑和 ADR。

---

## 1. 实测确立的事实

用 `SDKs/dxc/v1.9.2602.24/extracted/bin/dxc.exe` 实测，全部结论有输出支撑。
这些是后续所有设计决定的依据，**不要凭直觉推翻**。

### 1.1 DXIL 保留未使用参数，SPIR-V 删除

```hlsl
struct VertexInput { float3 Position : POSITION0; float3 Unused : NORMAL0; float2 UV : TEXCOORD0; };
// 只用 Position 与 UV
```

```
; DXIL -O3
; POSITION   0  xyz  reg 0  Used: xyz
; NORMAL     0  xyz  reg 1  Used: (空)      <- 仍在 signature 里
; TEXCOORD   0  xy   reg 2  Used: xy

// SPIR-V -O3
OpEntryPoint Vertex %VSMain %in_var_POSITION0 %in_var_TEXCOORD0 %gl_Position   <- NORMAL0 消失
OpDecorate %in_var_POSITION0 Location 0
OpDecorate %in_var_TEXCOORD0 Location 2                                        <- location 空洞 1
```

原因：`ReflectSpirv` 在 `spvc.cpp:513` 用 `get_active_interface_variables()` + `removeInactive`
主动删掉 inactive stage input。DXIL 侧无此行为。

推论：`SPIR-V ⊆ DXIL` **恒成立**，单向断言对"提取逻辑写错"没有区分度。
`HlslSignatureParameterDesc::ReadWriteMask`（`hlsl.h:300`）就是那个 `Used` 列，用它做反向断言。

### 1.2 矩阵与数组：DXIL 展开，SPIR-V 不展开

```hlsl
struct VertexInput { float4 UV[2] : TEXCOORD0; float3x4 Xform : TRANSFORM; float3 Position : POSITION0; };
```

```
; DXIL -O3
; TEXCOORD    0  xyzw  reg 0
; TEXCOORD    1  xyzw  reg 1     <- 数组展开成 2 个 SemanticIndex
; TRANSFORM   0  xyz   reg 2
; TRANSFORM   1  xyz   reg 3     <- 矩阵展开成 4 个(!), 按列拆成 4 个 float3
; TRANSFORM   2  xyz   reg 4
; TRANSFORM   3  xyz   reg 5
; POSITION    0  xyz   reg 6

// SPIR-V -O3
OpDecorate %in_var_TEXCOORD0 Location 0   %_arr_v4float_uint_2   <- ArraySize=2
OpDecorate %in_var_TRANSFORM Location 2   %mat3v4float           <- Columns=3, VectorSize=4
OpDecorate %in_var_POSITION0 Location 5
```

DXIL 侧展开后的 `TRANSFORM0..3` 与**手写**的 `TRANSFORM0..3` 在反射里完全同形，
无法区分，因此机械拒绝只能发生在 SPIR-V 侧。**已裁决：只在 SPIR-V 侧拒绝，ADR 登记 DXIL 侧的洞。**

### 1.3 `Register` 与 `Location` 在有空洞时不同

`arr.hlsl` 里 `POSITION0` 的 DXIL `Register = 6`、SPIR-V `Location = 5`。
两者是不同的编号空间，不可互换、不可跨 category 比较。

### 1.4 `BitWidth` 在两个 backend 上是不同的物理事实

```hlsl
struct VertexInput { min16float2 H : TEXCOORD5; };   // 不带 -enable-16bit-types
```

```
; DXIL:   TEXCOORD 5  xy  fp16                                    <- FLOAT16, 16 bit
// SPIR-V: %in_var_TEXCOORD5 = OpVariable %_ptr_Input_v2float      <- OpTypeFloat 32
```

`_BuildCompileArgs`（`dxc.cpp:911`）**从不加** `-enable-16bit-types`，所以这是常态而非边缘情况。
`ScalarType` 两边仍一致（都是 Float），所以比较键安全。
这是 `BitWidth` 必须排除在比较键外的**真实理由**，ADR 里要写这个，不要写含糊的"降级为诊断"。

### 1.5 SV_* 系统值确实出现在 DXIL input signature 里

```
; SV_VertexID   0  x  reg 5  VERTID  uint  x
```

必须靠 `IsSystemSemantic` 过滤。SPIR-V 侧对应的是 `BuiltIn` 有值，靠它过滤。

### 1.6 semantic 拼写原样保留

DXC 不改 HLSL 里的 semantic 拼写。`shaderlib` 里全是大写（`POSITION0`/`NORMAL0`/`TEXCOORD0`），
所以"大写规范化"这条只能用**手工构造的 reflection** 测，不能用真实编译产物测。

---

## 2. 数据类型

在 `shader_manifest.h` 的 artifact 数据区新增：

```cpp
enum class ShaderVertexScalarType : uint8_t {
    Unknown,
    Float,
    SInt,
    UInt,
};

struct ShaderVertexParameter {
    string Semantic;
    uint32_t SemanticIndex{0};
    /// 【按 category 分派语义】DXIL 是 HlslSignatureParameterDesc::Register,
    /// SPIRV 是 OpDecorate Location。两者是不同编号空间, 有空洞时数值不同,
    /// 故不跨 category 比较, 也不参与任何 hash 判定。
    uint32_t BackendLocation{0};
    ShaderVertexScalarType ScalarType{ShaderVertexScalarType::Unknown};
    uint8_t BitWidth{0};
    uint8_t ComponentCount{0};

    friend bool operator==(
        const ShaderVertexParameter&,
        const ShaderVertexParameter&) noexcept = default;
};

struct ShaderVertexInterface {
    vector<ShaderVertexParameter> Parameters;

    friend bool operator==(
        const ShaderVertexInterface&,
        const ShaderVertexInterface&) noexcept = default;
};
```

字段来源：

| 字段 | DXIL | SPIR-V |
|---|---|---|
| `Semantic` | 归一化后的 HLSL semantic（大写） | 同 |
| `SemanticIndex` | suffix 优先，反射字段兜底 | suffix，无则 0 |
| `BackendLocation` | `HlslSignatureParameterDesc::Register` | `SpirvStageIo::Location` |
| `ScalarType` | `ComponentType` 映射 | `SpirvTypeInfo::BaseType` 映射 |
| `BitWidth` | 16/32/64 | 16/32/64 |
| `ComponentCount` | `Mask` 的连续位数 | `VectorSize` |

**`Location` 改名为 `BackendLocation`**（原计划叫 `Location`）：同一字段装两种不可互换的语义，
名字必须体现这一点。

`BitWidth` 只作诊断：
- 不参与 DXIL/SPIR-V 子集比较（理由见 1.4，两边是不同的物理事实）
- 不参与未来 primitive format 兼容判断
- 不影响任何 artifact key

在以下结构新增 `std::optional<ShaderVertexInterface> VertexInterface;`：
- `ShaderArtifactBlob`
- `ShaderBytecode`

不修改 `ShaderArtifactEntry`，`index.json` 不保存接口。

结构不变量：

```
Vertex stage  -> VertexInterface 必须 engaged, 允许 Parameters 为空
其他 stage    -> VertexInterface 必须 nullopt
```

### 2.1 提取函数必须公开在 `shader_manifest.h`

`shader_reflection_map.h` 在 `modules/shader/src/`，是私有头（`CMakeLists.txt:5` 只 `PUBLIC include`）。
测试只 `#include <radray/shader/shader_manifest.h>`，拿不到私有头里的函数。
现有测试是通过公开的 `ValidateShaderReflection`（`shader_manifest.h:697`）间接测反射映射的。

所以公开入口放在 `shader_manifest.h`：

```cpp
/// 从 DXIL 反射提取 vertex 输入接口。已规范排序。失败时 outDiag 带原因。
std::optional<ShaderVertexInterface> ExtractVertexInterface(
    const render::HlslShaderDesc& reflection,
    ShaderAssetDiagnostic& outDiag) noexcept;

/// 从 SPIRV 反射提取 vertex 输入接口。已规范排序。失败时 outDiag 带原因。
std::optional<ShaderVertexInterface> ExtractVertexInterface(
    const render::SpirvShaderDesc& reflection,
    ShaderAssetDiagnostic& outDiag) noexcept;
```

`shader_manifest.h` 已包含 `hlsl.h`（第 10 行）与 `spirv.h`（第 12 行），不引入新依赖。
semantic 归一化的小工具留在 `shader_reflection_map.h`，公开的只有这两个入口。

---

## 3. Semantic 归一化

在 `shader_reflection_map.h/.cpp` 增加共用辅助。复用现有的
`SplitSemantic`（`:91`）、`IsSystemSemantic`（`:125`）、`EffectiveSemanticIndex`（`:132`）。
新增一个 ASCII 大写归一化函数。

### DXIL

输入：`HlslSignatureParameterDesc::SemanticName` + `::SemanticIndex`

1. `SplitSemantic` 拆 base 与尾数字
2. `IsSystemSemantic` 过滤 `SV_*`
3. `EffectiveSemanticIndex`：非零 suffix 优先，否则用反射的 `SemanticIndex`
4. base semantic 按 ASCII 转大写
5. 空 base 失败

```
POSITION0 + SemanticIndex 0 -> ("POSITION", 0)
POSITION  + SemanticIndex 0 -> ("POSITION", 0)
TEXCOORD2 + SemanticIndex 0 -> ("TEXCOORD", 2)
```

### SPIR-V

来源优先级：

1. `SpirvStageIo::HlslSemantic`（加了 `-fspv-reflect` 后生效；见 0.1）
2. `SpirvStageIo::Name` 去掉严格前缀 `"in.var."`
3. 两者都不可用则失败

```cpp
string raw;
if (!input.HlslSemantic.empty()) {
    raw = input.HlslSemantic;
} else if (input.Name.starts_with("in.var.")) {
    raw = input.Name.substr(sizeof("in.var.") - 1);
} else {
    // diagnostic + failure
}
```

之后：

1. `SplitSemantic(raw, base, suffixIndex)`
2. 直接用 `suffixIndex`，**不调用** `EffectiveSemanticIndex`（SPIR-V 没有独立的反射 index 字段）
3. 无尾数字自然得 0
4. base 转大写
5. 空 base 失败
6. `BuiltIn` 有值的直接跳过（不进接口）

```
in.var.POSITION  -> ("POSITION", 0)
in.var.POSITION0 -> ("POSITION", 0)
in.var.TEXCOORD2 -> ("TEXCOORD", 2)
```

**不得增加 SPIR-V strip name 开关** —— fallback 路径依赖 `OpName`。

---

## 4. 类型提取

### DXIL

`HlslRegisterComponentType`（`hlsl.h:120`）映射：

```
FLOAT16/32/64 -> Float + 对应 BitWidth
SINT16/32/64  -> SInt  + 对应 BitWidth
UINT16/32/64  -> UInt  + 对应 BitWidth
其他 (含 UNKNOWN) -> 失败
```

`ComponentCount` 从 `Mask` 得到，只接受连续低位：

```
0b0001 -> 1
0b0011 -> 2
0b0111 -> 3
0b1111 -> 4
其他 mask 显式失败
```

**注意**：用 `Mask` 而非 `ReadWriteMask`。`Mask` 是声明的分量数，`ReadWriteMask` 是实际用到的
（见 1.1）。接口描述的是"顶点数据布局需要什么"，所以取 `Mask`。`ReadWriteMask` 只在
cross-category 测试里用来做反向断言。

### SPIR-V

先检查 `input.TypeIndex < reflection.Types.size()`，越界失败。

类型映射结果**可以**按 `TypeIndex` 缓存，但**不能**按 `TypeIndex` 去重参数 ——
`POSITION` 和 `NORMAL` 可能引用同一个 `float3` TypeIndex，但仍是两个参数。

检查 `SpirvTypeInfo`（`spirv.h:65`）：

```
Columns > 1       -> 拒绝矩阵（见 1.2，这是唯一的矩阵拦截点）
ArraySize != 0    -> 拒绝数组
VectorSize 1..4   -> 接受
Struct/Image/...  -> 拒绝
```

`BaseType` 映射：

```
Float16/32/64 -> Float
Int16/32/64   -> SInt
UInt16/32/64  -> UInt
Int8/UInt8/Bool/Void/其他 -> 失败
```

`BitWidth` 从 `BaseType` 推（Float16→16, Float32→32, ...）。

---

## 5. 规范排序

每个 category 提取完成后排序。

主比较键（跨 category 可比）：`Semantic`, `SemanticIndex`, `ScalarType`, `ComponentCount`

因为第 6 节要求 `(Semantic, SemanticIndex)` 唯一，**主键已能唯一定序**。
`BackendLocation` / `BitWidth` 不作为排序键 —— 它们永远不会被用到，作为
"辅助稳定字段"是死代码。排序比较器只用上面四项，并在注释里说明唯一性由何保证。

排序目的：
- 让 `SPIR-V ⊆ DXIL` 可用线性双指针比较
- 让序列化 payload 具有规范表示
- 让 hash 不依赖提取代码的遍历顺序
- 让测试 diff 可读

---

## 6. 唯一性与编号

排序后拒绝：
- 相同 `(Semantic, SemanticIndex)` —— 两个 category 都查
- SPIR-V 另外拒绝重复 `BackendLocation`

允许 location 存在空洞（见 1.1，DCE 会造成空洞）。

**不得重新编号**：`0, 1, 3` 必须原样保存为 `0, 1, 3`。

参数数量上限 32（D3D12 IA slot 限制），提取与 writer 两侧都显式拒绝超出。

---

## 7. Cross-category 规则

接口是**精确 vertex-stage artifact** 的属性，由 category、`IsOptimize`、variant 投影
及其余 artifact identity 共同决定，不是 pass 级固定数据。

测试比较 DXIL 与 SPIR-V 时必须保证同一个：pass、source/entry point、
vertex-stage variant 投影、`IsOptimize`、`ShaderModel`、pass `Defines`、toolchain。

### 双向断言

原计划只有单向 `SPIRV ⊆ DXIL`，但实测（1.1）表明它恒成立、没有区分度。改为双向：

```
(a) 结构:   SPIRV ⊆ DXIL
(b) 有效性: { p ∈ DXIL | p.ReadWriteMask != 0 } ⊆ SPIRV
```

(b) 把 DCE 的不对称本身变成被测行为：DXIL 侧实际用到的参数必须都出现在 SPIR-V 侧。
注意 (b) 需要在提取之外访问 `ReadWriteMask`，所以测试要么直接用
`HlslShaderDesc`（推荐，测试本来就持有它），要么额外暴露。不要为此往
`ShaderVertexParameter` 里加字段 —— `ReadWriteMask` 不是接口的一部分。

比较键只含：`Semantic`, `SemanticIndex`, `ScalarType`, `ComponentCount`

不比较：`BackendLocation`（见 1.3）、`BitWidth`（见 1.4）

这只是一致性测试，不用于：合并两份接口、让一个 category 复用另一个的接口、
生成 artifact key、拒绝只 cook 单一 category。

---

## 8. Blob v1 新布局

原地重定义 v1（裁决见第 12 节）。

### 相对原计划的两处改动

**去掉 `hasVertexInterface` bool**。不变量是 `stage == Vertex ⟺ has interface`，
额外存一个 bool 等于有了第二个真相，reader 必须交叉校验，且多出一整类
"bool 与 stage 矛盾"的错误状态。改为：Vertex stage 无条件写 `parameterCount`（可为 0），
非 Vertex stage 不写任何 interface 字段。

**hash 覆盖 header 关键字段 + payload**，改名 `ContentHash`。原计划的 `PayloadHash`
只覆盖 payload，篡改 header 里的 stage/category/key 不会被 hash 发现。

### 布局

```
[header]
magic                   RADSBLB1
formatVersion           1
key.Low                 u64
key.High                u64
stage                   u32
category                i32
ContentHash.Low         u64
ContentHash.High        u64
payload                 SizedBytes
```

payload（仅当 `stage == Vertex` 时含 interface 部分）：

```
if stage == Vertex:
    parameterCount      u32
    parameters[]

bytecode                SizedBytes
```

parameter：

```
semantic                String
semanticIndex           u32
backendLocation         u32
scalarType              u8
bitWidth                u8
componentCount          u8
```

### 写入流程

```cpp
BinaryWriter payloadWriter;

if (entry.Stage == render::ShaderStage::Vertex) {
    payloadWriter.U32(static_cast<uint32_t>(parameterCount));   // 已在上游校验 <= 32
    for (...) {
        payloadWriter.String(...);
        payloadWriter.U32(...);   // semanticIndex
        payloadWriter.U32(...);   // backendLocation
        payloadWriter.U8(...);    // scalarType
        payloadWriter.U8(...);    // bitWidth
        payloadWriter.U8(...);    // componentCount
    }
}

payloadWriter.SizedBytes(bytecode);

// hash 输入 = header 关键字段 + payload。header 字段按 blob 里的顺序喂进去,
// 使篡改 stage/category/key 同样被 hash 捕获。
const ShaderHash contentHash = HashBlobContent(entry, payloadWriter.GetData());

BinaryWriter blobWriter;
WriteHeader(blobWriter, contentHash);
blobWriter.SizedBytes(payloadWriter.GetData());
```

先构造独立 payload，再计算 hash，不做回填。

`parameterCount` 用 `U32(static_cast<uint32_t>(n))` 而非 `Size32(n)`：
后者会 throw `std::length_error`。数量上限在提取与 writer 侧已显式校验（≤ 32），
不依赖序列化层兜底。

### Hash 语义

```
ContentHash:
    覆盖 header 关键字段 (key/stage/category) + 完整 payload 序列化字节
    payload = interface metadata + nested bytecode field

ShaderArtifactEntry::BytecodeHash:
    只覆盖原始 bytecode
```

两个 hash 不合并、不改名（`BytecodeHash` 保持原名原语义）。

---

## 9. Reader 不变量

所有与 blob 自身结构有关的检查都进 `ReadShaderArtifactBlob`，确保增量 cook 也覆盖。

外层：
- magic 正确
- version == 1
- stage 是合法枚举
- category 是合法枚举
- payload 存在
- `HashBlobContent(...) == ContentHash`
- 读取 payload 后外层 `reader.AtEnd()`

payload：
- Vertex stage 必须有 interface 段；非 Vertex stage 禁止出现
- `parameterCount <= 32`
- `parameterCount` 可由剩余 payload 容纳，**验证后才 reserve**。
  单参数最小编码 = `4(len) + 1(semantic 非空) + 4 + 4 + 1 + 1 + 1 = 16` 字节，
  用 `Remaining() / 16` 作上界。**这个常数随字段增减必须同步**，写在注释里。
- semantic 非空
- semantic 已是规范大写
- semantic **不是** `SV_` 前缀（系统值本该在提取时被过滤掉；原计划漏了这条）
- scalar type 有效且不为 `Unknown`
- bit width 有效（16/32/64）
- component count 为 1..4
- `(semantic, semanticIndex)` 唯一
- SPIR-V `backendLocation` 唯一
- bytecode 非空
- SPIR-V bytecode 尺寸为 4 的倍数
- nested bytecode 读取后 payload reader `AtEnd()`

`BinaryReader::String` 返回指向 payload 的 `string_view`（`binary_io.cpp:79`），
写入 `ShaderVertexParameter` 时**必须复制为拥有型 string**。

本轮不实现旧 v1 兼容分支。旧 blob 被拒绝后由 cook 重新生成。

---

## 10. Writer API

```cpp
bool WriteShaderArtifactBlob(
    const std::filesystem::path& path,
    const ShaderArtifactEntry& entry,
    const std::optional<ShaderVertexInterface>& vertexInterface,
    std::span<const byte> bytecode) noexcept;
```

写入前检查：
- `entry.Stage == Vertex ⟺ vertexInterface.has_value()`
- bytecode 非空
- interface 结构合法（参数数 ≤ 32、semantic 非空且大写非 SV_、类型有效、
  `(semantic, index)` 唯一、SPIR-V location 唯一、已排序）

`ShaderArtifactBlob` 增加：

```cpp
ShaderHash ContentHash{};
std::optional<ShaderVertexInterface> VertexInterface;
```

---

## 11. Cook 路径

现在反射被包在 `if (options.ValidateReflection)`（`shader_manifest.cpp:3226`）内，必须拆开。

新流程：

```
Compile
  -> 必要时反射（一次）
  -> Vertex stage 强制提取接口
  -> ValidateReflection 开启时核对 manifest 资源 ABI
  -> 写 blob
```

### 这是 `CookStage` 的结构性拆分，不是小改

`ValidateCompiled`（`shader_manifest.cpp:3072`）现在自己做反射。要让反射只做一次，
它必须改成接收已解析的 reflection。但 `HlslShaderDesc` 与 `SpirvShaderDesc` 之一在
`#if !defined(RADRAY_ENABLE_SPIRV_CROSS)` 下无法产生，现有代码靠 `#else` 返回 failure。
重构后 `CookStage` 里会有两条 `#if` 分支，各自完成"反射 + 提取 interface + 可选 validate"。
按第 16 节的顺序，这一步要留出相应工作量。

### DXIL

当 `stage == Vertex` **或** `options.ValidateReflection` 时读取 `output.Refl`：
- 只解析一次 `HlslShaderDesc`
- Vertex stage：强制提取 `ShaderVertexInterface`
- `ValidateReflection`：继续调用现有 manifest ABI 校验
- `output.Refl` 缺失则失败

**不得添加 `-Qstrip_reflect`**（`dxc.cpp:383` 会识别它并置 `isStripRefl`）。

### SPIR-V

请求 SPIR-V cook 且未编入 `RADRAY_ENABLE_SPIRV_CROSS` 时立即硬错误。

**这是 behavior break，要登记**：现在 `ValidateReflection=false` + 无 spirv-cross 时
SPIR-V cook 是**成功**的（validate 被跳过，反射从未调用）。改后变成报错。
默认路径撞不到（`shader_cook.cpp:93` 的 `DefaultCategories()` 只在
`RADRAY_ENABLE_VULKAN` 下加 SPIRV，而根 `CMakeLists.txt:83` 已强制
`VULKAN + SHADER_JIT ⇒ SPIRV_CROSS`），但 `--category spirv` + 无 Vulkan 构型会撞。

存在 SPIRV-Cross 时：
- `ReflectSpirv` 只调用一次
- Vertex stage 强制提取接口
- `ValidateReflection` 开启时继续做资源 ABI 核对
- **`ValidateReflection=false` 不能跳过 Vertex stage 反射**

### Category

允许 `DXIL` / `SPIRV`；显式拒绝 `MSL` / `METALLIB`。
`CookShaderAsset` 已有 guard（`shader_manifest.cpp:3319`），补测试即可。

---

## 12. 增量 Cook

命中已有 blob（`shader_manifest.cpp:3178`）：

```
ReadShaderArtifactBlob
  -> reader 完成全部结构校验
  -> 恢复 VertexInterface
  -> 恢复 Bytecode
  -> 计算 index BytecodeHash/BytecodeSize
  -> Reused++
```

不重新执行 reflection。

### 旧 v1 blob 的拦截点是巧合，测试不要断言原因

新旧 v1 的差别在 payload 结构。旧 blob 的 payload 就是裸 bytecode，且旧格式下
`payload == bytecode`，所以**旧的 hash 校验会通过**（如果只看 payload），
真正的拦截发生在后续结构读取上（把 bytecode 首字节当 `parameterCount` 读，
或长度校验失败）。

改成 `ContentHash` 覆盖 header 后，hash 计算方式本身变了，所以旧 blob 会在
hash 这一关就被拒绝 —— 这比原计划稳。但测试仍应**只断言"被拒绝"，不断言具体原因**，
并在注释里说明拦截点。

### 提取逻辑变更必须递增 `kShaderArtifactFormatVersion`（原计划遗漏）

`ComputeShaderArtifactKey`（`shader_manifest.cpp:2770`）与
`GetShaderToolchainHash`（`:2821`）都含 `kShaderArtifactFormatVersion`，
递增它能强制全量重烘。

但增量 cook 的判据只是"blob 存在且 key 匹配"。如果有人改了提取逻辑
（比如修了矩阵处理的 bug），旧 blob 的 key 仍匹配，增量 cook 会**复用带旧 interface 的 blob**，
bug 修复被静默吞掉。

ADR-0004 现有条目是"改动 blob 容器格式或 index schema 时递增"。
**必须扩展为**：改动 blob 容器格式、index schema、**或任何写入 blob 的派生数据的提取逻辑**时，
都要递增。

---

## 13. JIT 路径

`ShaderResolver::CompileWithJit`（`shader_manifest.cpp:2165`）在编译后执行与 cook 相同的提取。

DXIL：`DxcOutput::Refl -> HlslShaderDesc -> ShaderVertexInterface`
SPIR-V：`output.Data -> ReflectSpirv -> ShaderVertexInterface`

```cpp
ShaderBytecode result;
result.Data = ...;
result.Category = ...;
result.Stage = ...;
result.Source = ShaderBytecodeSource::Jit;
result.Key = key;
result.VertexInterface = ...;   // 非 Vertex stage 保持 nullopt
```

无 SPIRV-Cross 时请求 SPIR-V JIT 同样明确失败。

---

## 14. AOT 加载

`ReadShaderArtifactBlob` 返回：`Key`, `Stage`, `Category`, `ContentHash`,
`VertexInterface`, `Bytecode`。

`ShaderResolver::LoadFromArtifact`（`shader_manifest.cpp:2115`）把接口移动进
`ShaderBytecode::VertexInterface`。已有的 `blob->Key` / `blob->Stage` 校验保留；
不需要额外的 interface 校验（reader 已做完）。

本轮不扩展 index schema，不在 index 中重复保存接口。

---

## 15. Program/Variant API

在 `ShaderProgramVariant` 增加：

```cpp
Nullable<const ShaderVertexInterface*> FindVertexInterface() const noexcept;
```

语义：
- 没有 Vertex stage：返回空
- 有 Vertex stage：`Bytecode` 与 `VertexInterface` 必须存在
- metadata 缺失是内部不变量错误，**fail fast**
- 不把"没有 Vertex stage"和"损坏的 Vertex metadata"折叠成同一个空值

### 用 `RADRAY_ABORT` 而非 `RADRAY_ASSERT`（原计划有缺陷）

`logger.h:276` 是 `#define RADRAY_ASSERT(x) assert(x)`，Release（`NDEBUG`）下完全消失。
原计划先 assert 再 `.value()`：Release 下 `nullopt` 时 `.value()` 抛
`std::bad_optional_access`，在 `noexcept` 函数里直接 `std::terminate` —— 行为对但
是隐含 throw 路径，与"本轮不新增 throw"冲突。

改用 `RADRAY_ABORT`（`logger.h:264`），两种构型都生效，且诊断信息更好：

```cpp
for (const StageBlob& blob : _stages) {
    if (blob.Stage != render::ShaderStage::Vertex) {
        continue;
    }
    if (blob.Bytecode == nullptr || !blob.Bytecode->VertexInterface.has_value()) {
        RADRAY_ABORT("vertex stage blob has no vertex interface metadata");
    }
    return &blob.Bytecode->VertexInterface.value();
}
return nullptr;
```

### 生命周期契约修正范围（原计划只提了一处）

`ReleaseRenderResources()`（`shader_program.cpp:279`）清空 `_variants` 与 `_bytecodes`，
之后所有 variant / bytecode / interface 指针失效。需要同步修正三处过宽描述：

- `shader_program.h:82` `ShaderProgramVariant` 类注释"地址在 program 存活期内稳定"
- `shader_program.h:85` `StageBlob` 注释"都指向 program 内的稳定存储"
- `ShaderPassProgram::BytecodeEntry` 注释"unique_ptr 保证地址稳定"

---

## 16. SPIRV-Cross 异常清理

`spvc.h:50-60` 三个函数保持非 `noexcept`：`ReflectSpirv`, `ConvertSpirvToMsl`,
`ReflectSpirvAsMsl`。

三处实现（`spvc.cpp:550`, `:696`, `:1092`）保留 `catch (const spirv_cross::CompilerError&)`，
删除 `catch (const std::exception&)` 与 `catch (...)`。

结果：
- 可恢复的 SPIRV-Cross 编译错误转成 `nullopt`
- 分配失败、编程错误、未知异常不再被吞掉
- 上层不新增 catch
- 现有 `noexcept` cook/JIT 边界决定最终终止语义

更新异常注释，共三处（原计划只列了两处）：
- `shader_manifest.cpp:3101`（`ValidateCompiled` 里那段"不在此捕获"的说明）
- `shader_asset_template.cpp`
- **`tools/shader_cook/shader_cook.cpp:91`** —— 它提到"`ValidateCompiled` 的 `#else` 分支"
  且说"没有它 `ValidateReflection` 必然失败"，改后应为"没有它 SPIR-V cook 必然失败"

统一措辞：SPIRV-Cross 的可恢复 `CompilerError` 已在适配层转为 `nullopt`；
其他异常不在调用点捕获。

`spvc.cpp:198` 与 `:371` 两处局部 `throw CompilerError` 本轮保留，在 ADR 中登记独立清理。

---

## 17. 测试计划

### 提取单元测试（手工构造 reflection）

扩展 `ShaderArtifactTest`（`test_shader_asset.cpp`）。
这些必须用手工构造的 `HlslShaderDesc` / `SpirvShaderDesc`，因为真实 DXC 产物
无法覆盖（见 1.6：DXC 不改 semantic 拼写；`shaderlib` 里没有矩阵）。
现有 `test_shader_asset.cpp:1361` 已有这种构造方式可参照。

DXIL：
- semantic 提取
- suffix / 反射 index 兜底
- semantic 大写规范化
- scalar type、bit width、component count
- 非连续 mask（如 `0b1010`）失败
- `SV_*` 不进接口
- `ReadWriteMask == 0` 的参数**仍然进接口**（它是声明的一部分）

SPIR-V：
- 优先使用 `HlslSemantic`
- `HlslSemantic` 为空时剥 `in.var.`
- 两个来源都不存在时失败
- `POSITION` 无尾数字得 index 0
- semantic 大写规范化
- `TypeIndex` 越界失败
- 多个 parameter 共享 `TypeIndex` 仍全部保留
- `Columns > 1` 矩阵失败
- `ArraySize != 0` 数组失败
- `BuiltIn` 有值的不进接口
- location 空洞 `0,1,3` 原样保存

两侧共通：
- 参数数 > 32 失败
- 重复 `(Semantic, SemanticIndex)` 失败
- 排序结果与输入遍历顺序无关

### Cross-category 测试（真实 DXC 编译）

按 `IsOptimize=true/false` 分开编译，断言双向（见第 7 节）：

```
(a) SPIRV ⊆ DXIL
(b) { p ∈ DXIL | ReadWriteMask != 0 } ⊆ SPIRV
```

比较键只含 `Semantic`, `SemanticIndex`, `ScalarType`, `ComponentCount`。

显式证明：`BackendLocation` 不参与、`BitWidth` 不参与、不同 `IsOptimize` 结果不交叉比较。

用一个"声明了但不使用某属性"的 HLSL 作输入，使 (a) 与 (b) 都非平凡。

### Blob 测试

- 有接口的 Vertex blob round-trip
- 空参数 Vertex interface round-trip
- Pixel/Compute 无接口 round-trip
- Vertex 缺接口被 writer 拒绝
- 非 Vertex 携带接口被拒绝
- metadata 损坏触发 ContentHash 失败
- bytecode 损坏触发 ContentHash 失败
- **篡改 header 的 stage/category/key 触发 ContentHash 失败**（新增，`ContentHash` 覆盖 header 的收益）
- 外层 trailing bytes 被拒绝
- payload trailing bytes 被拒绝
- 非法 count / semantic / scalar type / bit width / component count 被拒绝
- `SV_` 前缀 semantic 被拒绝
- 参数数 > 32 被拒绝
- 巨额 `parameterCount`（如 `0xFFFFFFFF`）被拒绝且不发生巨额 reserve
- SPIR-V bytecode 非 4 字节倍数被拒绝
- 旧平铺 v1 真实 DXIL/SPIR-V 形状被新 reader 拒绝（**只断言被拒绝，不断言原因**）
- `BytecodeHash` 仍只等于原始 bytecode hash

### Cook 测试

- `ValidateReflection=false` 时 `VertexInterface` 仍生成
- 增量第二次 cook 从 blob 恢复接口
- DXIL 与 SPIR-V 分别写入接口
- MSL/METALLIB 显式失败
- 无 SPIRV-Cross 配置下 SPIR-V cook 硬失败

### Resolver/Program 测试

- AOT 加载得到接口
- JIT 编译得到接口
- 同 category、同输入的 AOT/JIT 接口相等
- `FindVertexInterface()` 返回稳定指针 —— **必须解析多个 variant 触发
  `_variants` / `_bytecodes` 的 vector 扩容**，然后验证先前拿到的指针仍有效。
  只解析一个 variant 的测试证明不了稳定性。
- Compute variant 返回空
- `ReleaseRenderResources()` 后缓存计数归零，不再使用旧指针

### 回归测试

- manifest `VertexInput` 仍能构建 PSO
- `ShaderVertexInputStorage` move 稳定性测试继续通过
- `PipelineStateCache` 测试不变
- VerticalSlice DXIL/SPIR-V 像素读回继续通过
  —— **加了 `-fspv-reflect` 后这条是关键验证**（见 0.1）
- shader CLI 仍只链接 `radrayshader` + `radraycore`

---

## 18. 文档计划

### 新增 ADR-0013 vertex-stage artifact 保留最小输入接口投影

内容：
- 为什么不是完整 reflection
- 为什么接口属于精确 artifact 而不是 pass
- SPIR-V semantic 来源与 fallback 规则；`-fspv-reflect` 的取舍与 Vulkan 扩展依赖
- `SPIRV ⊆ DXIL` 的限定条件，以及为什么需要**双向**断言（附 1.1 的实测）
- location 允许空洞且不得重编号
- `BitWidth` 降级为诊断的**实测理由**（1.4：两个 backend 上是不同的物理事实）
- `BackendLocation` 为何按 category 分派语义（1.3）
- **矩阵/数组当前只在 SPIR-V 侧拒绝**：DXIL 侧被 DXC 静默展开成多个连续
  `SemanticIndex`，与手写声明同形无法区分（1.2）。因此"只 cook DXIL"的配置
  不受此保护 —— 这是已知且已接受的洞
- 不得 strip SPIR-V `OpName`（fallback 依赖）
- 不得 strip DXIL reflection
- 为什么 payload 不存 `hasVertexInterface` bool
- `ContentHash` 覆盖 header 关键字段的理由
- 本轮仍未连接 primitive
- `spvc.cpp:198` / `:371` 两处局部 `throw` 后续处理

### ADR-0003 收窄

`0003-manifest-is-abi-authority.md` 有三处需改（**注意 ADR 原则是只追加不修订，
这属于"决策被收窄"，按 README:16 的规则处理**）：

- 第 33 行 "反射数据**不落盘**。cook 期用它核对，之后丢弃。"
- 第 65 行（放弃的方案）"反射结果落盘，运行时读反射代替 manifest"
- 第 72 行（必须保持为真）"反射数据不出现在 `index.json` 或任何 `.bin` 里"

收窄为：资源绑定完整反射不落盘；vertex-stage artifact 保留连接 primitive 所需的
最小接口投影。`index.json` 仍不含任何反射数据。

### ADR-0004 记录本轮裁决

- v1 从未实际部署或消费
- 本轮允许原地重定义 v1 blob grammar
- 不提供旧 v1 兼容；已有本地产物必须重新 cook
- 第 66 行"改动 blob 容器格式或 index schema 时递增 `kShaderArtifactFormatVersion`"
  **扩展为**：容器格式、index schema、**或任何写入 blob 的派生数据的提取逻辑**
  变更时都要递增（见第 12 节，否则增量 cook 会静默复用旧 interface）

### ADR README

在 `docs/adr/README.md` 的表格末尾加入
[ADR-0013](../adr/0013-vertex-stage-interface-projection.md) 条目。

`docs/architecture/overview.md` 的索引不枚举单条 ADR（只列 `adr/` 目录），无需改。

### shader-pipeline.md

更新 `docs/architecture/shader-pipeline.md`：

```
ShaderPassProgram
  -> BytecodeEntry
      -> ShaderBytecode
          -> optional ShaderVertexInterface
```

同时明确：当前 PSO 仍消费 manifest `VertexInput`；artifact interface 本轮只生成和暴露；
primitive 连接属于后续阶段。

第 41-56 行"manifest 是 ABI 权威"一节里"反射数据**不落盘**"要与 ADR-0003 同步收窄。
第 138-157 行"AOT 产物布局"里 blob 容器头部的描述要更新（hash 语义变了）。

---

## 19. 文件变更清单

| 文件 | 改动 |
|---|---|
| `modules/render/src/vk/vulkan_impl.cpp` | 加两个 `VK_GOOGLE_*` 必需扩展（见 0.1） |
| `modules/shader/src/dxc.cpp` | `_BuildCompileArgs` 加 `-fspv-reflect`（见 0.1） |
| `modules/shader/include/radray/shader/shader_manifest.h` | 新类型、`ExtractVertexInterface` 声明、blob 结构、writer 签名 |
| `modules/shader/src/shader_manifest.cpp` | 提取实现、blob writer/reader、`CookStage` 拆分、JIT/AOT 接入 |
| `modules/shader/src/shader_reflection_map.h` / `.cpp` | semantic 大写归一化辅助 |
| `modules/runtime/include/radray/runtime/shader_program.h` | `FindVertexInterface` + 三处生命周期注释 |
| `modules/runtime/src/shader_program.cpp` | `FindVertexInterface` 实现 |
| `modules/shader/src/spvc.cpp` | 删三处广义 catch |
| `modules/shader/src/shader_asset_template.cpp` | 异常注释 |
| `tools/shader_cook/CMakeLists.txt` | 把已启用后端投影为 CLI 默认 category 宏，不新增 render 链接 |
| `tools/shader_cook/shader_cook.cpp` | 异常/行为注释（原计划遗漏） |
| `modules/shader/tests/test_shader_asset.cpp` | 提取 / blob / cook 测试 |
| `modules/runtime/tests/test_shader_program.cpp` | resolver / variant 测试 |
| `docs/architecture/shader-pipeline.md` | 结构图 + 反射落盘描述 + blob 头部描述 |
| `docs/adr/0003-manifest-is-abi-authority.md` | 三处收窄 |
| `docs/adr/0004-content-addressed-shader-artifacts.md` | 版本递增条件扩展 + 本轮裁决 |
| `docs/adr/0013-vertex-stage-interface-projection.md` | 新增 |
| `docs/adr/README.md` | 表格加一行 |

`spvc.h` 不需要改（三个函数签名与 `noexcept` 状态都不变，原计划把它列进清单是多余的）。

---

## 20. 实施顺序

相对原计划的两处调整：异常清理提到最前（独立、纯删除、能先把噪音摘掉，
且会暴露之前被 `catch (...)` 吞掉的问题）；探针测试提到提取实现之前。

1. 解决 0.1（Vulkan 扩展 + `-fspv-reflect`），或裁决退回不加 flag
2. 清理 0.2 的孤儿产物
3. 清理 SPIRV-Cross 广义 catch（`spvc.cpp` 三处）+ 三处注释
4. 写 0.3 的探针测试（矩阵 / 数组 / 未使用参数三种形状）
5. 定义接口数据类型与规范排序比较键
6. 实现 DXIL / SPIR-V 提取单元
7. 实现 blob 嵌套 writer/reader 及结构校验
8. 接入 `CookStage`，拆开提取与 `ValidateReflection`
9. 接入增量 reuse
10. 接入 JIT 和 AOT 加载
11. 接入 `ShaderProgramVariant::FindVertexInterface` + 生命周期注释修正
12. 完成 CPU 单测
13. 更新 ADR 和架构文档
14. 完整构建
15. 顺序运行 shader、runtime、PSO 和 vertical slice 测试（**不并发**）

---

## 21. 完成标准

- 每份 Vertex DXIL/SPIR-V artifact 都携带规范化接口
- AOT/JIT 返回相同数据形状
- `ValidateReflection` 关闭不影响接口提取
- 增量 cook 不丢接口
- blob metadata 与 bytecode 一起受 `ContentHash` 保护，header 关键字段亦然
- variant 可以安全读取接口
- 当前渲染行为完全不变（含 vertical slice 在 `-fspv-reflect` 下的像素读回）
- 没有 `VertexFactory` 或 primitive 连接代码
- 没有新的 `noexcept` / `catch` / `throw`

## 22. 后续方向已取代

不再直接把公共 `ShaderVertexInterface` 接到 primitive。下一轮先按
[`backend-specialized-shader-lanes.md`](backend-specialized-shader-lanes.md) 拆成 DXIL semantic
signature 与 SPIR-V location/type interface，删除 Vulkan semantic 和 `-fspv-reflect` 扩展链；
primitive 连接在两条 backend lane 稳定后分别实施。
