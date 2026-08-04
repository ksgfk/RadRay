> - 适用: 历史参考。**已作废（2026-08），不要实施本文**
> - 权威: 无。本文前提已被 ADR-0014 取消
> - 锚点: `docs/adr/0014-cpp-trace-is-shader-source-of-truth.md`, `docs/adr/0015-variants-are-cpp-parameters.md`

**作废原因**：本文整份建立在「作者手写 HLSL + DXC 反射 + manifest 核对」之上，而 ADR-0014 把
shader 源真相改为 C++ trace、绑定由 trace 构造、manifest 整体删除 —— 反射本身没有了，
「双 lane 反射产物」这个问题不再存在。文内对 ADR-0014/0015 的引用指当时预留的编号，
与实际落地的这两条无关。

**仍有参考价值**：第 8.4 节关于 cook 事务、`ValidatedHash`、fail-closed 加载与原子发布的裁决 ——
这些约束与前端无关，新 cook 层应重新采纳。
> - 锚点: `modules/shader/include/radray/shader/dxc.h`, `modules/shader/include/radray/shader/hlsl.h`, `modules/shader/include/radray/shader/spirv.h`, `modules/shader/include/radray/shader/spvc.h`, `modules/shader/include/radray/shader/shader_manifest.h`
> - 锚点: `modules/shader/src/dxc.cpp`, `modules/shader/src/hlsl.cpp`, `modules/shader/src/spirv.cpp`, `modules/shader/src/spvc.cpp`, `modules/shader/src/shader_manifest.cpp`, `modules/shader/src/shader_reflection_map.h`, `modules/shader/src/shader_reflection_map.cpp`, `modules/shader/src/shader_asset_template.cpp`
> - 锚点: `modules/shader/tests/test_shader_asset.cpp`, `modules/shader/tests/test_shader_asset_template.cpp`, `tools/shader_cook/shader_cook.cpp`
> - 锚点: `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/src/shader_program.cpp`, `modules/runtime/include/radray/runtime/gpu_resource.h`, `modules/runtime/src/gpu_resource.cpp`, `modules/runtime/src/render_system.cpp`
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/render/src/d3d12/d3d12_impl.cpp`, `modules/render/src/vk/vulkan_impl.cpp`

# 后端专用 shader lanes

## 1. 已裁决的方向

当前 shader 基础设施继续保留，但边界改为：

```
共享控制面
  manifest / variant domain / source identity / cook 调度 / AOT-JIT 策略 / 生命周期
                              |
                    显式 target dispatch
                    /                    \
DXIL lane                                  SPIR-V lane
DXIL compile + reflection                  SPIR-V compile + reflection
semantic signature                         Location + SPIR-V type shape
DXIL artifact payload                      SPIR-V artifact payload
DxilShaderDescriptor                       SpirvShaderDescriptor
                    \                    /
                     runtime 只绑定一条 lane
```

本轮不再追求把两个编译结果压进同一份物理模型。以下各项是实施时不可重新打开的决策：

1. **共享的是控制流，不是后端数据形状。** manifest、keyword 投影、源码闭包扫描、cook 遍历、
   AOT 优先/JIT 兜底和资产生命周期只实现一份；编译输出、vertex metadata、blob payload 与 RHI
   shader descriptor 按 DXIL/SPIR-V 分开。
2. **DXIL vertex metadata 是 semantic signature。** 它保留 semantic/index、signature register、
   component type 与 mask；这是 D3D12 input layout 实际消费的编号空间。
3. **SPIR-V vertex metadata 没有 semantic。** 它只保留 `Location` 和 SPIR-V 数值类型形状；
   Vulkan primitive 连接只按 location 工作，不从 HLSL 名字恢复第二套身份。
4. **删除 `-fspv-reflect` 与两个 Google Vulkan 扩展。** 不保留 `HlslSemantic` fallback，
   不以 `OpName` 的 `in.var.*` 恢复 semantic，也不要求产物必须保留 `OpName`。
5. **不做跨 lane 接口相等/子集断言。** DXIL 的 DCE、signature 展开与 SPIR-V 的 active interface、
   聚合类型都是各自的物理事实；测试各自的契约，不再证明它们能还原成一个公共接口。
6. **runtime 在 composition root 只选一次 lane。** 选定后 resolver、program cache 与每次 variant
   请求都不再携带 category；错误 lane 只能在装配边界产生，不能成为每个调用点的自由参数。
7. **lane 独立失效。** DXIL 与 SPIR-V 分别拥有 index、artifact format version 与 toolchain hash；
   修改 SPIR-V 提取或 payload grammar 不得让已有 DXIL artifact 全部失效。
8. **模块边界不变。** `radrayshader` 不依赖 `radrayrender`；shader CLI 仍只链接
   `radrayshader + radraycore`。`RenderBackend` 不能进入 shader 模块。

---

## 2. 当前模型为什么必须拆

当前实现已经把编译、烘焙、AOT/JIT 恢复和 runtime 暴露接通，但以下公共类型实际都是 tagged union
被手工摊平后的形状：

| 当前位置 | 伪统一点 | 后果 |
|---|---|---|
| `DxcCompileOptions::IsSpirv` | 一个 bool 改变输出格式和参数语义 | 调用方可以请求自相矛盾的 options/output |
| `DxcOutput::{Data, Refl, Category}` | `Data` 同时表示 DXIL bytes 与 SPIR-V words | 对齐、反射来源和合法状态只能靠运行时 category 判断 |
| `ShaderVertexParameter` | semantic、DXIL register、SPIR-V location 塞在同一字段集 | Vulkan 被迫携带无用 semantic，编号空间也被命名成假公共概念 |
| `ShaderArtifactBlob` / `ShaderBytecode` | payload 形状由 `Category` 解释 | 每个 reader/consumer 都要重复 category 分支 |
| 单个 `index.json` / toolchain hash | 两条 lane 共用失效域 | 任一 lane 的提取变化都会重烘另一条 lane |
| `ShaderPassProgram` | 每次请求都传 category | 一个已绑定单一 device 的 program 仍允许请求错误字节码 |
| `render::ShaderDescriptor` | `Source + Category` | RHI 接口接受 D3D12+SPIR-V、Vulkan+DXIL 这类本可由类型排除的组合 |

ADR-0013 为制造公共 `ShaderVertexInterface`，又让 SPIR-V 生成 HLSL semantic decoration：

```
-fspv-reflect
  -> SPV_GOOGLE_hlsl_functionality1 / SPV_GOOGLE_user_type
  -> VK_GOOGLE_hlsl_functionality1 / VK_GOOGLE_user_type device dependency
  -> SpirvStageIo::HlslSemantic
  -> 公共 Semantic/SemanticIndex 投影
```

这条链没有给 Vulkan 增加任何它会消费的信息，却提高了设备最低扩展要求，并禁止正常的 name
stripping。重构必须把整条链删除，不能只从最终结构隐藏 semantic。

仓库里已有一份反例证明该 device 依赖不是必需的：`tools/generate_imgui_shader.py` 生成 imgui
SPIR-V 时只传 `-spirv -fspv-target-env=vulkan1.2 -fspv-preserve-bindings`，**不传 `-fspv-reflect`**，
因此 `modules/runtime/src/imgui/radray_imgui_shader.cpp` 里那两份内嵌 SPIR-V 不声明任何 Google
扩展，而 `vulkan_impl.cpp` 仍无条件要求 device 启用它们。这条事实同时说明删除扩展没有第二个
消费者，应写进 ADR-0014 的背景。

---

## 3. 共享与专用的精确边界

### 3.1 继续共享

- `ShaderAssetDesc`、`ShaderPassDesc`、`ShaderVariantDomain` 与 stage keyword 投影
- include 闭包扫描和 `ShaderSourceIdentity`
- pass/variant/stage 的 cook 遍历与去重算法
- `ShaderArtifactStaleness`、AOT 优先/JIT 兜底策略
- diagnostics、统计、artifact 目录根推导
- 资源绑定 manifest ABI，以及“声明包含实际使用项”的校验方向
- `ShaderAsset` / `ShaderPassProgram` 生命周期和 PipelineLayout 共享

### 3.2 DXIL lane 独占

- DXIL DXC 参数与 typed compile output
- `HlslShaderDesc` 反射及 DXIL 资源绑定核对
- DXIL vertex semantic signature
- DXIL blob payload reader/writer、format version、toolchain hash
- DXIL runtime bytecode payload与 RHI descriptor

### 3.3 SPIR-V lane 独占

- SPIR-V DXC 参数与 typed word output
- `SpirvShaderDesc` 反射及 Vulkan set/binding/location 核对
- SPIR-V vertex location/type interface
- SPIR-V blob payload reader/writer、format version、toolchain hash
- SPIR-V runtime word payload与 RHI descriptor

资源绑定规则可以共用小型 canonical comparison helper，但 helper 只能表达 manifest 的公共 ABI
（group/binding/type/count/stage）；不得把 DXIL semantic、SPIR-V location 或 HLSL-only decoration
塞回公共模型。

---

## 4. target discriminator 的保留范围

保留现有 `render::ShaderBlobCategory` 及其公开枚举成员，不重命名 `DXIL` / `SPIRV` / `MSL` /
`METALLIB`。它只允许出现在以下控制/线格式边界：

- cook CLI 的 `--category` 与 `ShaderCookOptions::Categories`
- artifact 目录名及 blob/index 自描述 target
- runtime 初始化时的已选 target
- 选择 typed lane 实现的单次 dispatch

它不再出现在：

- `DxcOutput`
- `ShaderBytecode` 的物理 payload 内
- `ShaderPassProgram::VariantEntry` 及其 variant cache 的比较键
- `GetOrCreateVariant` / `Resolve` 的逐次参数
- RHI shader descriptor 的公共字段

`MSL` / `METALLIB` 本轮继续是既有但不支持 cook/JIT 的公开标识；不加空实现，不为了未来后端
复制第三条 lane。

### 唯一 backend → lane 映射

在 `RenderSystem::OnInitialize`（或其同文件私有 helper）对 `Device::GetBackend()` 做一个穷尽 switch：

```
D3D12  -> ShaderBlobCategory::DXIL
Vulkan -> ShaderBlobCategory::SPIRV
```

结果传给 `ShaderResolveContext` 并在其生命周期内不变。映射属于依赖两侧类型的 runtime composition
root，不得下沉到 `modules/shader`，也不得在测试、asset loader、program 或 PSO 路径各复制一份。

这会推翻 ADR-0006 中“任何一层都没有映射”的绝对条款，但保留其真正目的：`RenderBackend` 留在
`radrayrender`，`radrayshader` 不因便利映射而反向依赖 render。

---

## 5. typed DXC 边界

### 5.1 API 形状

把 target-neutral 的 `DxcCompileOptions` 留作公共编译输入，删除 `IsSpirv`。新增 typed 输出与入口：

```cpp
struct DxilCompileOutput {
    vector<byte> Object;
    std::optional<vector<byte>> Reflection;
};

struct SpirvCompileOutput {
    vector<uint32_t> Words;
};

std::optional<DxilCompileOutput> CompileDxilMemory(...);
std::optional<DxilCompileOutput> CompileDxilFile(...);
std::optional<SpirvCompileOutput> CompileSpirvMemory(...);
std::optional<SpirvCompileOutput> CompileSpirvFile(...);
```

`PreprocessMemory/File` 继续只接公共 options；预处理不再伪装成一次 `IsSpirv=false` 编译。

SPIR-V 在 DXC 适配层完成“byte size 是 4 的倍数”和 bytes → words 转换。从这层出去后，SPIR-V
不再以任意 `vector<byte>` 表示。`DxilCompileOutput::Reflection` 明确对应
`HasOutput(DXC_OUT_REFLECTION)`：没有该 output 时为 `nullopt`，不能再用空 `Refl` 同时表达“未请求”、
“DXC 未产生”和“产生了零字节”。cook、JIT vertex metadata 与 shader_gen 各自在需要反射的路径显式
要求该 optional 有值；不需要反射的 DXIL 消费者只读 `Object`。

`Reflection` **不得出现 engaged-but-empty**。当前 `HasOutput(DXC_OUT_REFLECTION)` 为真而
`GetOutput` 失败时，`Dxc::Impl::GetBlobData` 返回空 span，空 `Refl` 于是又多承担一层含义，
错误直到 `GetShaderDescFromOutput` 的 `refl.empty()` 才被发现。typed 入口在 DXC 边界就把
“声明有 output 但取不到 / 取到零字节”判为编译失败，而不是交给下游当 `nullopt` 处理 ——
前者是 DXC 行为异常，后者是“本次没要反射”，两者不该收敛成同一个状态。

typed words 必须贯穿现有 SPIR-V API，不能只改 DXC 返回值：

- `SpirvBytecodeView::Data` 改为 `std::span<const uint32_t> Words`
- `ReflectSpirv`、`ConvertSpirvToMsl` 与 `SpirvAsMslReflectParams` 都接 word span
- SPIRV-Cross 构造器直接消费 `Words.data()/size()`，不再在内部 bit-cast 任意 byte pointer
- hash 或写文件需要 byte view 时，只在边界用 `std::as_bytes(std::span{words})`
- blob writer/reader、JIT result 与 RHI descriptor 都保持 typed words，不在中途退回通用 bytes

最后一条是**终态**约束（步骤 4 结束时成立），不是每个中间提交都成立。步骤 2 只 typed 化
compiler/reflection 边界，而 `ShaderBytecode` / RHI descriptor / 两个 backend 到步骤 4 才 typed 化，
所以步骤 2 到 4 之间必然存在一次 words → bytes 的收窄。该收窄的登记见第 14 节步骤 2 与末尾的
留存物表：它只允许出现在**一个指定位置**（把 JIT/cook 结果写进 `ShaderBytecode::Data` 那一处），
不得散落到 `spvc` 内部或 RHI 边界。

### 5.2 参数构建

拆成 `_BuildCommonCompileArgs`、`_BuildDxilCompileArgs`、`_BuildSpirvCompileArgs`：

- common：HLSL version、优化、include、define、`-all_resources_bound`
- DXIL：debug/reflection output 的 DXIL 专用参数
- SPIR-V：`-spirv` 及真正属于 Vulkan lane 的参数
- **SPIR-V 参数不得包含 `-fspv-reflect`**

删除 `Dxc::Impl::ArgsData` 与 `ParseArgs`，不得再扫描最终参数中的 `-spirv` / `-metal` 反推出 output
category。typed 入口本身就是 target 的唯一真相；随之删除无读者的 `isStripRefl`、`-metal` 推断分支
和 `DxcOutput::Category`。输出转换由 typed 入口直接选择 DXIL object/reflection 或 SPIR-V words，不能
重新引入“先拼通用 args，再从 args 猜输出类型”的回路。

保留/剥离 `OpName` 是 debug-size 策略，不是 vertex ABI。cook/JIT 即使未来增加 `-Qstrip_debug`，
SPIR-V vertex 提取和运行时校验也必须保持正确，测试必须覆盖空 `DeclaredName`。但 shader_gen 的 authoring
probe 当前用 SPIR-V push-constant `OpName` 找到同名 DXIL cbuffer，再由 DXIL 名字生成 manifest；在
这条 authoring-only 关联被替换前，authoring 编译**必须保留 `OpName`**。空名或找不到同名 DXIL
cbuffer 必须使模板生成显式失败，不能静默把 push constant 留成普通 cbuffer。这个局部依赖不把名字
升级为 cook/JIT/runtime ABI。

---

## 6. 两套 vertex metadata

删除 `ShaderVertexScalarType`、`ShaderVertexParameter`、公共 `ShaderVertexInterface` 与两个同名
`ExtractVertexInterface` overload，改成不会互相伪装的类型。`DxilVertexParameter`、
`DxilVertexSignature`、`SpirvVertexType`、`SpirvVertexInput` 与 `SpirvVertexInterface` 都定义在
`shader_manifest.h` 的 `radray` 命名空间；只引用 `radray::render` 中已有的后端反射枚举/类型。

### 6.1 DXIL semantic signature

```cpp
struct DxilVertexParameter {
    string Semantic;
    uint32_t SemanticIndex{0};
    uint32_t Register{0};
    render::HlslRegisterComponentType ComponentType{
        render::HlslRegisterComponentType::UNKNOWN};
    uint32_t Stream{0};
    uint8_t Mask{0};
    uint8_t ReadWriteMask{0};
};

struct DxilVertexSignature {
    vector<DxilVertexParameter> Parameters;
};
```

提取规则：

- 过滤 `SV_*` / 非 `UNDEFINED` system values
- semantic 拆 base/index，base 用 ASCII 大写规范化，保留独立 `SemanticIndex`
- 保留 DXIL 自己的 `Register`、`ComponentType`、`Stream`、`Mask`、`ReadWriteMask`
- 按 `(Semantic, SemanticIndex, Stream, Register)` 规范排序
- 拒绝重复 semantic/index/stream 与非法 component mask/type
- D3D12 的 32 input-element 限制只约束 DXIL lane，不再伪装成 SPIR-V 通用上限

矩阵/数组被 DXC 展开后无法恢复，按 signature 原样保存；不再借 SPIR-V 形状替 DXIL 猜测源码。

### 6.2 SPIR-V location/type interface

先在 `spirv.h` 的 `radray::render` 命名空间定义不会丢失 SPIRV-Cross
`array_size_literal` 的原生反射维度：

```cpp
enum class SpirvArrayDimensionKind : uint8_t {
    Literal,
    NonLiteralId,
    Runtime,
};

struct SpirvArrayDimension {
    SpirvArrayDimensionKind Kind{SpirvArrayDimensionKind::Literal};
    uint32_t Value{0};  // literal length 或 SPIR-V result id；Runtime 时必须为 0
};
```

`SpirvArrayDimension` 只服务 `SpirvTypeInfo` / `SpirvTypeMember` 等原始反射数据。vertex 提取在构造
artifact 前已经拒绝 `NonLiteralId` / `Runtime`，因此持久化类型不再保存一个不可达的 `Kind`。以下新
vertex 类型放在 `shader_manifest.h` 的 `radray` 命名空间：

```cpp
struct SpirvVertexType {
    render::SpirvBaseType BaseType{render::SpirvBaseType::UNKNOWN};
    uint32_t VectorSize{1};
    uint32_t Columns{1};
    vector<uint32_t> ArrayDimensions;  // 每项都是非零 literal length
};

struct SpirvVertexInput {
    uint32_t Location{0};
    SpirvVertexType Type;
};

struct SpirvVertexInterface {
    vector<SpirvVertexInput> Inputs;
};
```

把 `SpirvTypeInfo` 的单个 `ArraySize` 扩成 `vector<SpirvArrayDimension>`，逐维读取 SPIRV-Cross
`array` + `array_size_literal`；不能把 literal length `2` 与 specialization-constant id `2` 当成同一
形状，也不能用 `0` 同时表达“非数组”和“runtime-sized”。同步审计 `SpirvTypeMember::ArraySize` 与
`SpirvResourceBinding::ArraySize`：member 若继续保存数组形状也改用同一维度类型；resource binding
对 literal 维度折叠出 manifest count，对 runtime array 标记 unbounded，对所有 non-literal id
（包括 specialization constant 与 specialization expression）显式报不支持，不能静默把 result id
当长度。`_ReflectType` 计算 `Size` 时遇到 runtime/non-literal 维度立即把 size 置 0，禁止继续乘原始 id。

#### 先修 type cache 的身份碰撞，否则维度改造无效

只把字段改成 vector **不够**。`_ReflectType` 现在按 `rootType.self` 做 cache key，而 SPIRV-Cross 的
`OpTypeArray` / `OpTypeRuntimeArray` 处理里有一行决定性的赋值：

```cpp
// .self resolves down to non-array/non-pointer type.
arraybase.self = base.self;
```

即数组类型的 `self` 被降到 element type。于是同一 module 内 `float4`、`float4[2]`、`float4[3]`、
`float4[]`（runtime）、`float4[N]`（spec-sized）**共用同一个 `self`**，也就共用同一个 cache 项：
第一个被反射的形状会被后续所有形状复用。当前单 `ArraySize` 字段下这个 bug 就已存在，改成
维度 vector 只会让它更明显 —— 反射出的维度序列可能整个来自另一个类型。

所以本节必须包含 cache key 的修正，且它是前置项：

- cache key 改为**精确 type ID**（`SPIRType` 在 `ParsedIR` 中的 result id，即传给
  `get_type` 的那个 id），不用 `self`
- 或退一步用完整 shape key（`self` + `op` + 逐维 `array`/`array_size_literal`）；
  但精确 type ID 更简单且天然唯一，优先它
- `parent_type` 链在需要走 element type 时显式取，不再依赖 `self` 恰好指向 element

回归测试必须在**同一个 module 内**同时出现同一 element type 的多种数组形状，至少覆盖
`float4` / `float4[2]` / `float4[3]` 三者，断言三个 `SpirvTypeInfo` 的维度序列各自正确且互不污染；
再加一例把 runtime-sized 与 fixed-size 放进同一 module。只测单一形状的 module 无法暴露该碰撞。

提取规则：

- 跳过 `BuiltIn`
- 名字（`DeclaredName` / `DisplayName` 两者）都不参与成功条件、排序、唯一性或 vertex artifact
  payload；原始 reflection debug JSON 可以保留它们作为可选诊断字段
- 只读取 `Location` 与 `TypeIndex` 指向的数值类型形状
- 按 `Location` 排序并拒绝重复 location
- 保留 scalar width、vector size、column count 与固定数组维度，不做 semantic 展开
- 非数值/struct/runtime-sized/specialization-sized vertex input 显式失败
- 矩阵和固定数组不在提取时伪装成多个 semantic；未来 Vulkan primitive linker 按 location
  consumption 规则展开或给出不支持诊断
- 不套用 D3D12 的 32 项上限；reader 仍须先按 payload 剩余字节验证 count，避免恶意 reserve
- `maxVertexInputAttributes` 是未来 linker/device validation 的设备限制，不进入 reflection extractor

### 6.3 stage 不变量

- vertex DXIL payload 必有 `DxilVertexSignature`，允许参数为空
- vertex SPIR-V payload 必有 `SpirvVertexInterface`，允许输入为空
- pixel/compute payload 不携带 vertex metadata
- 不提供把两者转换成公共 parameter 列表的 helper

---

## 7. 清除 SPIR-V 中的 HLSL decoration 依赖

`-fspv-reflect` 除 semantic 外还产生 `DecorationUserTypeGOOGLE`。必须审计并删除整条消费链：

- 删除 `SpirvStageIo::HlslSemantic` 及其 JSON codec 字段
- 删除 `_ReflectStageIoValue` 对 `DecorationHlslSemanticGOOGLE` 的读取
- 删除 `ExtractVertexInterface(SpirvShaderDesc)` 的 `HlslSemantic` / `in.var.` 两级 fallback。
  **终态如此，但两级不同时删**：`HlslSemantic` 那级随本节其余项一起删（第 14 节步骤 2），
  `in.var.` 那级作为该步骤到 lane 化之间的唯一语义来源留存，随 `SpirvVertexInterface` 一起删
  （步骤 4）。理由与后果见第 14 节步骤 2 与末尾的留存物表
- 删除 `SpirvResourceBinding::HlslType` 及 `DecorationUserTypeGOOGLE` 读取
- 删除当前未被消费的 `HlslRegister` / `HlslSpace` SPIR-V JSON 字段，不保留永远为空的兼容壳
- 把 `SpirvResourceBinding::Name` 拆成 `DeclaredName` / `DisplayName`（`SpirvStageIo` 与
  `SpirvPushConstantRange` 同理），理由见下文「空名不是可用的判据」
- 删除仅为 HLSL 形状猜测存在且无消费者的 SPIR-V `IsViewInHlsl` 字段/helper；若实施时发现真实
  消费者，先把该消费者改为 SPIR-V 原生规则，不能因此保留 Google extension
- `ValidateShaderReflection(const SpirvShaderDesc&)` 的 vertex input 失败诊断改用 location 与
  `DisplayName`（诊断用途，允许是合成名），删除当前对 `input.HlslSemantic` 的读取；
  validation 路径不能成为漏网消费者
- 该 vertex input 检查本身**只比 location**（现状已如此，manifest 侧取
  `attribute.Location.value_or(声明序号)`），本轮不因 name policy 变化而改判定口径；
  下面「resource validation 的名字策略」只约束 resource binding，不约束 vertex input
- Vulkan device extension 集合删除
  `VK_GOOGLE_HLSL_FUNCTIONALITY_1_EXTENSION_NAME` 与 `VK_GOOGLE_USER_TYPE_EXTENSION_NAME`

上述字段和数组表示会改变 `SpirvShaderDesc` 的版本化 JSON schema。把当前共享的
`kReflectionFormatVersion` 拆成两个常量，归属与初值明确如下：

| 常量 | 位置 | 初值 | 使用点 |
|---|---|---|---|
| `kHlslReflectionFormatVersion` | `hlsl.h`（沿用现有声明位置） | 3（不变） | `hlsl.cpp` 的 writer/validator |
| `kSpirvReflectionFormatVersion` | `spirv.h`（新增） | 4 | `spirv.cpp` 的 writer/validator |

放在 `spirv.h` 而非继续共用 `hlsl.h`，同时断掉 `spirv.cpp` 对 `hlsl.h` 仅为取版本号而存在的
include —— 那是两条 lane 之间目前唯一一条纯版本号耦合。SPIR-V 版本从 3 跳到 4 而不是原地
重定义 3，reader 拒绝旧 schema，不加缺字段兼容。`SpirvStageIo` 的名字同样拆成
`DeclaredName`（可空）与 `DisplayName`，两者都可序列化供 authoring/诊断使用。
该 JSON codec 当前只有测试/直接 API 消费者，
没有生产 artifact 调用点，因此这是 schema 清理而非 AOT 产物迁移风险；仍用独立版本和拒绝测试
守住边界。

### resource validation 的名字策略

当前 `MatchReflectedBindings` 同时比较位置、类型和名字，SPIR-V 路径因而仍隐式依赖 `OpName`。
拆成公共 location/type/count 检查与 lane policy：

- DXIL：可继续核对 register/space 处的 HLSL resource name
- SPIR-V：以 set/binding/type/count/stage 为 ABI；**有真实 declared name 时**才附加名字 lint（见下）
- SPIR-V push constant：继续按现状只核对 range/size/stage，不要求名字；这是保持不变量，不是新增规则
- `radray_shader_gen` 仍以 DXIL 名字生成 manifest；SPIR-V authoring probe 只补 DXIL 无法识别的
  push-constant shape。probe 暂时以 SPIR-V `OpName` 关联同名 DXIL cbuffer，故必须遵守第 5.2 节的
  authoring 保名与显式失败不变量，但不把 SPIR-V name 升格为 cook/JIT/runtime ABI

#### 为什么 SPIR-V name 检查是「弱形式」而不是删除

把 name 完全移出正确性会打开一类系统性偏移漏检。forward_pass 的 `gShadowCube`(t1) 与
`gShadowArray`(t2) 同为 `Texture`、`Count` 都是 1、stage 都是 Pixel：binding 整体偏移 1 时两者
互相落到对方的槽位，set/binding/type/count/stage 全部对得上，纯位置检查会通过。

这不是假想风险。`shader_asset_template.cpp` 里 `AbsorbSpirvReflection` 的注释明确记着
「SPIR-V 侧的 set/binding 经过 DXC 的重映射，与 HLSL 的 (space, register) 不一定一致」，这正是
模板生成刻意不从 SPIR-V 并入绑定集合的原因。本文其余部分把这个「不一定一致」的编号空间
提升为 SPIR-V lane 的唯一 ABI，因此不能同时删掉唯一一条交叉核对。

补一条相关现状，避免实施时误判风险面：`shaderlib/core/platform.hlsli` 的
`#if defined(VULKAN) || defined(METAL)` 在仓库里**没有任何地方定义 `VULKAN`**，所以 `VK_BINDING`
一直展开为空，SPIR-V 的 set/binding 完全来自 DXC 默认映射。它当前恰好与 manifest 一致，靠的是
「b/t/s/u 共用同一编号空间」这条 ABI，不是显式标注。

故 SPIR-V 的 name 检查明确定性为 **opportunistic lint**，不是 ABI 校验：

- 有真实 declared name 且与 manifest 不符 → 失败，诊断同时给出 set/binding
- 无真实 declared name → 跳过名字比较，其余检查照常
- name 不参与 artifact key、不进 payload、不作为查找键

**必须直说它的代价，不能假装无损**：strip 掉 `OpName` 会关闭这条 lint，于是「binding 整体偏移」
这类输入在 strip 前失败、strip 后通过。这正是 lint 与 ABI 校验的区别 —— ABI 校验的结论不能
随 debug 信息存在与否而改变，lint 可以。

之所以接受这个代价而不是要求保名：要求保名等于把 `OpName` 升格为 cook 前置条件，与本轮
「SPIR-V lane 不依赖 HLSL 名字、产物可以 strip」的方向直接冲突。lint 的价值在于**默认工具链
（不 strip）下能挡住偏移**，这是当前唯一挡得住的地方；strip 是发布期的显式取舍，做这个取舍的人
应当知道自己同时放弃了这条检查。

因此本节的准确表述是：

- SPIR-V lane 的**正确性契约**是 set/binding/type/count/stage，与名字无关
- name lint 叠加在契约之上，尽力而为，可被 strip 关闭
- 剥掉 `OpName` 不改变 cook/JIT 产物、vertex metadata、runtime shader 创建结果，
  但**会放宽 resource binding 的校验强度** —— 这是已知且接受的
- 未来若要把偏移检测做成不依赖名字的强校验，正确方向是让 `VK_BINDING` 真正生效
  （见上文 `VULKAN` 宏从未定义那条现状），使 SPIR-V 的 set/binding 由 HLSL 显式钉住而非
  DXC 默认映射；那是独立课题，不在本轮

#### 「空名」不是可用的判据：必须区分 declared name 与 display name

上面的 policy 依赖「能否判断存在真实 `OpName`」，而当前 `SpirvResourceBinding::Name` **给不出
这个信息**。`_ProcessResource` 在 `compiler.get_name(res.id)` 为空时回退到 `res.name`，而 SPIRV-Cross
填充 `res.name` 的方式对 UBO/SSBO 是 `get_remapped_declared_block_name(var.self, false)`：

- `declared_block_names` 只在 emission 期被填充，纯反射路径永远查不中
- 于是落到 `get_block_fallback_name`，返回合成名 `_<basetype id>_<var id>`

结论：**strip 掉 `OpName` 之后，UBO/SSBO 的 `Name` 仍然非空**，只是变成一个与 manifest 声明
必然不同的合成串。若照「非空即比较」实现，strip 后不是「跳过 lint」，而是**每条 cbuffer 都
误报 mismatch** —— 把一个可选 lint 变成了硬失败，方向完全反了。

故 `SpirvResourceBinding` 的名字字段必须拆成两个语义：

```cpp
/// 真实 OpName（compiler.get_name(res.id)）。strip 后为空。只有它能喂给 lint。
string DeclaredName;
/// 供人读的显示名，允许是 SPIRV-Cross 合成的 fallback。只进诊断。
string DisplayName;
```

- reflection 只把 `compiler.get_name(res.id)` 写进 `DeclaredName`，**不做任何回退**
- `DisplayName` 保留现有「空则取 `res.name`」的回退，仅用于诊断可读性
- name lint 只消费 `DeclaredName`；`DeclaredName` 为空即跳过
- 同一处理同样适用于 `SpirvStageIo` 与 `SpirvPushConstantRange`：
  `_ReflectPushConstants` 现在直接取 `pc.name`，那也是可能被合成的显示名

这条拆分不是可选优化。不做它，第 11.3 节「空名版本仍通过」的测试用真实 strip 产物根本跑不通 ——
它只在手工构造的 `SpirvShaderDesc`（`Name` 真为空）上成立，而那不是 strip 后的实际形状。
测试必须至少有一例走真实 `-Qstrip_debug` 或等价 strip 产物，而不是只喂手搓结构体。

---

## 8. typed artifact 与独立失效域

### 8.1 目录布局

从单个根 `index.json` 改成每条 lane 自己的 index：

```
forward_pass.shader.json
forward_pass/
    dxil/
        index.json
        <dxil-key>.bin
    spirv/
        index.json
        <spirv-key>.bin
```

两份 index 都记录该 manifest 的 `AssetName` 与逐 source identity；重复少量控制面数据换来独立部署、
独立版本和独立损坏边界。`ShaderArtifactEntry` 不再逐项重复 `Category`，`BlobPath` 也只需 lane 内相对
路径。index 根必须自描述 `Target`，reader 同时核对目录所选 target，防止误拷贝静默通过。

路径 helper 同步拆开，避免在 lane index 下再次拼出 `dxil/dxil/...`：

```cpp
GetShaderArtifactDirectory(manifest)                  // forward_pass/
GetShaderArtifactLaneDirectory(manifest, target)      // forward_pass/dxil/
MakeShaderArtifactBlobFileName(key)                   // <key>.bin
```

cook 在 lane directory 下写 index/blob；resolver 从同一 helper 找 lane index，再把 entry 的 lane-relative
`BlobPath` 拼到 lane directory。删除当前返回 `<category>/<key>.bin` 的
`MakeShaderArtifactBlobPath(category, key)`，不保留两套路径约定。

旧根 index 的迁移行为明确裁决如下：当所选 `<lane>/index.json` 不存在时，resolver 额外探测 artifact
根的旧 `index.json`。若旧文件存在，返回“legacy root shader index; clean and recook”诊断，**即使允许
JIT 也不得按普通 AOT miss 兜底**；只有 lane index 与旧根 index 都不存在时，才走正常 JIT/无产物
分支。新 reader 对旧 blob magic/version 同样显式报不兼容。这样“旧产物被拒绝”是可测试行为，而
不是新路径恰好找不到旧文件。

index reader 必须在 join 前验证 `BlobPath`：非空、相对且非 rooted、不含 `.` / `..` component，
归一化后仍严格位于所选 lane directory 下；absolute path、盘符路径、UNC path 与 traversal 一律拒绝。
不要假设“文件由自己的 cook 生成”就跳过边界检查，发布包的 index 是不可信输入。

### 8.2 版本与 hash

删除一个全局 `kShaderArtifactFormatVersion` 控制所有概念的做法，至少拆成：

```cpp
kShaderSourceIdentityVersion
kDxilArtifactFormatVersion
kSpirvArtifactFormatVersion
GetDxilShaderToolchainHash()
GetSpirvShaderToolchainHash()
```

- source identity version 只在 include scanner/hash grammar 改变时递增
- DXIL format/extractor 改动只递增 DXIL version
- SPIR-V format/extractor 改动只递增 SPIR-V version
- 两个 toolchain hash 都含 DXC version、对应 lane 编译参数策略与对应 format version
- artifact key 含稳定 lane tag与该 lane toolchain hash，不依赖另一条 lane 的 version
- index staleness 只比较自己 lane 的 toolchain hash

#### 旧站点 → 新常量的逐项映射

现有 `kShaderArtifactFormatVersion` 在 `shader_manifest.cpp` 里有 8 个使用点，覆盖 4 个互不相干的
概念。**必须逐项迁移**，不能只改名字最显眼的那几处：

| 现有站点（`shader_manifest.cpp`） | 概念 | 迁移到 |
|---|---|---|
| index JSON writer / reader 的 `FormatVersion` | index schema | 所属 lane 的 `k*ArtifactFormatVersion` |
| `ShaderResolveContext` 闭包哈希的首个 `accum.U32` | source identity | `kShaderSourceIdentityVersion` |
| `ComputeShaderSourceIdentity` 的首个 `accum.U32` | source identity | `kShaderSourceIdentityVersion` |
| `ComputeShaderArtifactKey(params)` 的首个 `accum.U32` | artifact key grammar | 该 lane 的 `k*ArtifactFormatVersion` |
| `GetShaderToolchainHash` 的 `artifact={}` | toolchain 身份 | 拆成两个 `Get*ShaderToolchainHash` |
| blob writer 的 `writer.U32` | blob envelope | 该 lane 的 `k*ArtifactFormatVersion` |
| blob reader 的版本核对 | blob envelope | 该 lane 的 `k*ArtifactFormatVersion` |
| index reader 的 `formatVersion` 核对 | index schema | 该 lane 的 `k*ArtifactFormatVersion` |

**source identity 的两处必须同步改**，这是本节最容易漏且后果最隐蔽的一点。
`ShaderResolveContext` 的记忆化闭包哈希与无缓存版 `ComputeShaderSourceIdentity` 各写了一份
累加序列，`shader_manifest.h` 上「哈希公式不可改 —— 累加顺序与无缓存的
`ComputeShaderSourceIdentity` 逐字一致」这条注释要求二者逐字相同。只改一处**不会编译失败**：
后果是运行时算出的身份与 cook 写进 index 的身份永久不等，`Strict` 下全部 AOT 静默未命中并
退化成 JIT。加一条测试断言两条路径对同一源文件给出相同 hash。

artifact key 里的 lane format version 与 `ToolchainHash` 内含的那份是**同一个值经两条路径进入
key**，这是冗余但无害的：两者同时递增，不会产生分歧。实施时不要为了消除冗余就把其中一处
去掉 —— 保留 key 自己那一份的意义是「即使将来 toolchain hash 的配方变了，key 仍直接受
format version 保护」。若选择只留一处，必须在 ADR-0015 里写明是哪一处。

本次 schema 不再第二次原地重定义现有 v1。新 lane-aware envelope 使用新 magic/version（初始可把
两个 lane version 都定为 2）；旧根 `index.json` 和旧 blob 一律不兼容，靠 `--clean` + 全量重烘
迁移，不加 fallback reader。

### 8.3 公共 envelope，专用 payload

blob 公共 header 只负责 dispatch 和独立自验：

```
magic
target
laneFormatVersion
key
stage
ContentHash
payload SizedBytes
```

`ContentHash` 覆盖 target/version/key/stage 与完整 payload。header 核对后立即按 target 分派到
`ReadDxilArtifactPayload` 或 `ReadSpirvArtifactPayload`。

DXIL payload：

```
if vertex:
    signatureParameterCount
    parameters[]   semantic + index + register + componentType + stream + mask + readWriteMask
bytecode SizedBytes
```

SPIR-V payload：

```
if vertex:
    inputCount
    inputs[]       location + baseType + vectorSize + columns
                   + arrayDimensionCount + arrayDimensions[]   每项都是非零 literal length
wordCount
words[]            u32
```

数组维度用**显式 count + 定长项**，不用「0 作终止哨兵」。哨兵编码下 reader 无法在分配前知道
要读多少项，而第 11.4 节要求「array-dimension count 的边界检查先于分配」——两者不可兼得。
显式 count 也与现有 blob reader 先按 payload 剩余字节验证参数数量的做法一致。

因为 input 项**不再定长**（内嵌一个变长数组），预检要分两层，不能照抄现有那条单层
`count > Remaining() / kMinBytes`：外层按「每个 input 的最小编码字节数」验 `inputCount`，
内层在读到每个 `arrayDimensionCount` 时按当时的剩余字节再验一次。任一层不通过即拒绝，
不要先 `reserve` 再边读边判。

in-memory 结果使用 variant，而不是公共字段全集：

```cpp
struct DxilShaderPayload {
    vector<byte> Bytecode;
    std::optional<DxilVertexSignature> VertexSignature;
};

struct SpirvShaderPayload {
    vector<uint32_t> Words;
    std::optional<SpirvVertexInterface> VertexInterface;
};

using ShaderArtifactPayload = std::variant<DxilShaderPayload, SpirvShaderPayload>;
```

两个拥有者的公共字段不要混成一套：

- `ShaderArtifactBlob` 保存 `Key`、`Stage`、`ContentHash` 与 typed payload；它不保存 runtime `Source`
- `ShaderBytecode` 保存 `Key`、`Stage`、`Source` 与 typed payload；它不复制 `ContentHash`，因为 JIT
  结果没有 canonical artifact envelope 可供计算

两者的 target 都由 variant alternative 唯一决定，不得再同时保存 `Category` 制造第二个可冲突真相。

### 8.4 cook 事务边界

`CookShaderAsset` 继续共享 pass/variant/stage 遍历，但在 category 外层创建 lane-local result/index。
公共返回类型同步改成可表达多条 lane，而不是继续暴露一份含混的 `result.Index`：

```cpp
struct ShaderCookLaneResult {
    ShaderArtifactIndex Index;
    ShaderCookStats Stats;
    vector<ShaderAssetDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Diagnostics.empty(); }
};

struct ShaderCookResult {
    vector<ShaderCookLaneResult> Lanes;
    vector<ShaderAssetDiagnostic> CommonDiagnostics;

    bool Succeeded() const noexcept;
};
```

manifest/source/domain 这类 dispatch 前失败进入 `CommonDiagnostics`；编译、反射、blob/index 失败进入
对应 lane result。`ShaderArtifactIndex::Target` 是 lane result 的唯一 target 来源，result 不再重复一枚
可冲突的 `Target`。空 category 列表、`MSL` / `METALLIB` 等无法形成合法 lane 的请求在
创建任何 lane result 前失败，进入 `CommonDiagnostics`；现有 unsupported-category 测试同步改读这里。
CLI 按 `lane.Index.Target` 打印 entry/stats/diagnostics，测试不再读取单个 `result.Index`。

遍历顺序需要调整：现有循环是 `pass → category → variant → stage`，lane result 必须在**进入 pass
循环之前**按 requested categories 逐条建好，去重查表随之从 `result.Index.Find(key)` 变成
`lane.Index.Find(key)`。source identity 仍在 pass 循环前算一次，两条 lane 各记一份相同内容 ——
它是 lane 无关的共享控制面数据，不重复计算，只重复写入。

事务规则：

- 每条 requested lane 全部成功后才发布自己的 `index.json`
- 一条 lane 失败不写半份新 index，也不删除另一条 lane 的有效产物
- **blob 与 index 都经临时文件 + 原子替换发布**（见下文），否则「事务」只是措辞
- **一条 lane 失败后其余 lane 继续烘完**，不提前 return。现有实现遇到硬错误立刻
  `return result`，那在单 index 下无所谓；lane 化之后提前返回会让「DXIL 编译错」顺带
  埋掉「SPIR-V 也有独立的错」，一次构建只能暴露一个问题。这与 CLI 既有的「一份 manifest
  失败也继续烘剩下的」是同一条取舍，只是下沉了一层
- `CommonDiagnostics` 失败（manifest/source/domain）**是例外**：它发生在 dispatch 之前，
  没有任何 lane 可继续，直接返回
- overall result 在任一 requested lane 失败时仍失败，并按 lane 汇报 stats/diagnostics
- `Incremental` 只读当前 lane 的 blob/version
- `--clean` 仍删除整个 manifest artifact 目录；不新增 `--output`

#### 「发布」必须真的原子，否则失败会留下被截断的 blob

上面那组规则说的是**顺序**（先 blob 后 index、全成功才发布），但底层写入没有原子性：
`WriteBinaryFile` 与 `WriteTextFile` 都以 `std::ios::trunc` 直接打开目标路径，一旦
打开后写入中途失败（磁盘满、进程被杀、并发同名写），目标文件已经被清空，而旧
`index.json` 仍指向它。此时 index 与 blob 都存在、版本也对，只是 blob 是空的或半截的。

这在本轮之后更容易触发：`ValidatedHash` 变化会在 **artifact key 完全不变**的情况下要求
重跑校验并重写同一条 blob 路径 —— 也就是新增了一类「路径不变但要重写」的操作，
正是覆盖写最危险的形状。

规则：

- 新增一个 core 层的原子写入原语（`WriteFileAtomic` 之类），语义为
  「写同目录临时文件 → flush → 关闭 → `std::filesystem::rename` 覆盖目标」。
  同目录是必要条件，跨目录 rename 不保证原子
- blob 与 lane `index.json` 都走这个原语；lane index 是 lane 的提交点，必须最后发布
- 临时文件名带 lane 与唯一后缀，避免双 lane 或并发 cook 互相踩
- 任何一步失败：删掉临时文件后返回失败，**不触碰目标路径** —— 于是旧 blob 与旧 index
  保持配对，最坏结果是「这次 cook 没有推进」，而不是「产物目录处于自相矛盾的状态」
- rename 失败也当失败处理，不做「先删目标再重试」的降级（那正好重新引入被截断的窗口）

`WriteBinaryFile` / `WriteTextFile` 本身不改语义 —— 它们在非产物路径上仍是正确的选择；
本节只要求 cook 的产物发布路径改用新原语。

如果不打算实现原子替换，那就必须把第 8.4 节的「事务」措辞降级为「按顺序发布，失败可能
留下需要 `--clean` 的产物目录」，并在 CLI 文档里写明这一点。本文选择实现原子替换：
承诺了事务语义又做不到，比一开始就不承诺更糟。

#### incremental 命中会绕过 reflection validation（现存缺陷，本轮必须裁决）

`CookStage` 在 blob 自验通过后**直接 return**，而反射与 `ValidateShaderReflection` 在其后才执行。
同时 `ShaderArtifactKeyParams` 只含 source identity / pass name / stage / entry point / shader model /
category / defines / optimize / unbounded / toolchain hash —— **不含 manifest 的 binding ABI**。

两者叠加的后果：只改 manifest（binding 的 name/type/count/stage、push constant location、
`VertexInput`）而不动 HLSL 时，artifact key 不变 → blob 命中 → 反射根本不跑 → cook 报成功。
下一次真正发现不一致要等到运行时建 PSO 失败。这是本轮之前就存在的洞，不是新引入的；但本轮
新增的 name lint 与 lane 化会**同时**落进这个洞，所以必须在这里裁决而不是留给实施者。

三个候选，本文选第 2 个：

1. **把 manifest binding ABI 塞进 artifact key。** 语义错误：manifest 改 `Residency` 或 name 不改变
   字节码，让它进 key 会把纯声明改动变成全量重编，且与 ADR-0004「key 只含影响字节码的输入」
   直接冲突。否决。
2. **引入独立的 validation fingerprint，与 artifact key 并列存在 index 里。**（选定）
   `ShaderArtifactEntry` 增加一枚 `std::optional<ShaderHash> ValidatedHash`，覆盖该 (pass, stage)
   参与反射核对的 manifest 投影：binding 的 (group, binding, name, type, count, stages)、
   push constant 的 (name, location, size, stages)、`VertexInput` 的逐条 vertex 投影
   （精确口径见下文「覆盖范围」小节）。incremental 复用的条件从「blob 自验通过」收紧为
   「blob 自验通过**且** `ValidatedHash` 有值且未变」。fingerprint 不变 → 复用且不反射
   （与今天等价，但现在是有依据的）；变了或无值 → 强制重编重校，即使字节码最终逐字节相同。
3. **复用产物时重新反射。** 需要为已存在的 blob 重跑一次 DXC 才能拿到 reflection（blob 里不存
   完整反射，ADR-0003 也不允许存），等于让 incremental 退化成全量编译。否决。

配套约束：

- `ValidatedHash` 是 index 里的 cook 期元数据，**不进 blob、不进 artifact key、不参与内容寻址**，
  因此不违反 ADR-0003「反射数据不落盘」—— 它是 manifest 声明的摘要，不是反射结果
- 两条 lane 各自记录（随 lane index 走），值本身与 lane 无关但不跨 lane 借用
- 该 hash 的 grammar 变化归 lane artifact format version 管，与第 8.2 节一致

##### 字段必须表达「是否真的校验过」，不能只存一个 hash

`--no-validate-reflection` 的构建根本没跑过校验。若它照样写入一枚无条件的 hash，那么后续
开启校验的构建会看到「fingerprint 一致」→ 判定可复用 → 校验依旧不跑。manifest 从未被核对过，
系统却认为核对过 —— 这比不加 fingerprint 更糟，因为它给出了虚假的已验证信号。

故字段是 `std::optional<ShaderHash> ValidatedHash`，语义为「这份 blob 曾在该 manifest 投影下
通过校验」：

- `ValidateReflection == true` 且校验通过 → 写入当前 fingerprint
- `ValidateReflection == false` → 写 `nullopt`，**不写 fingerprint**
- 复用条件：blob 自验通过 **且** `ValidatedHash` 有值 **且** 等于当前 fingerprint
- `nullopt` 一律不复用（在本次开启校验时会重编重校；本次仍关闭校验则重编但仍写 `nullopt`）

代价说清楚：连续两次 `--no-validate-reflection` 构建之间不再有 incremental 复用。这是可接受的 ——
关校验本就是「我知道自己在跳过一道关」的显式选择，而 CI 与发布构建都开着校验。反过来若为了
保住这种复用而写入 fingerprint，就等于让一个从未验证的产物冒充已验证。

这也决定了序列化形态：`ValidatedHash` 在 index JSON 里是**可选成员**。缺字段按 `nullopt` 解析，
于是「旧 index / 手写 index / 关校验产出的 entry」三者天然都落到 fail-closed 一侧。

##### fingerprint 的旧值从哪里来：cook 必须显式加载 lane index

现有 cook **完全不读旧 index**，它只用 `ReadShaderArtifactBlob` 探测 blob 是否存在且自验通过 ——
`LoadShaderArtifactIndex` 在整个 cook 路径上没有调用点。而 `ValidatedHash` 存在 index 里，
所以「比较旧 fingerprint」这件事**需要新增一次 index 加载**，不能假装现成。

规则：

- cook 开始时按 lane 加载 `<lane>/index.json`，作为只读的旧状态
- 以下情形一律 **fail closed 到重新编译**（不是报错，也不是复用）：
  lane index 不存在 / 解析失败 / 版本不符 / 该 key 无 entry / entry 的 `ValidatedHash` 为 `nullopt`
- 旧 index 只用于取 `ValidatedHash`，不用于取 `BlobPath`（那仍由 key 与 lane directory 推导），
  也不影响新 index 的内容 —— 新 index 始终从本次 cook 的结果重新构建
- 加载旧 index 失败不进 `Diagnostics`：那只意味着「无法增量」，不是错误

##### fingerprint 覆盖范围必须与实际校验口径逐项对齐

第 2 个候选里那句「`VertexInput` 的 attribute location/semantic 集合」过粗，会漏掉真正会改变
校验结果的改动。以两条 lane 实际比的东西为准：

- DXIL 侧 `ValidateShaderReflection` 比的是 **base semantic + effective semantic index**
  （`SplitSemantic` + `EffectiveSemanticIndex`，大小写不敏感），所以 `SemanticIndex` 必须进 fingerprint；
  改 `TEXCOORD0` → `TEXCOORD1` 而不进 fingerprint 就会静默复用
- SPIR-V 侧比的是 **location**，且 location 在 `Location` 为 `nullopt` 时**由 attribute 在数组中的
  声明序号决定**（`attribute.Location.value_or(index)`）。因此 fingerprint 必须包含
  attribute 的**声明顺序**，或直接包含逐条算出的 effective location —— 只重排 attributes
  就能改变全部 effective location，而集合本身没变
- 显式 `Location` 与「靠顺序推导」两种形态要能区分，否则 `[A(loc=1), B(loc=0)]` 与
  `[B, A]` 会算出同一 fingerprint 而实际 location 分配不同

据此 fingerprint 的 `VertexInput` 部分按 attributes 的**声明顺序**逐条累加
`(base semantic, effective semantic index, effective location, 是否显式 Location)`，不做排序、不去重。
binding 与 push constant 部分同理按声明顺序累加，避免「只调整声明顺序」被判为无变化 ——
即使顺序对 binding 校验当前无影响，让 fingerprint 严于校验是安全方向（多重编，不漏校验）。

---

## 9. resolver 与 runtime 单 lane 化

### 9.1 `ShaderResolveContext`

构造时新增一个 `ShaderBlobCategory target`，只接受 DXIL/SPIRV，并据此固定：

- lane index 路径
- lane toolchain hash
- AOT blob reader
- JIT compile function

公开只读 `ShaderResolveContext::GetTarget()`；`ShaderResolver::GetTarget()` 与
`ShaderPassProgram::GetTarget()` 只沿持有链转发这个值，不各自存第二份 target。PSO cache 自己固定的
target 是 composition root 同时注入的防御性 expected value，不是 program target 的来源；两者核对的
仍是 composition root 作出的唯一 backend→lane 选择，cache 不得自行重新映射。

`ShaderResolver::Resolve`、`LoadFromArtifact`、`CompileWithJit` 删除 category 参数。调用期间发现 index、
blob 或 typed output 的 lane 与 context 不一致是产物损坏/内部不变量错误，不回退到另一条 lane。

#### 已知代价：双 lane 时源码缓存翻倍

`ShaderResolveContext` 同时是**与 lane 无关**的文件级 include 闭包缓存宿主。
`architecture/shader-pipeline.md` 专门论证过「缓存在文件层，不在 entry 层」，理由是闭包高度
重叠：error_pass 的 8 个文件是 forward_pass 那 15 个的子集。把 target 钉进 context 意味着
第 9.2 节要求的双 backend fixture 会各持一份缓存，那份重叠闭包被读两遍、扫两遍、哈希两遍。

更便宜的位置是 `ShaderResolver`（本就一份 manifest 一个，也满足决策 6 的“选定后不再逐次携带
category”），它能让两条 lane 共享单份源码缓存。**仍然选 context**，理由是 lane 决定了 AOT blob
reader 与 JIT compile function，把它放在 resolver 会让同一个 context 同时持有两条 lane 的
toolchain hash，而第 8.2 节刚把 toolchain hash 拆成 lane-owned。

代价的实际范围有限：生产 runtime 对一个 device 只建一个 context，只有测试会同时开两条 lane。
这是本文唯一一处与既有设计论证正面相撞的裁决，ADR-0014 必须把它记成显式代价与放弃的
更便宜位置，不能留白。

### 9.2 `ShaderPassProgram`

删除以下逐次 category 状态：

- `GetOrCreateVariant(..., category, ...)` 的 category 参数
- `GetOrCreateDefaultVariant(..., category, ...)` 的 category 参数
- `GetOrResolveBytecode(..., category, ...)` 的 category 参数
- `VariantEntry::Category`

同一 program 的 resolver context 已固定 lane，因此 `(variant)` 足以唯一标识 variant cache。
测试若要覆盖两个 backend，创建两个 context/program fixture，不在一个 program 中来回切 category。

`PipelineStateCache::GetOrCreateGraphics` 也删除 category 参数；它是当前逐次 target 传递链的最后一环，
不能只改 resolver/program。`RenderSystem` 用同一次 backend→lane 选择结果构造
`PipelineStateCache(device, target)` 与 `ShaderResolveContext(..., target)`；cache 建 PSO 前核对
`key.Program->GetTarget() == target`，不匹配时在创建 shader 前失败。cache 不自行调用
`Device::GetBackend()` 再做第二份映射。

`ShaderProgramVariant` 不再返回公共 `ShaderVertexInterface*`。通过 typed `ShaderBytecode` payload
访问，或提供两个不会丢类型的便利入口：

```cpp
Nullable<const DxilVertexSignature*> FindDxilVertexSignature() const noexcept;
Nullable<const SpirvVertexInterface*> FindSpirvVertexInterface() const noexcept;
```

wrong-lane accessor 返回空；存在对应 lane 的 vertex stage 却缺 metadata 仍 fail fast。不得新增一个
把两者抹平成公共列表的 `FindVertexInterface`。

---

## 10. RHI descriptor 也必须 typed

把 `ShaderDescriptor { Source, Category, Stages }` 改为 typed alternatives：

```cpp
struct DxilShaderDescriptor {
    std::span<const byte> Bytecode;
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

struct SpirvShaderDescriptor {
    std::span<const uint32_t> Words;
    ShaderStages Stages{ShaderStage::UNKNOWN};
};

using ShaderDescriptor = std::variant<DxilShaderDescriptor, SpirvShaderDescriptor>;
```

`MakeShaderDescriptor` visit `ShaderBytecode` payload构造对应 alternative。D3D12 与 Vulkan backend
仍对 wrong alternative 做防御性拒绝，但不再检查一枚可与 source 内容矛盾的 `Category`：

- D3D12 只消费 `DxilShaderDescriptor`
- Vulkan 只消费 `SpirvShaderDescriptor`
- Vulkan 的 `% 4` 与 pointer alignment 已在 bytes → words 边界保证，RHI 直接使用 word span

本轮只改变 shader module 创建数据形状，不提前实现 primitive vertex layout 连接。

一处将来会撞上的地方，本轮不处理但要知道：`radray_imgui_shader.cpp` 的
`GetImGuiVertexShaderSPIRV` / `GetImGuiPixelShaderSPIRV` 返回 `span<const byte>`（内嵌数组已
`alignas(4)`），而 typed descriptor 要 word span。该文件在 `RADRAY_ENABLE_IMGUI` 下确实参与编译，
但这两个 getter **当前没有任何调用点**，不会碰到 `CreateShader`，所以不在本轮改动面内。
接上 imgui 渲染时需要在那一侧做 bytes → words，不要因此给 `SpirvShaderDescriptor` 加一个收
bytes 的便利构造 —— 那会把本节刚消除的「同一 descriptor 有两种输入形状」重新引回来。

---

## 11. 测试替换清单

### 11.1 typed DXC

- DXIL 入口只返回 object + optional reflection，SPIR-V 入口只返回 words
- 有/无 `DXC_OUT_REFLECTION` 分别映射为 engaged/`nullopt`，需要反射的调用点显式检查
- 不存在可由 `IsSpirv` 与 output category 互相矛盾的状态
- 不存在 `ArgsData` / `ParseArgs` 从 `-spirv` / `-metal` 反推 output category 的第二真相源
- SPIR-V 编译产物不含 `SPV_GOOGLE_hlsl_functionality1` /
  `SPV_GOOGLE_user_type` `OpExtension`
- SPIR-V 编译产物不含 `HlslSemanticGOOGLE` / `UserTypeGOOGLE` decoration
- 非 4 字节 SPIR-V output 在 DXC 边界被拒绝

不要只 grep 编译参数；测试解析实际 SPIR-V instruction/string operand，证明扩展没有进入 module。

**该扫描必须测试内自实现。** spirv-cross 对 `radrayshader` 是 `PRIVATE` 链接，头文件路径不传播
给 `test_shader_asset`，测试无法直接调 `get_declared_extensions()`。写一个几十行的 word 扫描器
即可：跳过 5 个 word 的 header，按 `(wordCount << 16) | opcode` 遍历，检查 `OpExtension`(10) 的
literal string 与 `OpDecorateString`(5632) / `OpMemberDecorateString`(5633) 的 decoration 号。
不要为了这个测试把 spirv-cross 提成 `PUBLIC` —— 那会让 CLI 的链接边界跟着变。

### 11.2 lane reflection/metadata

DXIL：

- semantic/index 大小写与 suffix 规则
- system value 过滤
- register/component type/stream/mask/read-write mask 原样保存
- 未使用参数仍按 DXIL signature 保留
- 数组/矩阵展开结果按 DXIL signature 原样保存

SPIR-V：

- 空 `DeclaredName`（含真实 strip 产物）仍能按 location/type 成功提取
- built-in 过滤、location 排序/空洞/重复拒绝
- scalar/vector/matrix/fixed-array shape round-trip；artifact 数组维度只含 literal length，不含 kind
- 非数值、坏 `TypeIndex`、runtime/specialization-constant array 显式失败
- 无 `HlslSemantic` 字段与 `in.var.` fallback 测试
- **type cache 身份**：同一 module 内同时含 `float4` / `float4[2]` / `float4[3]`，三者维度序列
  各自正确且互不污染；另一例把 runtime-sized 与 fixed-size 数组放进同一 module。
  这两条针对第 6.2 节那个 `arraybase.self = base.self` 造成的碰撞，单形状 module 测不出来

删除全部 DXIL↔SPIR-V 子集/semantic equality 测试；它们验证的是被废弃的统一投影。

### 11.3 validation 与 name stripping

- SPIR-V `DeclaredName` 为空时，set/binding/type/count/range 校验仍通过
- 相同条件下位置或类型错误仍失败
- SPIR-V `DeclaredName` 非空且与 manifest 不符时失败，诊断含 set/binding
- **`DeclaredName` 不做 fallback 的回归**：拿一份真实 strip 过 `OpName` 的 SPIR-V（`-Qstrip_debug`
  或等价），断言 UBO 的 `DeclaredName` 为空而 `DisplayName` 是 SPIRV-Cross 的合成名，
  且资源校验通过。这一条守住第 7 节那个坑：若 `DeclaredName` 保留回退，strip 后每条 cbuffer
  都会误报 mismatch。**不允许只用手工构造的 `SpirvShaderDesc` 覆盖这一项**
- **系统性偏移负例**：两条同 type / 同 Count / 同 stage 的绑定（如 forward_pass 的
  `gShadowCube` 与 `gShadowArray`）binding 整体偏移 1 时，带 `DeclaredName` 的输入必须失败；
  同一输入在 `DeclaredName` 为空时按纯位置检查通过。这两条断言合起来正是第 7 节
  「lint 可被 strip 关闭」的定义，测试注释里要写明这是**刻意的强度差**，不是漏洞
- DXIL name mismatch 继续由 DXIL policy 报错
- shader template 继续用 DXIL 名字生成 binding/vertex 初稿；authoring SPIR-V probe 保留 `OpName`，
  继续关联并提升 push constant
- authoring probe 的 push constant 名为空或无法匹配 DXIL cbuffer 时显式失败，不生成悄悄错误的 manifest

### 11.4 blob/index

- DXIL 与 SPIR-V typed payload 各自 round-trip、损坏和 trailing-data 测试
- wrong target、wrong lane version、header/payload hash 篡改被拒绝
- SPIR-V `wordCount` 与 `arrayDimensionCount` 的边界检查先于分配（两者都是显式 count）
- `ShaderResolveContext` 的记忆化闭包哈希与 `ComputeShaderSourceIdentity` 对同一源文件给出
  相同 hash —— 守住第 8.2 节那对必须同步的累加序列
- `dxil/index.json` 与 `spirv/index.json` 独立加载
- lane index 的 absolute/rooted/`..` `BlobPath` 被拒绝，合法文件名仍限制在 lane directory 内
- 人为只递增 SPIR-V version/toolchain hash 时，DXIL index/blob 仍可命中
- **validation fingerprint**：只改 manifest 的 binding name/type/count/stage（HLSL 与所有 key 输入
  不动）后再 cook，`ValidatedHash` 变化使该 entry 不复用并重跑校验，manifest 与 HLSL 真不一致时
  cook 失败；改动无害时重编后 entry 仍收敛到同一 blob
- **fingerprint 的 vertex 口径**：只改 `SemanticIndex`（`TEXCOORD0` → `TEXCOORD1`）、
  只重排 attribute 声明顺序（集合不变、effective location 全变）、以及
  `[A(loc=1), B(loc=0)]` 与 `[B, A]` 两种形态，四种改动都必须使 fingerprint 变化。
  这几条正是「按集合算」会漏掉的
- **未验证状态**：`--no-validate-reflection` 构建产出的 entry 其 `ValidatedHash` 为 `nullopt`；
  随后开启校验的构建**不复用**该 entry 而是重编重校。连续两次关校验的构建之间不复用是预期行为
- **fail closed**：lane index 不存在 / JSON 损坏 / 版本不符 / 该 key 无 entry / entry 缺
  `ValidatedHash` 成员，五种情形都退化为重新编译且不产生 diagnostic
- **原子发布**：blob 与 lane index 写入失败后，目标路径保持旧内容（不被截断），旧 index 与旧 blob
  仍配对可用；临时文件不残留。用注入的写入失败或只读目标文件构造，不能只测 happy path
- 所选 lane index 缺失而旧根 `index.json` 存在时，resolver 报迁移错误且即使允许 JIT 也不兜底
- 旧 blob magic/version 由 reader 显式报不兼容
- incremental reuse 不跨 lane
- `ShaderAssetSampleTest` 的真实 forward/error manifest 按 lane 断言各自 `index.json` 与 blob 路径
- `MSL` / `METALLIB` cook 请求只产生 `CommonDiagnostics`，不创建伪 lane result

### 11.5 resolver/runtime/RHI

- context 构造时固定 target，`Resolve`/program API 不再收 category
- context 是 resolver/program target 的唯一来源；两者的 `GetTarget()` 只转发，不复制状态
- PSO cache 只保存 composition root 同时注入的 expected target 用于核对，不自行映射或决定 lane
- `PipelineStateCache::GetOrCreateGraphics` 不再收 category，cache target 与 program target 不一致时失败
- D3D12/Vulkan 两套 fixture 分别得到正确 typed bytecode
- AOT/JIT 在同一 lane 返回相同 metadata
- variant 与 bytecode cache 去重不再含 category
- backend→lane 映射穷尽 D3D12/Vulkan，且仓库只有 runtime composition root 一处
- `MakeShaderDescriptor` 产生正确 alternative；两个 backend 都拒绝 wrong alternative
- DXIL/SPIR-V vertical slice 的 JIT/AOT 像素读回继续通过

去 category 的调用面**以测试为主**，评估风险时不要按“热路径”估量：
`PipelineStateCache::GetOrCreateGraphics` 目前只有测试在调用，生产侧只有 `RenderSystem` 构造过
该 cache 而没有调用点；`GetOrCreateVariant` 的唯一生产调用点在 `PipelineStateCache` 内部。
真正需要逐个改签名的密集区是 `test_pipeline_state_cache.cpp` 与 `test_shader_program.cpp`。

### 11.6 layering

- `radrayshader` 不链接 `radrayrender`
- `shader_gen` / `shader_cook` 不链接 runtime/render
- 用 `ninja -C build_debug -t commands` 或 link map 核对，不用 `dumpbin /DEPENDENTS`

---

## 12. 文档与 ADR

实施代码时同步完成，不提前把 architecture 文档写成尚不存在的现状：

1. 新增 **ADR-0014 backend-specialized shader lanes**。
   它只完整取代 ADR-0013，重述 vertex artifact、typed lane、name stripping 与 runtime 单 lane 装配。
   它对 ADR-0003 的局部收窄仅是“vertex artifact 可保存 lane-specific 最小 metadata”，不改变
   manifest 的资源 ABI 权威、七项反射缺口与 shader_gen 默认值；对 ADR-0006 的局部收窄仅是允许
   runtime composition root 做一次 `RenderBackend -> ShaderBlobCategory` 映射，不改变类型归属和模块
   依赖边界。

   **必须接手 ADR-0013 的异常遗留登记。** ADR-0013 末条把 `spvc.cpp` 两处局部
   `throw spirv_cross::CompilerError` 记为已知遗留、独立后续清理项。ADR-0013 整体失效后，
   若 ADR-0014 不重述这条，仓库就失去该遗留的唯一记录，而本文第 16 节的“没有新增
   `try`/`catch`/`throw`”会被读成“spvc 已无异常控制流”。两处之一（递归深度上限）正位于第 6.2 节
   要改的 `_ReflectType` 内，本轮不清理它，但必须继续可见。同时把 ADR-0014 的记录更新为按
   函数而非行号定位 —— 行号已随本轮改动漂移。
2. 新增 **ADR-0015 shader artifact 按 lane 独立索引和失效**。
   它取代 ADR-0004，保留同目录、内容寻址、无 `--output`、逐 source identity 等裁决，只把根
   `index.json`、全局 version/toolchain 改成 lane-owned。
3. ADR-0013 状态改为“已被 ADR-0014 取代”，ADR-0004 状态改为“已被 ADR-0015 取代”；ADR-0003
   与 ADR-0006 继续生效，不能因为一条局部例外把各自主体裁决标成整体失效。
4. 按 ADR-0003 已存在的“后续收窄”体例处理局部 supersede：把该附录从 ADR-0013 重定向到
   ADR-0014，并只更新 vertex metadata 例外；在 ADR-0006 末尾追加指向 ADR-0014 的映射例外。
   两份 ADR 的原始背景、决策、放弃方案和“必须保持为真”正文不改。
5. 更新 `docs/adr/README.md`：加入 0014/0015 与状态，并把规则澄清为“原始决策正文冻结；完整取代
   只改状态，局部收窄只允许追加/重定向一个短附录指向新 ADR”。这一步显式修复 ADR-0003 既有附录
   与当前“只有状态可改”措辞的冲突，不把回写历史正文常态化。
6. 重写 `docs/architecture/shader-pipeline.md` 的归属图、vertex metadata、AOT layout、runtime target
   selection 与测试表，删除“任何一层都没有 backend→category 映射”的现状描述。
7. 更新 `docs/guide/shader-authoring.md` 的 cook 输出布局与 `--category` 说明。
8. 保留 `docs/todo/vertex-interface-projection.md` 作为已实施历史，但明确其“下一阶段”已由本文取代。

---

## 13. 文件变更清单

| 文件 | 改动 |
|---|---|
| `modules/shader/include/radray/shader/dxc.h` | typed DXIL/SPIR-V output 和 compile 入口；删除 `IsSpirv` / output category |
| `modules/shader/src/dxc.cpp` | 分离参数构建与输出转换；删除 `ArgsData` / `ParseArgs` / `-fspv-reflect` 及死 `isStripRefl` |
| `modules/shader/include/radray/shader/hlsl.h` | `kReflectionFormatVersion` 改名 `kHlslReflectionFormatVersion`，值仍为 3 |
| `modules/shader/src/hlsl.cpp` | HLSL JSON writer/reader 使用 HLSL 专用 schema version |
| `modules/shader/include/radray/shader/spirv.h` | 删除 HLSL decoration 字段；`Name` 拆成 `DeclaredName` / `DisplayName`；新增 `kSpirvReflectionFormatVersion = 4`、`SpirvArrayDimension`、逐维 `ArrayDimensions` |
| `modules/shader/src/spirv.cpp` | 同步反射 JSON codec；改用 SPIR-V 专用 version，去掉仅为版本号而 include 的 `hlsl.h` |
| `modules/shader/include/radray/shader/spvc.h` | 所有 SPIR-V view/转换入口改收 typed word span |
| `modules/shader/src/spvc.cpp` | 删除 Google decoration 依赖；`_ReflectType` 的 cache key 从 `self` 改成精确 type ID（修数组身份碰撞）并逐维读 `array`/`array_size_literal`；`_ProcessResource` / `_ReflectStageIoValue` / `_ReflectPushConstants` 停止把 fallback 名写进 declared name |
| `modules/shader/include/radray/shader/shader_manifest.h` | 两套 vertex metadata、typed payload、lane index/version/context API；`ShaderArtifactEntry` 增加 `std::optional<ShaderHash> ValidatedHash` |
| `modules/shader/src/shader_manifest.cpp` | lane validator、key/hash、blob/index、cook、AOT/JIT dispatch；按第 8.2 节映射表拆分 8 个 `kShaderArtifactFormatVersion` 站点，含 `ShaderResolveContext` 与 `ComputeShaderSourceIdentity` 那对必须同步的累加序列；cook 新增旧 lane index 加载（fail closed）与 fingerprint 计算；产物发布改用原子写入 |
| `modules/core/include/radray/file.h`, `modules/core/src/file.cpp` | 新增 `WriteFileAtomic`（同目录临时文件 + `rename`）；`WriteBinaryFile` / `WriteTextFile` 语义不变 |
| `modules/shader/src/shader_reflection_map.h/.cpp` | 公共 binding comparison 收窄，DXIL/SPIR-V policy 分开（SPIR-V 保留 name 非空时的弱检查） |
| `modules/shader/src/shader_asset_template.cpp` | typed compile API；DXIL 名字权威、SPIR-V 只补 push-constant shape |
| `tools/shader_cook/shader_cook.cpp` | lane-local result/index/stats 与新目录文案 |
| `modules/runtime/src/render_system.cpp` | 唯一 backend→lane switch，构造固定 target context |
| `modules/runtime/include/radray/runtime/shader_program.h` | program API 去 category；typed vertex metadata 访问 |
| `modules/runtime/src/shader_program.cpp` | typed bytecode cache 与 descriptor visitor |
| `modules/runtime/include/radray/runtime/gpu_resource.h` | PSO cache API 去 category，构造时固定 target |
| `modules/runtime/src/gpu_resource.cpp` | 删除逐次 category 传递，核对 program/cache target |
| `modules/render/include/radray/render/rhi.h` | DXIL/SPIR-V descriptor alternatives |
| `modules/render/src/d3d12/d3d12_impl.cpp` | 只消费 DXIL descriptor |
| `modules/render/src/vk/vulkan_impl.cpp` | 只消费 SPIR-V descriptor；删除两个 `VK_GOOGLE_*` 扩展 |
| `modules/shader/tests/test_shader_asset.cpp` | blob/index/cook/resolver 与真实 `ShaderAssetSampleTest` 改为 lane layout |
| `modules/shader/tests/test_shader_asset_template.cpp` | authoring probe 保名、push constant 关联与 typed compile API |
| runtime/render tests | 用第 11 节 lane-specific 测试替换公共 projection 测试 |
| ADR/architecture/guide | 按第 12 节同步 |

实施时用 grep 补齐调用点；本表不是删除未列文件的理由。尤其要检查 `shader_asset`、pipeline cache、
vertical slice 和测试 fixture 中手工传递 `ShaderBlobCategory` 的位置。

按现状清点，`ShaderBlobCategory` 的绝大多数出现在测试里（`test_shader_asset.cpp` 一处独大），
生产侧 runtime 只有个位数。改动量的重心在测试而非运行时热路径，见第 11.5 节末尾。

---

## 14. 实施顺序

1. 写真实 SPIR-V 探针测试：无 `-fspv-reflect` 时 location/type、资源绑定、push constant 的实际形状，
   并扫描 module 确认无两个 Google extension/decoration。
2. **原子迁移 typed compiler/reflection 边界**：引入 typed DXC 入口，把 `spvc.h` 与全部 SPIR-V
   调用方同步改成 word span，并迁移 shader_gen/cook/JIT；删除 `IsSpirv`、`DxcOutput::Category`、
   `ArgsData` / `ParseArgs`、`-fspv-reflect` 与 Vulkan 两个扩展。DXIL reflection 同步改成 optional，所有
   需要反射的调用点显式处理；同一步清理 `SpirvShaderDesc` 的 HLSL decoration 字段、reflection schema
   version 与 cook/JIT/runtime 对 vertex semantic 的 name 依赖，保留第 5.2 节明确列出的 authoring-only
   `OpName` 关联与第 7 节的 resource name lint。不能先让 DXC 返回 words、再让 `spvc` 暂时退回 bytes。

   **本步必须保留 `in.var.` fallback，它到步骤 4 才删。** `SpirvVertexInterface` 在步骤 4 才定义，
   所以步骤 2 结束时跑的仍是现有 `ExtractVertexInterface(const SpirvShaderDesc&)`，而它唯一的语义
   来源就是 `HlslSemantic` 与 `in.var.` 这两级 fallback。若在本步同时删掉两者，SPIR-V vertex stage
   在 cook 与 JIT 两条路径都会无条件返回 nullopt —— 那不是“原子删除旧路径”，是让 SPIR-V vertex
   烘焙在两个提交之间整体不可用。

   `in.var.` 前缀与 `-fspv-reflect` 无关：DXC 无条件给 stage IO 变量起这个名字，删掉
   `-fspv-reflect` 只会让 `HlslSemantic` 恒空、fallback 恒生效。故本步的精确边界是：

   - 删 `-fspv-reflect`、`SpirvStageIo::HlslSemantic` 及其 codec、`DecorationHlslSemanticGOOGLE` 读取、
     `HlslType` / `DecorationUserTypeGOOGLE`、`HlslRegister` / `HlslSpace`、两个 `VK_GOOGLE_*` 扩展
   - 「清理 name correctness 依赖」**只指 vertex semantic 与 HLSL decoration**，不含第 7 节保留的
     resource name lint。同一步要做的是把 `Name` 拆成 `DeclaredName` / `DisplayName` 并让 lint 只
     消费前者 —— 那是把依赖收窄到可 strip，不是删除
   - `ExtractVertexInterface(SpirvShaderDesc)` 的两级 fallback **降为一级**：只剩 `in.var.`。
     “两级都取不到名字即失败”是现状行为，保持不变，只是失败条件从“两级都不可用”收紧成
     “`in.var.` 不可用”
   - `ValidateShaderReflection(SpirvShaderDesc)` 的 vertex 诊断改读 `Name`，不再读 `HlslSemantic`
   - 本步注释里写明该 `in.var.` 依赖是**步骤 4 之前的临时唯一路径**，不是长期契约

   同一步还需把 `ShaderBytecode::Category` 的赋值来源从 `output->Category` 改成“本次请求的
   target”，因为 `DxcOutput::Category` 在本步被删；字段本身到步骤 4 才随 variant 化消失。

   **本步必须同时让旧 SPIR-V 产物失效，否则会构造出不可加载的组合。** 现有
   `GetShaderToolchainHash` 只含 DXC version 与 artifact format version，**不含编译参数策略**；
   `ComputeShaderArtifactKey` 也不含。于是删掉 `-fspv-reflect` 后，新旧 SPIR-V 的 artifact key
   逐位相同，incremental 会命中仍带 `SPV_GOOGLE_*` 声明的旧 blob，而本步刚把两个
   `VK_GOOGLE_*` 从 device extension 集合里删掉 —— 结果是在一个未启用这两个扩展的 device 上
   加载声明了它们的 module。这不是「产物过期」，是本轮亲手造出来的非法组合。

   本步引入一枚**临时 toolchain epoch** 并入 `GetShaderToolchainHash` 的哈希材料（例如
   `|argpolicy=<n>`），本步递增一次。它在步骤 4 被 lane-owned toolchain hash 取代 ——
   彼时「对应 lane 编译参数策略」已按第 8.2 节进入两个 `Get*ShaderToolchainHash`，epoch 随之删除。
   epoch 是第 14 节末尾留存物表的第三项。

   不选「把扩展删除推迟到步骤 4」：那会让步骤 2 到 4 之间的 SPIR-V 既不带 Google decoration
   却仍要求 device 启用扩展，反而多出一个需要解释的中间态；而删 extension 与删 `-fspv-reflect`
   本就是同一件事的两面，拆开没有收益。

   **typed words 在本步无法一路贯通到 RHI，需要一个登记在案的收窄点。** 本步之后 DXC 与 JIT
   已返回 `vector<uint32_t>`，但 `ShaderBytecode::Data` 仍是 `vector<byte>`、`ShaderDescriptor` 仍收
   byte span、两个 backend 仍消费 bytes（都到步骤 4 才动）。所以第 5.1 节末条「不在中途退回通用
   bytes」在本步做不到字面成立。

   裁决：允许**唯一一处** words → bytes 收窄，位置钉死在「把 JIT/cook 的 typed 编译结果写进
   `ShaderBytecode::Data`」那一行，用 `std::as_bytes` 完成并带注释注明删除时机（步骤 4）。
   禁止的做法是让 `spvc` 的入口退回 bytes、让 `ReflectSpirv` 接 bytes、或在 RHI 边界再转一次 ——
   那些会让「哪一层才是 typed 的」重新变得说不清。该收窄是留存物表的第四项。

   合并步骤 2 与步骤 4 也能消掉这个收窄，但代价更大：那个合并提交要同时改 typed DXC、
   `spvc` 全部调用方、两套 vertex metadata、blob envelope、lane index、cook 事务、RHI descriptor
   与两个 backend，规模大到无法分别验证 —— 正是本文一开始拆步骤要避免的情况。
3. 先给 `ShaderResolveContext` 与 `PipelineStateCache` 固定同一 target，增加只转发 context 的 program
   target accessor，并依次移除 resolver/program/asset/PSO cache 调用链的 category 参数。在这个中间
   提交中，根 index 仍是尚未迁移的**当前格式**，可由 context target 过滤 category；下一步切换 lane
   layout 的同一提交会立刻把根 index 定义为 legacy 并改成显式拒绝，不存在“新布局读取旧根 index”
   的过渡状态，也不需要临时新增 `GetToolchainHash(category)` API。
4. **原子迁移 typed data plane**：一次完成 source identity/lane format/toolchain hash 拆分（按第 8.2 节
   的映射表逐项迁移，含那对必须同步的 source identity 累加序列；lane-owned toolchain hash 在此
   吸收步骤 2 的临时 epoch 并删除它）与第 8.4 节的
   `ValidatedHash`（它进 index schema，必须与 lane index 同一提交落地）、旧 lane index 的
   fail-closed 加载、以及产物发布的原子写入（`WriteFileAtomic` 可以更早独立落地，但
   cook 切换到它必须与 lane index 同一提交，因为发布点本身在这一步改形状），定义
   `DxilVertexSignature` / `SpirvVertexInterface`，替换公共提取和 cross-category 测试，并迁移 blob
   envelope/payload reader/writer、lane-local index、cook transaction、`ShaderArtifactBlob`、
   `ShaderBytecode`、`MakeShaderDescriptor`、RHI descriptor 与两个 backend；同一步加入 legacy-root
   显式探测并迁移 `ShaderAssetSampleTest`。该步结束时删除 `ShaderVertexInterface`、bytecode
   `Category`、根 index、全局 format/toolchain 旧路径、toolchain epoch、那处 words → bytes
   收窄（typed payload 与 RHI descriptor 同时到位后它无处可留），以及步骤 2 留下的 `in.var.` 临时依赖 ——
   `SpirvVertexInterface` 不含 semantic，提取改为只读 `Location` 与类型形状，名字彻底退出该路径。
   不能先删 metadata 类型后留下 blob codec 引用，不能先切 `ShaderBytecode` variant 后让
   `MakeShaderDescriptor` 继续读 `bytecode.Category`，也不能先启用 lane-local hash 后让单根 index
   尝试保存两条 lane 的 hash。
5. 新增 ADR-0014/0015，按第 12 节处理完整取代与局部收窄，更新 architecture、authoring guide 与
   本 todo 状态。
6. 完整构建后，严格顺序运行 shader、template、layout、program/PSO、vertical slice 测试；构建与
   测试不得并发。

每个边界在**迁移该边界的步骤**原子删除旧路径，不保留“新 typed API + 旧 bool/category API”双轨
兼容层。步骤 3 只迁移 target 传播，尚未迁移 artifact layout，所以根 index 在该提交仍是唯一当前
格式而非 compatibility reader；步骤 4 才在同一提交发布 lane layout 并删除根格式。若需要让中间
commit 可构建，在对应步骤内迁移全部调用方，不长期保留 adapter，也不让新旧两套线格式并存。

“原子删除旧路径”约束的是**同一个边界不留双轨**，不是“任何旧代码都必须在最早的那一步消失”。
两处刻意的跨步骤留存已在上文点名，都必须带注释说明何时删除：

| 留存物 | 存活区间 | 删除于 | 若提前删 / 不加的后果 |
|---|---|---|---|
| `in.var.` 名字 fallback | 步骤 2 → 4 | 步骤 4 随 `SpirvVertexInterface` | SPIR-V vertex cook/JIT 在两个提交之间整体失效 |
| `ShaderBytecode::Category` 字段 | 步骤 2 → 4 | 步骤 4 随 typed payload variant | 步骤 2 的 blob codec / descriptor 失去 target 来源 |
| toolchain epoch（`argpolicy`） | 步骤 2 → 4 | 步骤 4 随 lane-owned toolchain hash | 不加则旧 SPIR-V blob 命中，在已删 `VK_GOOGLE_*` 的 device 上加载声明了它们的 module |
| words → bytes 收窄（单点） | 步骤 2 → 4 | 步骤 4 随 typed payload / RHI descriptor | 不允许它就等于要求合并步骤 2 与 4，那个提交大到无法分别验证 |

区别在于：这四者在其存活区间内都是**唯一路径**，不与任何新 API 并存，因此不构成双轨兼容层。
反例（禁止）是让 `DxcOutput::Category` 与 typed 入口同时存在、让根 index 与 lane index 同时可读、
或让 words → bytes 的转换散落在多处。

---

## 15. 验证命令

```powershell
python tools/check_docs.py
cmake --build build_debug --parallel 24
ctest --preset win-x64 -R "ShaderAssetTest|ShaderArtifactTest|ShaderResolverTest|ShaderAssetSampleTest" --output-on-failure
ctest --preset win-x64 -R "ShaderAssetTemplateTest|ShaderKeywordPragmaTest" --output-on-failure
ctest --preset win-x64 -R ShaderLayoutBindingTest --output-on-failure
ctest --preset win-x64 -R "ShaderAssetIdTest|ShaderAssetLoadTest" --output-on-failure
ctest --preset win-x64 -R PipelineStateCacheTest --output-on-failure
ctest --preset win-x64 -R VerticalSliceTest --output-on-failure
ninja -C build_debug -t commands
```

额外做源码级闭环检查：

```powershell
rg "fspv-reflect|HlslSemantic|DecorationHlslSemanticGOOGLE|DecorationUserTypeGOOGLE|VK_GOOGLE_HLSL_FUNCTIONALITY_1_EXTENSION_NAME|VK_GOOGLE_USER_TYPE_EXTENSION_NAME" modules tools docs/architecture docs/guide
rg -U "(?s)GetOrCreate(?:Default)?Variant\([^)]*ShaderBlobCategory|GetOrResolveBytecode\([^)]*ShaderBlobCategory|GetOrCreateGraphics\([^)]*ShaderBlobCategory|Resolve\([^)]*ShaderBlobCategory" modules
rg -n "ShaderBlobCategory" modules/runtime/include modules/runtime/src
```

`check_docs.py` **当前不是干净门禁**：`AGENTS.md` 与 `README.md` 都引用了不存在的
`docs/guide/build-test.md`，实施前该脚本就报 4 个问题。要么先补上那份缺失的 guide，要么在本轮
只比较问题数是否相对该基线增加 —— 不要把「脚本非零退出」直接当成本轮引入的回归。

第一条 `rg` 在实施后的现状文档/代码中应无命中；历史 ADR/todo 可保留过去时记录。第二条应无命中。
第三条只允许 target 的存储/accessor、composition-root 映射和明确的 lane invariant，不允许逐次选择
参数；逐项人工核对。测试 fixture 与 shader cook 的显式 target 输入不在这条 production-runtime
allowlist 检查内。再用 `git diff --check` 检查补丁。

步骤 2 与步骤 4 之间的中间提交上，第一条 `rg` 允许命中 `in.var.`（不在该表达式内，但相关），
但**不允许**命中 `HlslSemantic` 或任何 `GOOGLE` decoration —— 见第 14 节的留存物表。

---

## 16. 完成标准

- DXIL 与 SPIR-V 编译 API、runtime payload、vertex metadata、artifact payload 和 RHI descriptor
  都由类型区分，不靠 `bool IsSpirv` 或公共 `Category` 字段解释
- Vulkan vertex metadata 完全不含 semantic/index/name，strip 过 `OpName` 的产物仍可 cook、validate、load
- 仓库生成的 SPIR-V 不含 `SPV_GOOGLE_hlsl_functionality1` / `SPV_GOOGLE_user_type`
- Vulkan device 创建不再要求两个 `VK_GOOGLE_*` 扩展
- 不存在 DXIL/SPIR-V vertex 接口相等、子集或互相转换规则
- runtime 对一个 device 只绑定一条 lane，resolver / program / PSO cache 的调用链不再接 category
- DXIL/SPIR-V index、format version、toolchain hash 与 incremental reuse 相互独立
- source identity 有独立版本，且记忆化与无缓存两条路径对同一源文件给出相同 hash
- 所选 lane 缺 index 但存在旧根 index 时明确报迁移错误且不 JIT 兜底；旧 blob 也由 reader 明确拒绝，
  通过 `--clean` + 全量重烘迁移，无兼容 reader
- SPIR-V 的 `DeclaredName` 不含 SPIRV-Cross fallback 名；name lint 只消费它，strip 后自动关闭
  而**不是**误报 mismatch，且有真实 strip 产物的回归守住
- SPIR-V 的 name 检查在文档与代码注释里都被称为 opportunistic lint，不被描述成 ABI 校验
- 同一 module 内同 element type 的多种数组形状各自反射正确，type cache 不再按 `self` 碰撞
- incremental 复用受 `ValidatedHash` 约束：只改 manifest binding ABI 时不会静默复用旧 blob；
  改 `SemanticIndex` 或重排 attribute 声明顺序同样不会
- 从未校验过的产物（`ValidatedHash == nullopt`）不会在后续开启校验的构建里冒充已验证
- 旧 lane index 缺失/损坏/缺字段一律 fail closed 到重编，不会被当成「fingerprint 相同」
- blob 与 lane index 都经临时文件 + `rename` 发布；写入失败不留下被旧 index 引用的截断 blob
- 步骤 2 单独提交后，旧 SPIR-V blob 因 toolchain epoch 而失效，不会在已删 `VK_GOOGLE_*` 的
  device 上被加载
- shader CLI 链接边界不变，D3D12/Vulkan JIT/AOT vertical slice 都通过
- 当前 PSO 仍消费 manifest `VertexInput`；本轮没有偷跑 primitive layout 连接
- 没有新增 `try` / `catch` / `throw`。`spvc.cpp` 既有的两处 `throw spirv_cross::CompilerError`
  仍在，本轮不清理，由 ADR-0014 继续登记为遗留 —— 不得把本条读成“spvc 已无异常控制流”

## 17. 后续阶段（本轮不做）

primitive 连接也按 backend 分开：

```
DXIL:  DxilVertexSignature + PrimitiveVertexLayout
         -> semantic/index match -> D3D12 input layout

SPIR-V: SpirvVertexInterface + PrimitiveVertexLayout
         -> location match -> Vulkan vertex input state
```

两条 linker 可以共用 format compatibility 的纯数值 helper，但不重新发明公共 semantic/location
projection。等两条路径都接通并覆盖真实 mesh 后，再原子删除 manifest `VertexInput` 与
`ShaderVertexInputStorage`。
