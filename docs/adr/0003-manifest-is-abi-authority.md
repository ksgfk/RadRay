# ADR-0003 manifest 是 ABI 权威，反射只做核对

状态: 生效
日期: 2026-07
影响: `modules/shader/include/radray/shader/shader_manifest.h`、`shader_asset_template.h`；全部 `*.shader.json`

## 背景

问题是："既然 DXC 能反射出 shader 用了哪些资源，为什么还要人手维护一份 `*.shader.json`？"

答案是**反射不足以构建 PSO**。反射能告诉我们"有哪些资源、在哪个 register/space"，
但下面 7 项拿不到，它们必须由作者声明：

1. **push constant 身份**。DXIL 反射把 `[[vk::push_constant]]` 的 cbuffer 当普通 cbuffer，
   完全不知道它特殊；SPIRV 反射知道 size 但没有 D3D 的 register/space。
   **两个后端都缺**，不是"只有 vk 侧标记"。
2. **绑定驻留方式**。同一个 cbuffer 既可放进 descriptor table，也可做 root descriptor
   （D3D12 root CBV / Vulkan `UNIFORM_BUFFER_DYNAMIC`）。这是作者的性能决策，不是 shader 的属性。
3. **immutable / static sampler**。纯 pipeline-layout 期概念，不在字节码里。
4. **unbounded 数组的实际容量**。反射只说"unbounded"，两个后端都拒绝 `Count == 0`。
5. **被 DCE 或 keyword `#ifdef` 消掉的绑定**。反射看不到，但 layout 必须保留槽位，
   否则同一 shader 的不同变体无法共用一个 `PipelineLayout`。
6. **顶点属性的 VertexFormat / buffer slot / offset / stride**。反射只给 semantic 与
   component type + mask，推不出归一化与位宽（`UNORM8X4`、`FLOAT16X2` 都推不出）。
7. **entry point 名与 keyword 组合域**。这些本来就是编译输入。

## 决策

数据流方向单向：**manifest 声明 ABI，反射只做一致性核对。**

校验方向是 `声明 ⊇ 反射`：

- 反射出现但 manifest 未声明 → 失败（改了 HLSL 忘改 manifest）；
- manifest 声明但反射没有 → 通过（DCE / keyword `#ifdef` 的正常结果）。

三个直接后果：

- `BuildPipelineLayoutStorage` 不需要任何反射数据 / 字节码 / target 信息。可以在编译任何
  shader 之前建好 `PipelineLayout`，且结果对 target 与 variant 都不变。
- 反射数据**不落盘**。cook 期用它核对，之后丢弃。
- 手抄成本由 `radray_shader_gen` 承担：它把反射**能**确定的部分写成合法初稿，对上面 7 项
  填保守默认值（原则是「宁可保守到需要改，不可乐观到能跑但错」）并在 `_TODO` 里逐条点名。

`radray_shader_gen` 的默认值：

| 项 | 默认 | 为何必须人工确认 |
|---|---|---|
| `Residency` | 全部 `DescriptorTable` | root descriptor 是性能决策 |
| `VertexFormat` | 按 32 位分量数直译（`FLOAT32X3`…） | 归一化与位宽推不出 |
| `ArrayStride` / `Offset` | 按声明顺序紧密打包 | 真实布局由 mesh 决定，与 shader 无关 |
| unbounded 数组容量 | 1 | 后端拒绝 0，必须给真实上限 |
| `ImmutableSampler` | 一律不生成 | 不在字节码里 |
| `BakeVariants` | 不生成 | 发布决策，同一份 HLSL 在 PC 与移动端可以烘不同集合 |
| 被 `#ifdef` 消掉的绑定 | 探测多轮求并集，仍可能缺 | 反射的原理性局限 |

`BakeVariants` 留给作者的额外理由：改烘焙范围不该动 shader 源码，否则会让所有产物 cache 失效。

## 放弃的方案及代价

- **纯反射驱动，不要 manifest**。上面 7 项直接使它不可行。第 5 项是致命的：变体间 layout
  不一致会让 `PipelineLayout` 无法共享，PSO 缓存的 key 也随之失去意义。
- **manifest 只写反射给不出的部分，其余靠反射填**。等于要求运行时也做反射，而运行时
  （发布包）根本没有 DXC。且 PipelineLayout 就无法在编译前构建。
- **反射结果落盘，运行时读反射代替 manifest**。多一份必须与 manifest 保持同步的产物，
  而两者不一致时没有任何机制能发现。同时 manifest 已经是唯一 ABI 来源，反射落盘是纯冗余。
- **让 manifest 覆盖反射结果**（即"反射是基线，manifest 是补丁"）。方向错了：补丁语义下
  无法表达"这个绑定被 DCE 了但槽位要留着"，因为基线里根本没有它。

## 必须保持为真

- `BuildPipelineLayoutStorage(pass)` 的签名里没有反射数据、字节码、target 或 variant 参数。
- `ValidateShaderReflection` 的失败条件只有"反射有而 manifest 无"，反之通过。
- 反射数据不出现在 `index.json` 或任何 `.bin` 里。
- `radray_shader_gen` 生成的文件能被 `ParseShaderAssetDesc` 直接解析（`_TODO` 键被忽略）。
- 生成器与校验器共用 `modules/shader/src/shader_reflection_map.h` 的折叠规则——两处各写一份
  会让生成出的模板通不过自己的校验。
