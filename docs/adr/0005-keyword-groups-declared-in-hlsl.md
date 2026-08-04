# ADR-0005 keyword 组在 HLSL 里用 #pragma 声明

状态: 已被 ADR-0015 取代
日期: 2026-07
影响: `shaderlib/**/*.hlsl(i)` 里的 `#pragma radray_keyword_group`；`ParseShaderKeywordPragmas`；`radray_shader_gen`

## 背景

一个 keyword 组由哪些互斥宏构成、是否允许全关、影响哪些 stage——这些信息既要进 manifest
（`ShaderKeywordGroupDesc`），又必须与 HLSL 里的 `#ifdef` 对应。声明放哪？

## 决策

**HLSL 源码是唯一权威。** 用 pragma 声明，`radray_shader_gen` 从 DXC 的预处理输出解析它并
写进生成的 manifest：

```hlsl
#pragma radray_keyword_group(BaseColorMap, _BASECOLOR_MAP) stages(Pixel)
#pragma radray_keyword_group(AlphaMode, _ALPHATEST_ON, _ALPHABLEND_ON) stages(Pixel)
#pragma radray_keyword_group(Lighting, _LIT, _UNLIT) stages(Vertex, Pixel) required
```

第一个标识符是组名，其后是组内互斥的 keyword（至少一个）。`stages(...)` 省略取 `Graphics`；
`required` 关掉 `IsOptional`。DXC 忽略未知 pragma，这些行不影响编译（`-WX` 亦不告警）。

**默认采纳整条 include 链。** 预处理已把 include 展开，于是提供某组绑定的那个头文件可以把
对应的 keyword 声明放在**自己身边**——声明与被它守护的 `#ifdef` 同文件，是唯一不会失同步的
位置。例如 `forward_pipeline/view.hlsli` 声明阴影绑定，就由它自己声明
`PointShadows` / `DirectionalShadows`，每个 include 它的入口文件自动继承。
代价是"include 了某个头文件"就意味着"继承它的全部变体维度"，即使本 shader 并不使用；
需要时用 `ShaderTemplateOptions::KeywordPragmaScope = EntryFileOnly` 切换。

**pragma 必须在无条件位置。** 预处理 respect `-D`，所以被 `#ifdef` 包住的 pragma 只在对应宏
开启时才可见——那会形成"要先知道 keyword 才能发现 keyword"的循环。头文件的 include guard
不算条件块（它总是成立）。

**解析基于 `dxc -P` 的预处理输出**，不是自己写的词法扫描。于是块注释、续行符、`#if 0`
全部由编译器正确处理：注释掉的 pragma 不会被误认为声明。

**`StripShaderKeywordPragmas` 把 pragma 行替换为空行而非删除**，以保持行号不变，
使 DXC 对这份文本报的错误位置仍与预处理输出一一对应，也不破坏其中的 `#line` 指令语义。

## 放弃的方案及代价

- **在 manifest 里手写 KeywordGroups**。等于把同一份声明抄两遍，而两侧**无法**互相校验：
  manifest 里把 `_BASECOLOR_MAP` 拼成 `_BASECOLOR_MAPP` 不会有任何报错，只会静默编出一个
  所有 `#ifdef` 都不成立的变体。这是最难发现的一类错误。
- **扫源码里的 `#ifdef` 自动推导 keyword 组**。刻意不做：pragma 即契约本身。HLSL 里 `#ifdef`
  写错宏名属于普通代码 bug，不该由本工具链承担——那等于让"扫源码猜意图"参与定义 ABI。
  同时"哪些宏属于同一互斥组"、"是否允许全关"根本推导不出来。
- **交叉校验 pragma 与 `#ifdef`**（即两者都读，不一致就报错）。看似免费的安全网，实际会把
  "扫源码猜意图"引回来：一个宏在注释里、在字符串里、在未走到的 `#if` 分支里出现都会误报。
- **自己写 pragma 的词法扫描器**。块注释与续行符要正确处理才不误报，而 DXC 已经会做。
- **只采纳入口文件的声明**（即默认 `EntryFileOnly`）。那会迫使每个入口文件重复列出它 include
  进来的头文件所需的全部 keyword，重新引入抄两遍的问题。

## 必须保持为真

- 每个 `#pragma radray_keyword_group` 位于任何 `#if` / `#ifdef` 之外（include guard 除外）。
- 声明与它守护的 `#ifdef` 在同一个文件里。
- `ParseShaderKeywordPragmas` 的输入是 `Dxc::Preprocess*` 的输出，不是原始源码。
- `ParseShaderKeywordPragmas` 只做行内语法校验。组名重复、keyword 跨组撞名这类跨组约束
  由 manifest 校验统一报错——两处实现同一规则会让口径分叉。
- `StripShaderKeywordPragmas` 处理前后行数相同。
