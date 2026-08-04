# ADR-0015 变体是 C++ 函数参数，烘焙集合也写在 C++ 里

状态: 提议
日期: 2026-08
影响: `modules/shader` 的变体机制；`tools/shader_cook`。
取代 [ADR-0005](0005-keyword-groups-declared-in-hlsl.md)

## 背景

ADR-0005 把 keyword 组的声明权威放在 HLSL 的 `#pragma radray_keyword_group` 里，理由充分：
声明与它守护的 `#ifdef` 同文件，是唯一不会失同步的位置；解析基于 `dxc -P` 的预处理输出，
块注释与续行符由编译器正确处理。

但这套机制的存在前提是 **keyword 是预处理期的字符串开关**。字符串开关必须在外部声明"有哪些组、
组内哪些宏互斥、是否允许全关"，因为预处理器本身表达不出这些约束。这催生了约 587 行变体机器
（`ShaderVariantDomain`、`ShaderVariantKey`、`ExpandShaderBakeSet`、`ValidateBakeSet` 等）
与 108 个测试用例。

ADR-0014 把 shader 源真相改为 C++ 之后，`#ifdef` 与 `-D` 都不再存在，pragma 也失去了宿主。

## 决策

**变体就是 trace 函数的普通 C++ 参数。** 一个特性开关是 `bool` 形参、枚举形参或模板参数；
"哪些组合合法"由 C++ 类型系统表达，不需要外部声明域。

**预编译覆盖范围（如果需要）也写在 C++ 里** —— 一个返回变体参数列表的普通函数，
离线工具遍历它并逐个 trace。但**第一期不做离线编译**：变体在运行期按需 trace 并编译。
覆盖范围声明与它的命名（旧代码里的 cook / bake）一并推迟到真要做离线产物时再定。

由此消失的东西：9 个 `#pragma radray_keyword_group` 声明、pragma 解析与 strip、
keyword 组的跨组约束校验（组名重复、keyword 跨组撞名）、`ShaderVariantKey` 的槽位编码、
以及"manifest 里把 `_BASECOLOR_MAP` 拼成 `_BASECOLOR_MAPP` 不会有任何报错"这一整类错误 ——
拼错标识符现在是编译错误。

**离线工具因此必须链接 shader 定义。** 今天 `radray_shader_cook` 扫描 `*.shader.json`，
shader 内容对它是数据；之后 shader 是代码，任何离线工具必须是链接了 shader 定义的宿主程序。
这是推迟离线编译的一个附带理由：工具形态本身要变。

**产物 key 的输入从源文件闭包改为 codegen 输出。** 今天 key 依赖
`ComputeShaderSourceIdentity`（HLSL 文件的 include 闭包哈希）；HLSL 源文件不再存在后，
改为对生成的 HLSL 文本取哈希。这比原方案更准：C++ 侧改动若不影响生成结果（改注释、
重排不影响输出的逻辑），key 不变、产物可复用，而按源文件闭包哈希会误判失效。
第一期只把它用作进程内/磁盘缓存的键；ADR-0004 的内容寻址原则不变，只是收窄了 key 的输入定义。

## 放弃的方案及代价

- **保留 pragma 或 manifest 声明变体域**。宿主（HLSL 源文件）已不存在。
- **变体参数用 C++ 类型，但 bake set 留在配置文件里**。ADR-0003:56 曾论证"改烘焙范围不该动
  shader 源码，否则会让所有产物 cache 失效"——但那条理由的前提是变体域写在 HLSL 源码里、
  且 key 依赖源文件哈希。两个前提都已改变：bake set 是独立的 C++ 函数，改它不动 shader 定义；
  key 只取决于生成的 HLSL，改 bake set 不影响已有 variant 的 key。而 JSON → C++ 变体参数需要
  一层手写映射（C++ 结构体无法从 JSON 自动构造），那是新的一道握手。
- **彻底不预烘，只靠运行时 trace + DXC 加磁盘缓存**（LuisaCompute 的做法：按生成 HLSL 的 MD5
  索引 `bin/.cache`）。发布包必须带 `dxcompiler.dll`（20.5 MB）且首帧卡顿，与"发布包不带 DXC、
  只读烘焙产物"的既有前提相抵。
- **保留 `ShaderVariantKey` 作为运行期变体标识**。它本来就是作者/cook 期概念，运行期已经用
  artifact key 的 `ShaderHash`；C++ 化后更没有理由保留一套并行编码。

## 必须保持为真

- 不存在描述变体域或预编译范围的非 C++ 文件。
- 任何离线工具都链接 shader 定义；它不解析任何 shader 描述文件。
- 产物 key 的输入是 codegen 产出的 HLSL 文本，不是任何源文件的路径或内容。
- 拼错一个特性开关的名字是编译错误，不是静默产出错误变体。
- 第一期不引入 "cook" / "bake" 命名，也不产出可分发的离线产物。
