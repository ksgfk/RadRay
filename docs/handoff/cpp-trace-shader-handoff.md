# Handoff: RadRay C++ trace shader 前端

**日期**: 2026-08-04
**上一轮性质**: `/grilling` + `/domain-modeling` 会话。**8 项决策已闭合并落成文档**，第 9 项被中断。
**未动任何代码。** 全部产出都是文档。

---

## 1. 这轮在做什么

用户判断 RadRay 的 shader 系统「过于复杂、难扩展、难跨平台、难用」，根因是 HLSL+DXC
无法良好反射元数据。要求研究把 LuisaCompute 的 C++ DSL 能力接进来，并被无情采访直至达成共识。

结论：**shader 源真相改为 C++ trace，绑定在 trace 期构造，manifest 整体删除。**
Slang 被用户明确否决（"它很烂"）。DXC 保留但降为 codegen 背后不可见的后端。

---

## 2. 已落成的文档（先读这些，不要重复推导）

按此顺序读：

| 路径 | 状态 | 内容 |
|---|---|---|
| `docs/todo/cpp-trace-shader-frontend.md` | 新增 | **主实施清单**。第一期范围、引入方式、薄 DSL 层、光栅硬约束、删除清单、6 项风险、测试与文档影响 |
| `docs/adr/0014-cpp-trace-is-shader-source-of-truth.md` | 新增（提议） | 取代 ADR-0003。含 ADR-0003 那 7 项理由逐条失效的对照表、放弃 Slang/整体引入 LC 的理由 |
| `docs/adr/0015-variants-are-cpp-parameters.md` | 新增（提议） | 取代 ADR-0005。变体是 C++ 函数参数；离线编译推迟 |
| `docs/research/ue5-material-resource-layout.md` | 新增 | **UE5 源码研究**。Static Switch 的 active resource 集合、Material UB、D3D12 root signature、Vulkan descriptor layout 与 bindless 差异 |
| `CONTEXT.md` | 新增 | 领域词汇表。含 Trace / Binding layout / Binding group / Residency / Artifact 等术语与 `_Avoid_` |
| `docs/adr/0003`, `0005` | 已改状态 | 标记「已被 ADR-0014/0015 取代」（按 ADR-README 规则，改状态是唯一允许的修订） |
| `docs/adr/README.md` | 已改 | 索引加 0014/0015；0013 标「待随 ADR-0014 重审」 |
| `docs/todo/backend-specialized-shader-lanes.md` | 已作废 | 1,311 行，前两轮刚修订完又整份作废（前提是反射，而反射没了）。**第 8.4 节的 cook 事务/ValidatedHash/fail-closed/原子发布裁决留待第二期采纳** |

`docs/todo/cpp-trace-shader-frontend.md` 已经包含全部技术细节（11 个 attribute 键表、
LC 需改的两处、删除规模、风险清单）。**不要在新会话里重新调 subagent 研究这些。**

**ADR-0015 的高层方向仍成立，但接口结论已重新打开。** “变体是普通 C++ 参数”只回答了
trace-time 分支，没有回答请求身份、active binding、stage 投影和结构变体；在这些问题闭合前，
不得把 ADR-0015 当成可直接实施的完整设计。

---

## 3. 已闭合的 8 项决策

1. 四个症状全都要治；**不用 Slang**
2. 「作者手写 HLSL + manifest + 反射核对」这套**握手**是真问题；**DXC 不解决**，降为不可见后端
3. 引 LC 的 **`ast` + `hlsl-codegen`** 两个库（需改 LC 两处，见 todo 文档第 2 节）
4. **Tier A**：不引 `dsl/`，自写薄 DSL 层。理由：顶点属性只有 `Type::structure(align, members, attributes)` 能表达，`LUISA_STRUCT` 无 attribute 通道
5. **删掉 manifest**，绑定全由 trace 产出 → ADR-0003 被取代
6. 变体全面 C++ 化 → ADR-0005 被取代。**只闭合了方向，具体接口已在续会话重新打开，见第 5 节**
7. shader 层全部重写；**`modules/render` 的 RHI 契约不动**（16,829 行，本来就不消费反射）
8. 薄 DSL 层**从 LC 的 `dsl/` 剪裁拷贝**（Apache-2.0），只剔除 runtime 耦合

会话末追加两项范围决定：
- **"cook" 与 "bake" 两个词一并搁置**，不在第一期。旧代码里边界要靠注释解释（`shader_manifest.h:150`），属遗留命名。新代码不得引入这两个词
- **第一期终点 = 只保留到 PSO 的那条链**：trace → codegen → DXC → `ShaderDescriptor` → `PipelineStateCache` → 画出像素。进程内缓存按生成 HLSL 哈希，**不落盘**。因此第一期无发布路径（与 `render_system.cpp:67-70`「无 DXC 即发布包」冲突，冲突已在 ADR-0014 里显式接受）

---

## 4. 先前中断点：codegen 失败

**问题**：codegen 失败的处理方式。

已核实的事实（`luisa/core/logging.h`）：

```cpp
template<typename... Args>
[[noreturn]] LUISA_FORCE_INLINE void log_error(Args &&...args) noexcept {
    ...
    detail::default_logger().error(error_message);
    std::abort();
}
```

两个推论，都已修正我先前的错误说法：

- 这是 **`std::abort()`，不是 `throw`**。所以接受它**不违反** AGENTS.md 的禁异常规则 —— 根本没有异常，无需 `try`/`catch`
- 但 `std::abort()` **进程内无法拦截**。「在边界隔离」不是可用选项，第一期又是运行期 trace，没有离线工具可供隔离

codegen 用它很随意：无效 attribute 键、缺 attribute、成员非标量/向量、position 非 float4、
`PrintValue<float>` 遇 NaN。

**给用户的 4 个选项**（用户 dismiss 了，未作答，需重新提出）：

1. **（推荐）在自己的薄 DSL 层做全量预校验**：调 `RasterCodegen` 前把 codegen 会报错的条件全查一遍（每个顶点成员有 attribute、键在 11 个内、成员是标量或向量、`vertex_id` 是 `uint`、v2p 第 0 个成员是带 `position` 的 `float4`、`attributes` 全长、pixel 参数 0 == vertex 返回类型），全部返回 `error_code`，零 `try`/`catch`/`throw`。依据：abort 拦不住，预校验是唯一真存在的防线；且 pixel/vertex 类型匹配这条 codegen 本来就不查，我们本来就得自己断言。代价：校验规则与 LC 版本耦合
2. 打补丁把 LC 的 abort 改成 throw —— **需用户单独批准**，直接撞 AGENTS.md「不得用 catch 把错误转成 nullopt/false」
3. 接受 abort，只用 `LUISA_CUSTOM_LOGGER` 把日志接到 RadRay logger（见下）
4. 只预校验 codegen 不查的那一条（pixel 参数 0 == vertex 返回类型）

**附带发现**（可写进 todo 文档风险 #6 的改进）：`LUISA_CUSTOM_LOGGER` 宏可把 spdlog 换成
纯回调 `set_custom_logger(CustomLoggerCallback&&)`，无需碰 spdlog sink 类型即可把 LC 日志
接进 RadRay 的 logger。但该宏下 `log_error` 仍以 `std::abort()` 结尾。

---

## 5. 续会话新增：C++ DSL 如何表达 Variant

用户指出这是方案的 **go/no-go 问题**：如果 C++ DSL 不能表达真实 shader 变体，就不能采用
整个 C++ trace 方案。经 RadRay、LuisaCompute 与 UE5 源码调查，现有 ADR-0015 过于乐观。

### 5.1 已核实的表达机制

LuisaCompute 的 `RasterStageKernel` 构造会经 `FunctionBuilder::_define` **立即执行 lambda**。
因此有两种本质不同的分支：

- 捕获的宿主 `bool` / enum 配普通 C++ `if`：在 trace 时执行，只有选中路径进入 AST，形成变体
- `Expr<bool>` 配 DSL `if_` / `$if`：进入 AST，成为 GPU 运行期分支，不形成编译变体

但“普通参数”不能消灭变体身份：

- 每个 Pass 的 typed `Variant` 值是**请求身份**，回答“材质/管线要哪个变体”
- 生成 HLSL 的内容哈希是**编译产物身份**，回答“这份字节码是否已经存在”
- 后者必须先 trace/codegen 才能得到，不能取代前者。能删除的是旧的字符串/槽位式
  `ShaderVariantKey`，不是请求 key 这个概念

### 5.2 三档复杂度

1. **只改变函数体：简单。** 例如 alpha test、选择不同算法，宿主 `if` 即可
2. **改变 active binding：中等。** RGB 与纹理分支若要求前者根本没有 texture slot，就必须有
   RadRay 自己的 trace-time `BindingBuilder`、symbolic resource handle 和 variant-level layout；
   只剪裁 LC 的 `Var` / `Expr` 不够
3. **改变 stage 类型/接口：困难但可表达。** 当前 `_POINT_SHADOW_LAYERED` 会改变 vertex 返回结构，
   运行期 `bool` 无法改变 C++ 返回类型；需要模板实例化 + 运行期分派 + type-erased trace 结果，
   或拆成两个 Pass

当前真实变体正好覆盖后两档：

- `forward_pass` 的 8 个维度都只影响 Pixel；若追求 exact active resources，5 个贴图开关和两种
  阴影资源开关会改变 binding 集合
- `shadow_pass` 的 `PointShadowLayered` 只影响 Vertex，但会改变 v2p 结构

### 5.3 LC 光栅 codegen 对 stage 去重的限制

`RasterCodegen(vertex, pixel)` 一次生成一份同时包含 VS/PS 的 HLSL，以 `#ifdef VS` /
`#elif defined(PS)` 分支，再由 DXC 用 `/DVS`、`/DPS` 编译两次。若直接按**整份生成 HLSL**哈希，
Pixel-only 变体也会改变 VS 的源 key，旧系统的 stage 投影去重不会自然保留。

这不影响功能正确性，但影响 trace/编译放大。后续设计必须明确：第一期接受整对重编，还是增加
stage-specific trace/config 与 codegen/cache 机制。当前未裁决。

### 5.4 UE5 源码研究结论

完整证据在 `docs/research/ue5-material-resource-layout.md`。后续会话只读该报告，不要重新扫描 UE5。

- Static Switch 决定**实际参与编译的 active resource 集合**。Legacy 路径只编译选中输入；
  MaterialIR 为报错检查会 Build 两侧，但 active analysis/link/HLSL 只保留选中依赖
- 图级 `ReferencedTextures` 可能含未选分支纹理，但它不是 shader binding 集合
- 纯 RGB / `VectorParameter` 不产生 texture uniform/SRV contract；active `TextureSample` 才产生
- active texture 参数值为空时，UE 使用同类型 fallback texture；只替换资源对象，不改变 layout
- 两个 permutation 的资源数量、类型和 stage 使用相同时，Material/native layout 通常可共享
- RGB 与纹理导致资源形状不同时，Vulkan bindful layout 通常精确变化；D3D12 会量化资源计数，
  不同原始集合仍可能复用同一 root signature；bindless 路径进一步归一到全局 layout
- 所以“Unity 选择相同 binding layout，UE5 选择不同 binding layout”是过度概括。准确说法是：
  **UE5 专门化 active resource 集合，再由 RHI 缓存/归一化 native layout**

### 5.5 当前推荐，尚未裁决

推荐规则：`BindingLayout` 由当前 Variant 的 active trace 自然产出，不预先要求所有 Variant
相同或不同；内容相同就由 `PipelineLayoutCache` 共享，active resource 形状不同时才产生不同
logical layout。

推荐把最小验证从无变体的 `error_pass` 提高为同时证明：

1. RGB 变体没有 texture binding，纹理变体有 texture binding，且相同 layout 能内容去重
2. 一个改变 stage 输出类型的模板变体能经 type-erased trace 结果进入统一编译/PSO 链
3. D3D12 + Vulkan 都能完成 trace → codegen → DXC → PSO → 像素读回

**用户尚未接受上述规则与验证门槛，不得写入 ADR/todo 为既定决策。**

---

## 6. 下一步

按顺序：

1. **先继续第 5 节的 Variant grilling**，每次只裁决一个问题：
   - 是否接受“active trace 产出 layout，相同内容由 cache 共享”的规则
   - 接受后，再裁决新的 go/no-go 原型门槛
   - 再裁决第一期是否接受 VS/PS 整对重编，还是必须先保住 stage 投影去重
2. Variant 模型闭合后，更新 ADR-0015、`docs/todo/cpp-trace-shader-frontend.md` 与必要的领域术语；
   在此之前它们保持提议/待实施状态
3. **重新提出第 4 节的 codegen 失败问题**，拿到裁决后写进 todo 第 7 节风险 #2
4. 继续 grilling 尚未触及的分支（每次只问一个，等回答）：
   - 薄 DSL 层放哪个模块？（新建 `modules/shaderdsl`？还是重建 `modules/shader`？依赖链 `core ← shader ← render ← runtime` 是否保持）
   - LC 怎么进 `third_party/`？（`AGENTS.md` 硬规则：`third_party/` 与 `SDKs/` 是脚本填充的只读树，不得编辑 —— 但决策 3 要求改 LC 两处 CMake/头文件。这是**真冲突，必须裁决**：patch 文件？fork？还是例外）
   - `LUISA_COMPUTE_USE_SYSTEM_STL` 定 ON 还是 OFF？（决定 `luisa::string` 是不是 `std::string`）
   - `glslang` 无条件引入怎么处理？（`src/ext/CMakeLists.txt` 无 guard，会随 `luisa-compute-ext` PUBLIC 传染）
   - 第一期先做哪个 pass（原建议 `error_pass` 已被第 5.5 节的新验证门槛挑战）
   - `ADR-0002/0004/0006/0013` 的重审结论
5. 用户确认共识后才动代码。**上一轮明确要求：在他确认前不要行动。**

---

## 7. 硬约束与陷阱（易踩，务必先读）

- **`AGENTS.md` 与 `docs/adr/README.md` 是权威**。ADR 只追加不修订，改状态是唯一例外
- **`third_party/` 与 `SDKs/` 是只读树** —— 与「改 LC 两处」直接冲突，见上
- **禁异常**：不得新增 `try`/`catch`/`throw`，动手前须问用户
- **容器必须用 `radray::types` 别名**；协程用 `radray::task`；`RADRAY_IS_DEBUG` 而非 `NDEBUG`
- **后续会话的研究型任务全部起子代理**，避免外部大型代码库与资料污染主上下文；主会话只消费研究报告和结论。实现、编辑与普通仓库定位不受此条限制
- 当前运行 `python tools/check_docs.py` 返回 `ok`。`tools/check_docs.py` 本身另有非本会话修改，后续不能把这个结果误当未修改检查器的基线，仍应按实际工作树复跑
- **`check_docs.py` 只扫文档头 8 行**找 `适用:`/`权威:`/`锚点:`。头块写超 8 行会误报（我已踩过一次）
- **不要并发跑构建和测试**（`AGENTS.md`）
- **`ctest -R` 匹配 gtest suite 名，不是 cmake target 名**
- 回复与文档**用中文**

---

## 8. 关键事实（已核实，可直接引用，无需重查）

- `modules/shader` = 18,431 行（11,748 非测试 + 6,619 测试），服务 1,472 行 HLSL，比 12.5:1
- 归属拆分：约 4,600 行是 DXC 反射本身的成本；约 10,700 行是 manifest/变体/artifact 层（即握手成本）
- **这套系统零生产调用方**：`PipelineStateCache::GetOrCreateGraphics` 只有测试调用；`LoadShaderAsset` 生产侧零调用点；`RenderPipelinePass` 无子类；两个 example 因缺头文件被排除在构建外。296 个 shader 层用例从未在测试外画出一个像素 → **这是改造最便宜的时刻**
- imgui 的 4 个 shader getter 零调用点，确认死代码
- LC 的**光栅路径本身就是 HLSL→DXC**，Vulkan 也是（`ast2xir.cpp:1289` 对 `RASTER_STAGE` 是 `LUISA_NOT_IMPLEMENTED()`，故光栅进不了它那个 20,364 行的自研 XIR→SPIR-V 发射器）
- LC 的 `ROADMAP.md` 最后修改于 **2022-12-17**，全部过期，不可引用
- `luisa-compute-ast` 零 runtime 符号依赖；`hlsl-codegen` 那条 `PUBLIC luisa-compute-runtime` 边无符号依据（23 个 `LUISA_RUNTIME_API` 在 codegen 目录零命中）
- 仓库自带无 Device 的 codegen 测试为证：`src/tests/unit/ext/test_hlsl_validation_codegen.cpp`
- **未编译验证过的两处**（最小验证首要目标）：手工构造参数时的 cbuffer 打包与 register 分配；生成的 HLSL 能否端到端过 DXC
- `test_vertical_slice.cpp` 现为 D3D12/Vulkan × Aot/Jit 四参数；第一期无 AOT，退化为按后端两参数
- LC 的宿主 C++ 分支确实能形成 AST 变体，但 LC 没有提供 RadRay 所需的 typed Variant 请求、
  active binding layout、结构变体 type erasure 与 stage 投影缓存；这些是薄 DSL 的新增职责
- `Variant` 请求身份与生成 HLSL 的产物身份是两个 key，不能合并

---

## 9. 建议调用的 skills

- **`grilling`** — 从第 6 节第 1 步继续 Variant 分支。规则：一次一个问题，等回答；每个问题给出推荐答案；事实自己查不要问；用户确认前不得行动
- **`domain-modeling`** — 与 grilling 配合。新术语定下就立刻更新 `CONTEXT.md`（它是纯词汇表，不放实现细节）；ADR 只在「难逆转 + 无上下文会困惑 + 真有取舍」三条全中时才提议。格式见 `C:\Users\xiaoxs\.config\opencode\skills\domain-modeling\{CONTEXT-FORMAT,ADR-FORMAT}.md`，但**仓库自己的 ADR 格式**（`docs/adr/README.md` 的固定小节：背景/决策/放弃的方案及代价/必须保持为真）优先
- **`research`** — 后续任何研究型任务必须通过背景子代理完成，并把可复用结论落到 `docs/research/`；主会话不要亲自扫描外部大型源码树
- **`handoff`** — 下一轮结束时再压缩一次

---

## 10. 环境

- `rtk` 包装 git/rg（例：`rtk git status --short`、`rtk rg -n "pattern" path`）
- 构建：`cmake --preset win-x64-debug` → `cmake --build build_debug --parallel 24`
- 测试：`ctest --preset win-x64 -R <SuiteName> --output-on-failure`
- 当前工作树另有 `AGENTS.md`、`tools/check_docs.py` 修改，来源不是本轮 Variant 研究；不得覆盖或回退
- 本轮新增 `docs/research/ue5-material-resource-layout.md`，并更新本 handoff。用户**未要求提交**
