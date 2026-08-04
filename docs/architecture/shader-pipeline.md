> - 适用: 改 shader 编译/烘焙/变体链路；排查"字节码取错了 / layout 建不出来 / 产物不命中"
> - 权威: 本文是 shader 三层架构与产物布局的唯一说明。写 HLSL 与改 manifest 的操作流程见 `guide/shader-authoring.md`；HLSL 库自身的分层见 `architecture/shaderlib.md`
> - 锚点: `modules/shader/include/radray/shader/shader_manifest.h`, `modules/runtime/include/radray/runtime/shader_program.h`, `modules/runtime/include/radray/runtime/shader_asset.h`, `modules/shader/src/shader_manifest.cpp`

# shader 链路

## 三层分工

```
shader_manifest.h   格式层   modules/shader     manifest desc、变体域、产物索引、
                                               ShaderResolver、cook。不含 Asset，不碰 GPU
shader_program.h    对象层   modules/runtime    ShaderPassProgram：共享 PipelineLayout 引用
                                               + 字节码缓存。不含 Asset
shader_asset.h      资产层   modules/runtime    ShaderAsset：一份 manifest 一个 Asset
```

分界照 `image_data.h`（数据格式）与 `image_asset.h`（Asset）的既有先例。

格式层单独成一个模块（`radrayshader`），是为了让 `tools/shader_cook` 和 `tools/shader_gen`
只链它 + `radraycore`，不吃进图形后端。对象层停在 Asset 之下，是为了让材质层能只拿
"一个 pass 的 layout + 字节码"而不被迫拖进 `AssetManager`。理由见
[ADR-0002](../adr/0002-shader-three-layer-split.md)。

## 归属关系

```
ShaderAsset                      一份 manifest（shader_asset.h）
  ├─ ShaderAssetDesc             manifest 解析结果
  ├─ ShaderResolver              一资产一份，与 "index.json 每 manifest 一个" 对齐
  └─ ShaderPassProgram[]         每 pass 一个（shader_program.h）
       ├─ ShaderPassDesc             Source 已展开的副本
       ├─ SharedPipelineLayout       共享，非独占。variant/target 无关，加载期取得
       ├─ ShaderVertexInputStorage   仅 graphics pass
       ├─ ShaderVariantDomain
       └─ 字节码缓存（两级）
            └─ BytecodeEntry[]
                 └─ ShaderBytecode
                      └─ optional ShaderVertexInterface（仅 vertex stage 有值）
```

`ShaderAsset` 内 `_resolver` **必须声明在 `_passes` 之前**：program 借用 resolver 裸指针，
析构逆序保证 program 先死。

## manifest 是 ABI 权威

`*.shader.json` 存在的理由是：**HLSL 源码 + 后端反射不足以构建 PSO**。反射能说"有哪些资源、
在哪个 register/space"，但拿不到 7 项必须由作者声明的信息。完整清单与推导见
[ADR-0003](../adr/0003-manifest-is-abi-authority.md)。

资源绑定的数据流方向由此确定：**manifest 声明 ABI，资源绑定反射只做一致性核对**。
三个直接后果：

- `BuildPipelineLayoutStorage` 不需要任何反射数据 / 字节码 / target 信息。可以在编译任何
  shader 之前建好 `PipelineLayout`，且结果对 target 与 variant 都不变。
- 用于资源绑定 ABI 核对的完整反射不落盘。cook 期用它核对"声明 ⊇ 反射"，之后丢弃；
  唯一收窄是 vertex-stage 的精确 artifact 在自身 `.bin` 中保留最小输入接口投影，
  `index.json` 仍不保存反射数据。
- 校验方向是单向的：反射出现但 manifest 未声明 → 失败（改了 HLSL 忘改 manifest）；
  manifest 声明但反射没有 → 通过（DCE / keyword `#ifdef` 的正常结果）。

与 `hlsl.h` / `spirv.h` 里的反射 JSON 是两种不同性质的文件：那份机器生成、枚举存整数；
manifest 人写人 diff、枚举存字符串。刻意不共用约定。

### vertex-stage 最小输入接口投影

每份 vertex-stage DXIL/SPIR-V artifact 都携带自己的 `ShaderVertexInterface`；pixel/compute stage
保持 `nullopt`。vertex stage 即使没有用户输入也保持 engaged，`Parameters` 可以为空。
投影只保留连接 primitive 所需的 semantic、semantic index、按 category 分派含义的 backend
location、scalar type、bit width 与 component count，不保存资源绑定完整反射。

接口是**精确 stage artifact 的属性**，由 category、`IsOptimize`、vertex-stage variant 投影、
source/entry point、shader model、pass defines 与 toolchain 等完整编译身份共同决定，不是 pass
级固定数据。AOT 从 blob 恢复，JIT 在编译后提取，二者都落进 `ShaderBytecode`；
`ShaderProgramVariant` 只读暴露它。跨 category 的提取与比较约束见
[ADR-0013](../adr/0013-vertex-stage-interface-projection.md)。

当前渲染路径没有消费这份投影：PSO 仍走 manifest `VertexInput` →
`ShaderVertexInputStorage` → graphics pipeline。把 `ShaderVertexInterface` 与 primitive vertex layout
连接起来属于后续阶段。

### 不在 manifest 里的东西

- **PSO 固定功能段**（`PrimitiveState` / `DepthStencilState` / `BlendState` /
  `ColorTargets` / `MultiSampleState`）。它们不影响字节码，由建 PSO 的调用方给出。
  注意这意味着 manifest 里没有"pass 基线"可供材质覆盖——基线由谁提供**尚未裁决**，
  见 `render_framework/render_pipeline.h` 的 `MaterialRenderState`。
- **AssetId**。身份不是 manifest 内容的一部分，由 manifest 路径推导（`MakeShaderAssetId`）。
  故同一份内容放在两个路径下是两个资产。

### 关键 desc 字段的约束

| 字段 | 约束 |
|---|---|
| `ShaderBindingDesc::Binding` | 等于 HLSL register 号。b/t/s/u 在本 ABI 中**共用同一编号空间**（Vulkan 的要求） |
| `ShaderBindingGroupDesc::Group` | 同时是 D3D12 的 `RegisterSpace` 与 Vulkan 的 set index。后端已硬化，manifest 不做重映射 |
| `ShaderBindingDesc::Residency` | `RootDescriptor` 仅 CBuffer/Buffer/RWBuffer 合法，且 `Count` 必须为 1 |
| `ShaderBindingDesc::ImmutableSampler` | 仅 Sampler 类型且 `Count == 1` |
| `ShaderPushConstantDesc` | 整个 layout 至多一个，由类型系统（`optional`）保证而非计数校验。`Size` 非零且 4 字节对齐 |
| `ShaderPushConstantDesc::Location` | D3D12 用它填 `32BIT_CONSTANTS` 的 RegisterSpace/ShaderRegister；Vulkan 只用它做 `SetPushConstants` 的匹配键。一个字段服务两个后端 |
| `ShaderPassDesc::IsOptimize` / `EnableUnbounded` | 直接改变字节码（`-O3`/`-Od`、`-all_resources_bound`），因此是 artifact 身份的一部分，必须进 manifest |
| `ShaderPassDesc::Source` | 留空则继承 `ShaderAssetDesc::Source`。用 `GetEffectiveSource` / `MakeResolvablePass` 展开，不要各处自己写 `?:` |

`ShaderAssetDesc` 是解析结果，保持与 manifest 逐字对应（`pass.Source` 空就是空）以使往返
序列化不失真；展开是消费侧的需要，故 `MakeResolvablePass` 返回副本而非就地改。

## 变体系统

### 三个概念不要混

| 概念 | 是什么 | 谁定义 |
|---|---|---|
| **合法组合域** | 哪些 keyword 组合可以被请求 | `KeywordGroups` → `ShaderVariantDomain` |
| **烘焙范围** | 哪些组合值得离线预编译 | `BakeVariants` → `ExpandShaderBakeSet` |
| **变体身份** | 一个具体组合 | `ShaderVariantKey` |

域大而烘焙集小是**正常状态**：未烘焙的组合在开发构建走 JIT，在发布包
（`ShaderResolveSettings::AllowJit == false`）成为显式错误。

### ShaderVariantKey 的编码

长度 == domain 的组数，槽位序 == 组的声明顺序，值 == 组内 keyword 下标或 `kShaderKeywordOff`
（`0xFFFF`）。按**组**而非按 keyword 编码，因为组内互斥已由 manifest 校验保证，于是
"每组选了什么"就是完整身份，且 keyword 总数不受任何上限约束。

两条使用约束：

- **仅在同一个 domain 内可比**。槽位语义由 domain 的组声明顺序定义，跨 (asset, pass) 比较无意义。
- **不要放进每帧路径**。变长结构，属作者期/cook 期概念。运行时 PSO 缓存用
  `ComputeShaderArtifactKey` 得到的 `ShaderHash`——那是 POD，无分配。

### stage 投影是字节码去重的机制

`ShaderKeywordGroupDesc::Stages` 声明某组影响哪些 stage。`ProjectToStage` 把与该 stage
无关的组槽位归一化为 `kShaderKeywordOff`，**无论 `IsOptional`**。

这条归一化规则给出核心不变量：

> 两个变体投影结果相同 ⟺ 该 stage 共用同一份字节码。

若必选组保留其默认选择而不归 Off，两个本应共用同一份 VS 的变体会算出不同的 artifact key，
去重失效。实测 forward_pass 的 `Deduplicated == 1`。

### 烘焙声明

`ShaderBakeRuleDesc` 的 `Expand` 与 `Combination` 恰好一个非空：

- `Expand`：一组正交轴的全组合，对应 Unity 的 `multi_compile`。
- `Combination`：单个精确组合，表达 `Expand` 表达不出的稀疏点。

`Skip` 是剔除规则（对应 Unity 的 `skip_variants`），**只作用于 `Expand` 的积**——显式
`Combination` 是作者点名要的，不该被泛化规则悄悄拿掉。

展开结果**总是**含默认变体：一份 shader 至少要能在不开任何 keyword 时工作，且这使
"空 BakeVariants"与"只烘默认"是同一件事。不设数量上限。

**继承是不对称的**，这是刻意的：pass 显式写的规则引用了本 pass 没有的组会报错（显式声明
必须被严格核对），而从资产级继承下来的规则遇到本 pass 没有的组会静默投影掉（共享默认值
必须能被裁剪，否则每个 pass 都得复写一遍）。`ExpandShaderBakeSet` 的 `isInherited` 参数
就是这个开关。`Combination` 的未知 keyword 与组内多选**始终**是错误，与继承无关。

## AOT 产物布局

manifest `foo.shader.json` 的产物放在同目录的同名文件夹 `foo/`：

```
forward_pass.shader.json
forward_pass/
    index.json                  变体表: key -> blob 相对路径 + cook 元信息
    dxil/a3f29c1b40e8d715....bin
    spirv/8c01ae52f7d3b064....bin
```

blob 采用**内容寻址**：文件名即 stage artifact key 的 hex。由此当多个 program variant
投影到同一份 stage 字节码时自然去重。按 target 分子目录，使"只发布 DXIL"退化为删除一个目录。

blob v1 是一个带嵌套 payload 的自验容器：

```
header:
    magic + formatVersion
    key.Low + key.High
    stage + category
    ContentHash.Low + ContentHash.High
    payload                  SizedBytes

payload:
    if stage == Vertex:
        parameterCount       u32
        parameters[]         semantic + semanticIndex + backendLocation
                             + scalarType + bitWidth + componentCount
    bytecode                 SizedBytes
```

接口存在性直接由 `stage` 决定，不另存 `hasVertexInterface` bool：vertex stage 总是有
`parameterCount`（允许为 0），其他 stage 没有接口段。接口只在对应 blob 中，`index.json`
不重复保存。

`ContentHash` 覆盖 header 中的 `key` / `stage` / `category` 以及完整 payload 序列化字节，
所以接口 metadata、嵌套 bytecode 和关键 header 被篡改都能由单个 blob **独立自验**。
`ShaderArtifactEntry::BytecodeHash` 仍只覆盖原始 bytecode；两者不合并，也不改变 blob 文件名
由 artifact key 内容寻址的规则。

产物与 manifest 同处一地的完整理由（以及为什么 `radray_shader_cook` 刻意不提供 `--output`）
见 [ADR-0004](../adr/0004-content-addressed-shader-artifacts.md)。

### index.json 按源文件分别记录身份

`ShaderArtifactIndex::Sources` 是一个列表，每个源文件一条。因为 artifact key 是按
**该 pass 自己源文件**的身份算的，而一个资产内不同 pass 可以有不同 `Source`。若只存一份
合并哈希，多源资产在 `Strict` 下永远判为过期、在 `Lenient` 下永远算错 key。

`ShaderArtifactEntry::Keywords` **不参与 Key，也不用于查找**：运行时按
(pass, stage, category, 投影后的 defines, SourceIdentity, ToolchainHash) 纯函数算出 Key
再查表，这里只是供人工核对与工具使用的可读身份。

## 源码身份

`ShaderSourceIdentity` = 源文件及其 `#include` 闭包的内容哈希。

自己扫 `#include` 的理由：DXC 只暴露默认 include handler，编译后拿不到依赖列表，
因此无法事后得知该重编什么。扫描**不求解 `#if`**，对被条件排除的 include 也一并计入——
这是安全的方向（过度失效优于漏失效）。

这也是 AGENTS.md 里"include 必须根相对 + 尖括号"那条硬规则的来源：扫描器按 include 根
解析路径，DXC 接受的文件相对形式它认不出。

哈希用 `HashShaderBytes` 而**不是** `radray::HashCode`——后者按 `size_t` 宽度分派，
32/64 位结果不同，而 cook 机器与运行机器可能不同。

### 缓存在文件层，不在 entry 层

`ShaderResolveContext` 按**文件**记忆化源码读取与 include 扫描，跨 manifest 共享。

理由是闭包高度重叠：error_pass 的 8 个文件是 forward_pass 那 15 个的子集，shadow_pass
的 6 个也几乎全部重叠。按 entry 缓存（每份 manifest 一份）会把 `math.hlsli` 读 N 遍、
`filtering.hlsli`（9.7 KB）读 N 遍，并各自重扫一遍 `#include`。

缓存条目同时存内容与时间戳，命中时先 stat 复核。光存内容不够——`Strict` 承诺"改 shader
立刻生效"，而 context 会跨整个进程存活。

哈希公式与无缓存版本的 `ComputeShaderSourceIdentity` **逐字一致**（排序后的
path + size + bytes），故已有 AOT 产物不失效。改动缓存实现时不要碰累加顺序。

## 解析：AOT 优先，JIT 兜底

```
ShaderResolveContext   全进程一份。策略 + 工具链哈希 + 文件级源码缓存
ShaderResolver         一份 manifest 一个。index 缓存 + artifact 目录
ShaderPassProgram      字节码缓存（两级）
```

`ShaderResolveSettings` 的三项（`ShaderRoot` / `Staleness` / `AllowJit`）加上 `Dxc` 指针，
回答的是同一个问题："这是开发构建还是发布包"。所以它们是**进程级**答案，放在
`ShaderResolveContext` 里，不按资产给。曾经它们在 `ShaderAssetLoadOptions` 里各占一项，
那让每个加载调用点都能自行决定，形成第二套真相——而且从未被兑现（`AssetId` 只哈希
manifest 路径，dedup 命中时第二次的 options 连协程都没启动就被丢弃）。

`ShaderArtifactStaleness` 两档：

| 档 | 用途 | 行为 |
|---|---|---|
| `Strict` | 开发 | 源码哈希与产物不符即视为未命中，回退 JIT。改 shader 立刻生效 |
| `Lenient` | 发布 | 只按逻辑 key 命中。源码可读且哈希不符时仅告警，源码缺失时静默接受 |

`Lenient` 的存在理由：发布包内没有 DXC 可回退，源码若被改动一个字节也不该让整包 shader 失效。

### 字节码缓存必须在 program 层

`ShaderResolver::Resolve` **不缓存字节码**，每次调用都重新读 blob 或重新 JIT（它缓存的
只有 index 与源码身份）。若 program 层不缓存，每次 PSO cache miss 都要重新读盘/重编。

缓存是两级的：

1. `ShaderHash -> ShaderBytecode`，拥有字节码，按 artifact key 去重；
2. `(variant, category) -> 各 stage 的 ShaderHash`。

两级是必要的，因为 stage 投影保证多个变体共用同一份字节码。只按变体缓存会存多份副本。

**失败不写缓存**：中途任一 stage 失败即整体返回 nullptr，已解析的 stage 字节码仍留在
一级缓存（它们本身有效且已按 key 去重），但不产生半个变体条目——否则下次命中会拿到一个
缺 stage 的变体。

## 刻意不缓存的两样东西

**`render::Shader` 是瞬态参数，不是资源。** 两个后端都只在建 PSO 时消费它，PSO 建成后
无任何回指：`GraphicsPsoD3D12` 成员只有 device/layout/pso/vertexStrides/topo（字节码在
`CreateGraphicsPipelineState` 内被拷进 PSO），`GraphicsPipelineVulkan` 成员只有
device/layout/pipeline（Vulkan 规范明确允许 pipeline 建成后立即销毁 shader module）。
`ShaderEntry::EntryPoint` 那个 `string_view` 同理只需活到调用返回。所以 Shader 应在建 PSO
的函数里当局部量创建，出作用域即销毁。常驻缓存它只能省下一次 `CreateShader`（输入就是本层
缓存的字节码，不读盘不 JIT），代价却是永久驻留全部 `VkShaderModule` / 字节码副本。

**PSO 必须常驻缓存，但归 RenderSystem。** PSO 的 key 比字节码宽（含 `MaterialRenderState`、
vertex layout、RT 格式），同一份字节码会喂给多个 PSO。

## PipelineLayout 是共享的

layout 只由 binding 布局决定，与 variant / target 无关，且规模化后大量 pass 的布局逐字节
相同。故它归 `PipelineLayoutCache` 按内容去重，program 只持
`IntrusivePtr<SharedPipelineLayout>` 一份引用，归零时对象自毁。细节见
`architecture/asset-system.md`。

由此 `ShaderPipelineLayoutStorage` 降级为**瞬态**：只在加载期把 manifest 打包成一份
descriptor 喂给缓存，缓存把内容归一化进自己的 key，之后 storage 即可丢弃。program 与缓存
都不再存它——key 只用于查表，不需要能还原成 descriptor。

## 资产层

### 粒度 = manifest，不是 (manifest, pass)

产物布局已经定死了这条边界：index.json 每份 manifest 一个，artifact 目录由 manifest 路径
推导，`ShaderArtifactIndex::Entries` 混装该 manifest 下所有 pass 的 stage（`PassName` 只是
条目里的一个字段）。若按 pass 切，两个资产会共享一份 index.json 与一份 resolver 缓存，
"谁持有 resolver"立刻无解。PipelineLayout 的 per-pass 性质落在资产内部分层
（`ShaderPassProgram`），不上升为资产边界。

### "构造即完整"的兑现方式

`Asset` 要求放进 `AssetManager` 时已可用，而字节码本质按 variant 惰性。解法是加载期只做
variant / target **无关**的部分——desc 解析、PipelineLayout、vertex input storage。
"完整"指 ABI 与 layout 就绪，不指所有变体已编译。

这也让**加载路径不碰 DXC**：加载协程里只有同步文件 IO 与同步 GPU 调用，两者都短，
所以 AssetManager 的单线程泵不会被 JIT 阻塞。JIT 发生在后续 `GetOrCreateVariant`，
那是调用方主动触发的。

### ShaderAssetLoadOptions 只放共享设施指针

两个字段（`Context`、`LayoutCache`）都是**指向唯一一份共享设施的指针**，不是决策。
所有调用点必须传同一个，传错不会产生"两套策略"，只会产生一个错误的依赖注入。

这条约定由 `LoadShaderAsset` **机械兑现**，不只是注释：资产记下它是用哪一份建的
（`ShaderAsset::GetResolveContext`），dedup 命中且不一致时 abort。那是依赖注入接错了线，
不是可恢复的运行时状况——调用方拿到的资产其 layout 来自别人的缓存，后续建 PSO 的行为
无从预测。

`options` 在**发起加载之前**被校验。`AssetManager::Load` 命中既有 slot 时直接返回 handle，
`request.Task` 那个协程帧一次都不 resume。故校验若只写在 `CreateShaderAsset` 里，第二次
调用带的空 options 会被静默接受，调用方以为自己的 context 生效了，实际用的是第一次那份。

`CreateShaderAsset` 刻意不收 `render::Device`（`LayoutCache` 已绑定一个非空 device，
传 device 只会多一条"两者是否一致"的校验）也不收 `AssetManager&`（那是 `ShaderContent`
时代的遗留许可证）。

## 模板生成器

`shader_asset_template.h` 从 HLSL 反射生成 `*.shader.json` 的**起始模板**，不是可直接发布
的 manifest。它把 manifest 初稿成本从"手抄全部"降到"审阅并补齐 TODO"。

反射能给的：每个绑定的名字、(space, register)、类型、数组容量，以及顶点输入的 semantic
列表。生成器与校验器共用同一套反射折叠规则（`src/shader_reflection_map.h`），所以生成出的
模板天然能通过 `ValidateShaderReflection`。

反射给不出、必须人工确认的字段，生成器填保守默认值并在 `Todos` 里逐条点名，同时在序列化
输出里写 `_TODO` 键（JSON 无注释，只能借键传达；该键不属于 manifest schema，
`ParseShaderAssetDesc` 直接忽略，故生成的文件**可以直接解析与 cook**）。默认值的选取原则是
「宁可保守到需要改，不可乐观到能跑但错」。清单见
[ADR-0003](../adr/0003-manifest-is-abi-authority.md)。

### 根本局限：生成结果是"某一个变体"的下界

反射只能看到**实际编译出来的那份字节码**里活着的绑定。不给 keyword 种子时生成的是默认变体
的绑定集合；被 `#ifdef` 关掉的分支所用的绑定不会出现。而 PipelineLayout 必须对所有变体一致。

缓解手段是探测多轮：`ShaderTemplateOptions::ProbeDeclaredKeywords`（默认开）按声明的
keyword 组**逐组各开一轮**，n 组时轮次是 O(n) 不是 O(2^n)——绑定与 keyword 通常一一对应
（一张贴图一个宏），逐组开启即可覆盖。需要"两个宏同时开启才出现的绑定"时才手工给
`ProbeDefineSets`。

`UseSpirvReflection` 默认开，因为 **push constant 只有 SPIR-V 反射能识别**：DXIL 把
`[[vk::push_constant]]` 的 cbuffer 当普通 cbuffer。关掉这项，生成的模板会把 push constant
误写成一条普通 CBuffer 绑定。

### keyword 组以 HLSL 里的 #pragma 为唯一权威

```hlsl
#pragma radray_keyword_group(BaseColorMap, _BASECOLOR_MAP) stages(Pixel)
#pragma radray_keyword_group(AlphaMode, _ALPHATEST_ON, _ALPHABLEND_ON) stages(Pixel)
#pragma radray_keyword_group(Lighting, _LIT, _UNLIT) stages(Vertex, Pixel) required
```

第一个标识符是组名，其后是组内**互斥**的 keyword（至少一个）。`stages(...)` 省略则取
`Graphics`；`required` 关掉 `IsOptional`。DXC 忽略未知 pragma，这些行不影响编译（`-WX` 亦不告警）。

声明放回 HLSL 而不是 manifest 的理由，以及"默认采纳整条 include 链"的取舍，见
[ADR-0005](../adr/0005-keyword-groups-declared-in-hlsl.md)。

解析基于 DXC 的预处理输出（`dxc -P`），不是自己写的词法扫描。于是块注释、续行符、`#if 0`
全部由编译器正确处理。`StripShaderKeywordPragmas` 把 pragma 行**替换为空行而非删除**，
以保持行号不变。

`ParseShaderKeywordPragmas` 只做行内语法校验（组名非空、至少一个 keyword、stage 名合法、
修饰符已知）。组名重复、keyword 跨组撞名这类**跨组**约束交由 manifest 校验统一报错，
以免同一规则两处实现而口径分叉。

## 没有"后端 → 字节码类型"的映射

任何一层都没有。本层所有接口都直接收 `ShaderBlobCategory`，由调用方决定用哪种字节码。
曾有一个 `GetShaderBlobCategoryForBackend`，而它是 `RenderBackend` 看起来属于 shader 层的
唯一理由——一个便利函数把一个类型拽低了一层。删掉它，`RenderBackend` 就干净地留在 `rhi.h`。

判断一个类型该放哪层时的可用问法：**"如果我删掉这一个便利函数，这一层还需要它吗？"**
完整推导见 [ADR-0006](../adr/0006-shader-types-layer-boundary.md)。

## CLI 工具

| 工具 | 时机 | 方向 |
|---|---|---|
| `radray_shader_gen` | 作者期，一次性 | 反射 → manifest 模板（输出需人工收敛） |
| `radray_shader_cook` | 构建期 | 消费已收敛的 manifest → AOT 产物 |

两者都要 DXC 但方向相反，故不合并。两者都**只链 `radrayshader`**，不链
`radrayruntime`/`radrayrender`。用法见 `guide/shader-authoring.md`。

`radray_shader_gen` 默认**拒绝覆盖**已存在的输出文件（需 `--force`）：目标通常是一份已经
人工补齐过 Residency / BakeVariants 的 manifest，直接覆盖会静默丢掉那些手工决策。

`radray_shader_cook --clean` 先删整个产物目录再烘。增量只跳过已存在且自验通过的 blob，
从不删除任何东西；删掉一个 bake 规则后上一轮的 blob 会留在目录里（不影响正确性，
但会一直占着发布包）。

## 测试

| 套件 | 覆盖 | 需要 GPU |
|---|---|---|
| `ShaderAssetTest` 等 6 个（`test_shader_asset`） | manifest 解析/序列化/校验、变体域、烘焙集、产物索引、resolver | 否 |
| `ShaderAssetTemplateTest`, `ShaderKeywordPragmaTest` | pragma 解析、模板生成（对真实 shaderlib） | 否 |
| `ShaderLayoutBindingTest` | manifest → PipelineLayout 的绑定映射 | 否 |
| `ShaderAssetIdTest`, `ShaderAssetLoadTest` | 加载路径不碰 DXC、字节码缓存在 program 层、layout 经 OnUnload 释放 | 是 |
| `VerticalSliceTest` | manifest → PSO 全链路，JIT/AOT 双参数化 | 是 |

`test_shader_asset*` 只链 `radrayshader`，这本身就是层边界的回归测试。
