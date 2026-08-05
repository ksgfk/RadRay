# RadRay

C++20 实时渲染器，D3D12 + Vulkan 双后端，纯光栅。

本文件是**领域词汇表**：定义术语「是什么」。不描述实现、不记录决策、不当规格书用。
实现现状见 `docs/architecture/`，决策见 `docs/adr/`。

## Shader 管线

**Pass**:
属于同一 shader 家族的一组 stage。一次绘制所需的完整 shader 程序；同一 Pass 的不同 Variant
可以拥有不同的 active binding layout。
_Avoid_: program, effect, technique

**Pass source unit**:
一份入口 `.hlsl` 文件定义且只定义一个 Pass；共享代码放在 `.hlsli`。stage entry functions
在 HLSL 中用 RadRay attributes 声明，由 compiler 自动发现。CompileVariantRequest 不再携带
作者维护的 stage/entry 列表。

**Stage**:
Pass 中的一个可编程管线阶段（vertex / pixel / compute）。
_Avoid_: 单独使用的 "shader"（过于笼统，可指 stage、pass 或字节码）

**Variant**:
同一个 Pass 的一组具体 keyword assignment，是 target-independent 的逻辑身份。每个 target
各自产生一个 Compiled Variant artifact 及其实际需要的 binding layout。
_Avoid_: permutation, configuration, 变种

**Compiled Variant artifact**:
一个 logical Variant 针对一个 target category 的物理编译产物。身份至少包含 keyword
assignment、DXIL/SPIR-V target 与 compiler/toolchain identity；DXIL 与 SPIR-V 可以拥有不同的
active binding 集合和 layout identity。

**HLSL source truth**:
HLSL 是唯一 shader authoring 源真相，forked DXC 是唯一编译与 metadata 权威。不保留 C++
trace、LuisaCompute 或其他 shader authoring/codegen 路线。作者维护的 shader metadata 必须
位于 HLSL；不再存在作者手写的 `.shader.json` 或其他并行 metadata 文件。

**Generated artifact index**:
compiler/cook 为查找 compiled Variant artifacts 生成的索引。它可以采用 JSON 或其他序列化
格式，但不能由作者编辑，也不能成为 shader contract 的第二份真相。

**Binding layout**:
一个 Compiled Variant artifact 需要哪些资源、各自落在哪个 target-native group 与 slot 上的
完整描述。由编译器生成并随产物交付，是该 target binding ABI 的唯一权威；不使用的资源不占
active layout 槽位。对于 graphics Variant，layout 是所有 active stage binding 的并集；每个
entry 记录自己的 stage visibility。stage-specific projection 只服务编译与缓存，不产生独立的
RHI ABI layout。
_Avoid_: pipeline layout, root signature, descriptor set layout（这三个是后端说法）；
Property（这是 codegen 内部的表示）；binding ABI（指同一事物的契约面，不指数据本身）

**Layout identity**:
描述 binding layout 形状的身份。它区分资源集合、类型、槽位与 stage 可见性；不同 Variant
只有在 layout identity 相同的时候才能共享 layout。
_Avoid_: shader hash（shader 产物身份，不等于 layout 身份）

**Declared contract**:
shader 作者声明的资源与接口约束。它说明允许或要求什么，不等于某个 Variant 编译后实际
使用了哪些资源。
_Avoid_: reflection（编译结果事实，不是作者声明）

**Stable binding identity**:
shader 源使用 target 已有的 HLSL 语义声明 binding：DXIL 以 `register(..., space...)` 为真相，
SPIR-V 以 `[[vk::binding(...)]]` / `[[vk::push_constant]]` 为真相。RadRay 不新增 binding
attribute，也不要求两套数字相等。同一资源在同一 target 的不同 Variant 中保持 binding
稳定，只会处于 active 或 inactive；compiler 负责验证并输出当前 artifact 的 active subset。

**Compiler-generated binding metadata**:
编译器根据最终产物生成的 active binding、stage visibility、类型与 layout identity。它与
shader bytecode 一起构成 Variant artifact；运行时直接信任它，不再通过 DXIL/SPIR-V 反射做
二次校验。

**Variant-level compiler metadata**:
graphics Variant 的各 stage 必须在同一个 compiler-level request 中完成 metadata 合并，直接
生成 Variant 级 metadata。合并发生在编译器内部，不交给 shader_cook 或 runtime；最终交付的
metadata 与各 stage bytecode 一起由这次 compiler request 产生。一个 request 代表一个确定的
Variant，不得把不同 keyword assignment 的结果混在同一个 metadata 中。

**Variant assignment ownership**:
合法组合域由 shader contract 定义；具体 assignment 由调用方传入。shader_cook 为 AOT
逐个传入要烘焙的 assignment，runtime 在允许 JIT 时为当前请求传入 assignment。编译器负责
校验并编译该 assignment，不负责枚举全部 permutation。

**AOT bake set**:
一次 cook 要预编译的 Variant assignments，是调用方根据项目、平台和内容使用情况提供的
build input，不属于 shader metadata。HLSL 只声明合法 keyword domain；不再存在作者维护的
`BakeVariants` 字段或文件。

**Artifact trust**:
运行时只接受 schema/version、工具链身份与完整性检查通过的 Variant artifact，并把其中的
compiler-generated binding metadata 视为事实；编译期/离线 cook 负责发现 metadata 与 bytecode
不一致的问题。

**Binding group**:
一组资源绑定的集合。同时是 D3D12 的 register space 与 Vulkan 的 descriptor set index ——
这是后端已硬化的不变量，任何一层都不做重映射。
_Avoid_: descriptor set, register space, space, table

**Residency**:
一个绑定是经 descriptor table 访问，还是作为 root descriptor 直接绑定。属于性能决策，
不是 shader 的属性。
_Avoid_: binding mode, access path

**Artifact**:
离线编译产出的、可在无编译器环境下加载的 shader 产物。内容寻址。
_Avoid_: blob（指承载 artifact 的单个文件）, cache（cache 可弃，artifact 是交付物）

### 离线编译（术语待定）

第一期不做离线编译，"cook" 与 "bake" 两个词一并搁置 —— 旧代码里它们的边界要靠注释解释
（`shader_manifest.h:150`），属于需要重新命名的遗留。等第一期跑通、真要做离线产物时再定名。
在此之前不要在新代码里引入这两个词。

## 资产

**Asset**:
由路径标识、引用计数管理生命周期的可加载资源。
_Avoid_: resource（resource 指 GPU 侧对象）

**AssetId**:
由归一化路径派生的资产标识。
_Avoid_: asset key, path hash
