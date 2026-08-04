> - 适用: 把当前伪统一的 DXIL/SPIR-V shader 产物改成「共享控制面 + 后端专用 data lane」
> - 权威: 本文是待实施清单；实施后由 ADR-0014/0015 与 shader pipeline 架构文档接管长期约束
> - 状态: 待实施（2026-08）。修正 ADR-0013 的公共 vertex projection 方向，不代表当前代码现状
> - 锚点: `modules/shader/include/radray/shader/dxc.h`, `modules/shader/include/radray/shader/shader_manifest.h`,
>   `modules/shader/src/shader_manifest.cpp`, `modules/shader/include/radray/shader/spirv.h`,
>   `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/src/render_system.cpp`,
>   `modules/render/include/radray/render/rhi.h`

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
- `ShaderProgramVariant` cache key
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
    vector<byte> Reflection;
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
不再以任意 `vector<byte>` 表示；DXIL 也不携带一个可能为空但语义不明的通用 `Refl` 字段。

typed words 必须贯穿现有 SPIR-V API，不能只改 DXC 返回值：

- `SpirvBytecodeView::Data` 改为 `std::span<const uint32_t> Words`
- `ReflectSpirv`、`ConvertSpirvToMsl` 与 `SpirvAsMslReflectParams` 都接 word span
- SPIRV-Cross 构造器直接消费 `Words.data()/size()`，不再在内部 bit-cast 任意 byte pointer
- hash 或写文件需要 byte view 时，只在边界用 `std::as_bytes(std::span{words})`
- blob writer/reader、JIT result 与 RHI descriptor 都保持 typed words，不在中途退回通用 bytes

### 5.2 参数构建

拆成 `_BuildCommonCompileArgs`、`_BuildDxilCompileArgs`、`_BuildSpirvCompileArgs`：

- common：HLSL version、优化、include、define、`-all_resources_bound`
- DXIL：debug/reflection output 的 DXIL 专用参数
- SPIR-V：`-spirv` 及真正属于 Vulkan lane 的参数
- **SPIR-V 参数不得包含 `-fspv-reflect`**

保留/剥离 `OpName` 是 debug-size 策略，不是 vertex ABI。authoring 工具可以保留名字以改善模板和
诊断；cook/JIT 即使未来增加 `-Qstrip_debug`，SPIR-V vertex 提取和运行时校验也必须保持正确。
本轮不必为了证明解耦而强制所有 authoring 编译剥名，但测试必须覆盖空 `Name`。

---

## 6. 两套 vertex metadata

删除 `ShaderVertexScalarType`、`ShaderVertexParameter`、公共 `ShaderVertexInterface` 与两个同名
`ExtractVertexInterface` overload，改成不会互相伪装的类型。

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

先在 `spirv.h` 定义不会丢失 SPIRV-Cross `array_size_literal` 的原生维度：

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

vertex artifact 再引用这个 SPIR-V lane 类型：

```cpp
struct SpirvVertexType {
    render::SpirvBaseType BaseType{render::SpirvBaseType::UNKNOWN};
    uint32_t VectorSize{1};
    uint32_t Columns{1};
    vector<render::SpirvArrayDimension> ArrayDimensions;
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

提取规则：

- 跳过 `BuiltIn`
- `Name` 可以为空，且不参与成功条件、排序、唯一性或 vertex artifact payload；原始 reflection
  debug JSON 可以保留这个可选诊断字段
- 只读取 `Location` 与 `TypeIndex` 指向的数值类型形状
- 按 `Location` 排序并拒绝重复 location
- 保留 scalar width、vector size、column count 与固定数组维度，不做 semantic 展开
- 非数值/struct/runtime-sized/specialization-sized vertex input 显式失败
- 矩阵和固定数组不在提取时伪装成多个 semantic；未来 Vulkan primitive linker 按 location
  consumption 规则展开或给出不支持诊断
- 不套用 D3D12 的 32 项上限；reader 仍须先按 payload 剩余字节验证 count，避免恶意 reserve

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
- 删除 `ExtractVertexInterface(SpirvShaderDesc)` 的 `HlslSemantic` / `in.var.` 两级 fallback
- 删除 `SpirvResourceBinding::HlslType` 及 `DecorationUserTypeGOOGLE` 读取
- 删除当前未被消费的 `HlslRegister` / `HlslSpace` SPIR-V JSON 字段，不保留永远为空的兼容壳
- 删除仅为 HLSL 形状猜测存在且无消费者的 SPIR-V `IsViewInHlsl` 字段/helper；若实施时发现真实
  消费者，先把该消费者改为 SPIR-V 原生规则，不能因此保留 Google extension
- Vulkan device extension 集合删除
  `VK_GOOGLE_HLSL_FUNCTIONALITY_1_EXTENSION_NAME` 与 `VK_GOOGLE_USER_TYPE_EXTENSION_NAME`

上述字段和数组表示会改变 `SpirvShaderDesc` 的版本化 JSON schema。把当前共享的
`kReflectionFormatVersion` 拆成 `kHlslReflectionFormatVersion` 与
`kSpirvReflectionFormatVersion`，本轮只递增 SPIR-V 版本；SPIR-V reader 拒绝旧 schema，不加缺字段
兼容。`SpirvStageIo::Name` 在 reflection JSON 中改为允许空值，但仍可序列化供 authoring/诊断使用。

### resource validation 的名字策略

当前 `MatchReflectedBindings` 同时比较位置、类型和名字，SPIR-V 路径因而仍隐式依赖 `OpName`。
拆成公共 location/type/count 检查与 lane policy：

- DXIL：可继续核对 register/space 处的 HLSL resource name
- SPIR-V：以 set/binding/type/count/stage 为 ABI；name 只在存在时写进诊断，不参与正确性
- SPIR-V push constant：核对 range/size/stage，不要求名字
- `radray_shader_gen` 仍以 DXIL 名字生成 manifest；SPIR-V authoring probe 只补 DXIL 无法识别的
  push-constant shape，不把 SPIR-V name 升格为 runtime ABI

完成后，剥掉所有 SPIR-V `OpName` 只会降低诊断可读性，不会改变 cook/JIT、vertex metadata、
manifest validation 或 runtime shader 创建结果。

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
    inputs[]       location + baseType + vectorSize + columns + repeated array dimension (kind + value)
wordCount
words[]            u32
```

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

`ShaderArtifactBlob` / `ShaderBytecode` 只在 envelope 保存 `Key`、`Stage`、`Source`、`ContentHash` 等公共
字段；target 由 variant alternative 唯一决定。不得再同时保存 `Category` 制造第二个可冲突真相。

### 8.4 cook 事务边界

`CookShaderAsset` 继续共享 pass/variant/stage 遍历，但在 category 外层创建 lane-local result/index。
公共返回类型同步改成可表达多条 lane，而不是继续暴露一份含混的 `result.Index`：

```cpp
struct ShaderCookLaneResult {
    render::ShaderBlobCategory Target{render::ShaderBlobCategory::DXIL};
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
对应 lane result。CLI 按 lane 打印 entry/stats/diagnostics，测试不再读取单个 `result.Index`。

事务规则：

- 每条 requested lane 全部成功后才发布自己的 `index.json`
- 一条 lane 失败不写半份新 index，也不删除另一条 lane 的有效产物
- overall result 在任一 requested lane 失败时仍失败，并按 lane 汇报 stats/diagnostics
- `Incremental` 只读当前 lane 的 blob/version
- `--clean` 仍删除整个 manifest artifact 目录；不新增 `--output`

---

## 9. resolver 与 runtime 单 lane 化

### 9.1 `ShaderResolveContext`

构造时新增一个 `ShaderBlobCategory target`，只接受 DXIL/SPIRV，并据此固定：

- lane index 路径
- lane toolchain hash
- AOT blob reader
- JIT compile function

`ShaderResolver::Resolve`、`LoadFromArtifact`、`CompileWithJit` 删除 category 参数。调用期间发现 index、
blob 或 typed output 的 lane 与 context 不一致是产物损坏/内部不变量错误，不回退到另一条 lane。

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

---

## 11. 测试替换清单

### 11.1 typed DXC

- DXIL 入口只返回 object + reflection，SPIR-V 入口只返回 words
- 不存在可由 `IsSpirv` 与 output category 互相矛盾的状态
- SPIR-V 编译产物不含 `SPV_GOOGLE_hlsl_functionality1` /
  `SPV_GOOGLE_user_type` `OpExtension`
- SPIR-V 编译产物不含 `HlslSemanticGOOGLE` / `UserTypeGOOGLE` decoration
- 非 4 字节 SPIR-V output 在 DXC 边界被拒绝

不要只 grep 编译参数；测试解析实际 SPIR-V instruction/string operand，证明扩展没有进入 module。

### 11.2 lane reflection/metadata

DXIL：

- semantic/index 大小写与 suffix 规则
- system value 过滤
- register/component type/stream/mask/read-write mask 原样保存
- 未使用参数仍按 DXIL signature 保留
- 数组/矩阵展开结果按 DXIL signature 原样保存

SPIR-V：

- 空 `Name` 仍能按 location/type 成功提取
- built-in 过滤、location 排序/空洞/重复拒绝
- scalar/vector/matrix/fixed-array shape round-trip
- 非数值、坏 `TypeIndex`、runtime/specialization-constant array 显式失败
- 无 `HlslSemantic` 字段与 `in.var.` fallback 测试

删除全部 DXIL↔SPIR-V 子集/semantic equality 测试；它们验证的是被废弃的统一投影。

### 11.3 validation 与 name stripping

- SPIR-V resource/push-constant name 为空时，set/binding/type/count/range 校验仍通过
- 相同条件下位置或类型错误仍失败
- DXIL name mismatch 继续由 DXIL policy 报错
- shader template 继续用 DXIL 名字生成 binding/vertex 初稿，SPIR-V probe 继续识别 push constant

### 11.4 blob/index

- DXIL 与 SPIR-V typed payload 各自 round-trip、损坏和 trailing-data 测试
- wrong target、wrong lane version、header/payload hash 篡改被拒绝
- SPIR-V words 与 array-dimension count 的边界检查先于分配
- `dxil/index.json` 与 `spirv/index.json` 独立加载
- lane index 的 absolute/rooted/`..` `BlobPath` 被拒绝，合法文件名仍限制在 lane directory 内
- 人为只递增 SPIR-V version/toolchain hash 时，DXIL index/blob 仍可命中
- 旧根 `index.json` 不被当成新产物读取
- incremental reuse 不跨 lane

### 11.5 resolver/runtime/RHI

- context 构造时固定 target，`Resolve`/program API 不再收 category
- `PipelineStateCache::GetOrCreateGraphics` 不再收 category，cache target 与 program target 不一致时失败
- D3D12/Vulkan 两套 fixture 分别得到正确 typed bytecode
- AOT/JIT 在同一 lane 返回相同 metadata
- variant 与 bytecode cache 去重不再含 category
- backend→lane 映射穷尽 D3D12/Vulkan，且仓库只有 runtime composition root 一处
- `MakeShaderDescriptor` 产生正确 alternative；两个 backend 都拒绝 wrong alternative
- DXIL/SPIR-V vertical slice 的 JIT/AOT 像素读回继续通过

### 11.6 layering

- `radrayshader` 不链接 `radrayrender`
- `shader_gen` / `shader_cook` 不链接 runtime/render
- 用 `ninja -C build_debug -t commands` 或 link map 核对，不用 `dumpbin /DEPENDENTS`

---

## 12. 文档与 ADR

实施代码时同步完成，不提前把 architecture 文档写成尚不存在的现状：

1. 新增 **ADR-0014 backend-specialized shader lanes**。
   它取代 ADR-0013、ADR-0006 与 ADR-0003，并完整重述仍有效的 manifest ABI authority、反射核对、
   shader type/module 边界；唯一放宽是 runtime composition root 可以做一次
   `RenderBackend -> ShaderBlobCategory` 映射，vertex artifact 则保存 lane-specific 最小 metadata。
2. 新增 **ADR-0015 shader artifact 按 lane 独立索引和失效**。
   它取代 ADR-0004，保留同目录、内容寻址、无 `--output`、逐 source identity 等裁决，只把根
   `index.json`、全局 version/toolchain 改成 lane-owned。
3. ADR-0013、ADR-0006、ADR-0004、ADR-0003 只改状态为“已被 ... 取代”；不追加或回写历史正文。
4. 更新 `docs/adr/README.md` 状态与 0014/0015 条目。
5. 重写 `docs/architecture/shader-pipeline.md` 的归属图、vertex metadata、AOT layout、runtime target
   selection 与测试表，删除“任何一层都没有 backend→category 映射”的现状描述。
6. 更新 `docs/guide/shader-authoring.md` 的 cook 输出布局与 `--category` 说明。
7. 保留 `docs/todo/vertex-interface-projection.md` 作为已实施历史，但明确其“下一阶段”已由本文取代。

---

## 13. 文件变更清单

| 文件 | 改动 |
|---|---|
| `modules/shader/include/radray/shader/dxc.h` | typed DXIL/SPIR-V output 和 compile 入口；删除 `IsSpirv` / output category |
| `modules/shader/src/dxc.cpp` | 分离参数构建与输出转换；删除 `-fspv-reflect` |
| `modules/shader/include/radray/shader/spirv.h` | 删除 HLSL decoration 字段；保留原生 stage IO/type shape |
| `modules/shader/src/spirv.cpp` | 同步反射 JSON codec |
| `modules/shader/include/radray/shader/spvc.h` | 所有 SPIR-V view/转换入口改收 typed word span |
| `modules/shader/src/spvc.cpp` | 删除 Google decoration/name correctness 依赖，完善数组维度反射 |
| `modules/shader/include/radray/shader/shader_manifest.h` | 两套 vertex metadata、typed payload、lane index/version/context API |
| `modules/shader/src/shader_manifest.cpp` | lane validator、key/hash、blob/index、cook、AOT/JIT dispatch |
| `modules/shader/src/shader_reflection_map.h/.cpp` | 公共 binding comparison 收窄，DXIL/SPIR-V policy 分开 |
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
| shader/runtime/render tests | 用第 11 节 lane-specific 测试替换公共 projection 测试 |
| ADR/architecture/guide | 按第 12 节同步 |

实施时用 grep 补齐调用点；本表不是删除未列文件的理由。尤其要检查 `shader_asset`、pipeline cache、
vertical slice 和测试 fixture 中手工传递 `ShaderBlobCategory` 的位置。

---

## 14. 实施顺序

1. 写真实 SPIR-V 探针测试：无 `-fspv-reflect` 时 location/type、资源绑定、push constant 的实际形状，
   并扫描 module 确认无两个 Google extension/decoration。
2. 引入 typed DXC 入口并迁移 shader_gen/cook/JIT；删除 `IsSpirv`、`DxcOutput::Category`、
   `-fspv-reflect` 与 Vulkan 两个扩展。
3. 把 `spvc.h` 及其全部调用方改成 typed word span；清理 `SpirvShaderDesc` 的 HLSL decoration 字段、
   reflection schema version 与 name correctness 依赖，先保证资源 ABI validation 在空 name 下成立。
4. 定义 `DxilVertexSignature` / `SpirvVertexInterface` 并替换公共提取、cross-category 测试和
   `ShaderVertexInterface`。
5. 分离 source identity version 与两条 lane 的 format/toolchain hash。
6. 实现新公共 envelope、typed payload reader/writer 与 lane-local index；完成旧产物拒绝测试。
7. 把 cook 改成共享遍历 + lane-local transaction，完成 incremental 独立性测试。
8. 给 `ShaderResolveContext` 与 `PipelineStateCache` 固定同一 target，依次移除
   resolver/program/asset/PSO cache 调用链的 category 参数。
9. 把 runtime `ShaderBytecode` 与 RHI `ShaderDescriptor` 改成 typed variant，迁移两个 backend。
10. 新增 ADR-0014/0015，更新旧 ADR 状态、architecture、authoring guide 与本 todo 状态。
11. 完整构建后，严格顺序运行 shader、template、layout、program/PSO、vertical slice 测试；构建与
    测试不得并发。

每一步删除旧路径，不保留“新 typed API + 旧 bool/category API”双轨兼容层。若需要让中间 commit
可构建，可在同一 commit 内迁移全部调用方，而不是长期保留 adapter。

---

## 15. 验证命令

```powershell
python tools/check_docs.py
cmake --build build_debug --parallel 24
ctest --preset win-x64 -R "ShaderAssetTest|ShaderArtifactTest|ShaderResolverTest" --output-on-failure
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

第一条在实施后的现状文档/代码中应无命中；历史 ADR/todo 可保留过去时记录。第二条应无命中。
第三条只允许 target 的存储/accessor、composition-root 映射和明确的 lane invariant，不允许逐次选择
参数；逐项人工核对。测试 fixture 与 shader cook 的显式 target 输入不在这条 production-runtime
allowlist 检查内。再用 `git diff --check` 检查补丁。

---

## 16. 完成标准

- DXIL 与 SPIR-V 编译 API、runtime payload、vertex metadata、artifact payload 和 RHI descriptor
  都由类型区分，不靠 `bool IsSpirv` 或公共 `Category` 字段解释
- Vulkan vertex metadata 完全不含 semantic/index/name，空 `OpName` 仍可 cook、validate、load
- 仓库生成的 SPIR-V 不含 `SPV_GOOGLE_hlsl_functionality1` / `SPV_GOOGLE_user_type`
- Vulkan device 创建不再要求两个 `VK_GOOGLE_*` 扩展
- 不存在 DXIL/SPIR-V vertex 接口相等、子集或互相转换规则
- runtime 对一个 device 只绑定一条 lane，program/resolver 热路径不再接 category
- DXIL/SPIR-V index、format version、toolchain hash 与 incremental reuse 相互独立
- 旧根 index/blob 被明确拒绝并通过全量重烘迁移，无兼容 reader
- shader CLI 链接边界不变，D3D12/Vulkan JIT/AOT vertical slice 都通过
- 当前 PSO 仍消费 manifest `VertexInput`；本轮没有偷跑 primitive layout 连接
- 没有新增 `try` / `catch` / `throw`

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
