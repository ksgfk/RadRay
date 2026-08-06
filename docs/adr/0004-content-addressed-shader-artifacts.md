# ADR-0004 AOT 产物内容寻址，且与 manifest 同处一地

状态: 已被 ADR-0016 取代
日期: 2026-07
影响: `GetShaderArtifactDirectory`、`MakeShaderArtifactBlobPath`、`ShaderArtifactIndex`；`tools/shader_cook`

## 背景

一份 manifest 会烘出 `category × variant × stage` 个字节码。需要决定两件事：
blob 文件怎么命名，以及产物目录放哪。

同时有一个可观测的机会：stage 投影（见 `ShaderKeywordGroupDesc::Stages`）使多个
program variant 常常共用同一份 stage 字节码——一个只影响 pixel stage 的 keyword，
其开与关对应的 VS 字节码逐字节相同。

## 决策

**目录约定**：manifest `foo.shader.json` 的产物放在同目录的同名文件夹 `foo/`。

```
forward_pass.shader.json
forward_pass/
    index.json
    dxil/a3f29c1b40e8d715....bin
    spirv/8c01ae52f7d3b064....bin
```

**blob 内容寻址**：文件名即 stage artifact key 的 hex。多个 variant 投影到同一份字节码时
自然去重，无需任何显式去重逻辑。

**按 category 分子目录**：使"只发布 DXIL"退化为删除一个目录。

**blob 自验**：容器带头部（magic + 版本 + stage/category + 内容哈希），单个 blob 文件可
独立校验，不依赖 index.json 的正确性。

**index.json 按源文件分别记录身份**（`ShaderArtifactIndex::Sources` 是列表）：artifact key
是按该 pass 自己源文件的身份算的，而一个资产内不同 pass 可以有不同 `Source`。

**`radray_shader_cook` 刻意不提供 `--output`**：运行时 `ShaderResolver` 用同一个
`GetShaderArtifactDirectory(manifest)` 从 manifest 路径反推产物目录。一旦可以自定义输出位置，
布局约定就有了第二个真相，而运行时那一侧看不到构建时传的参数。要换位置就换 manifest 的位置
（构建里 cook 的是部署到输出目录的那份 shaderlib，不是源码树）。

## 放弃的方案及代价

- **blob 按 `<pass>_<stage>_<variant描述>.bin` 命名**。可读，但去重必须显式实现且容易漏；
  且变体描述随 keyword 数量增长会撞上路径长度限制。
- **全部 blob 打成单个 pack 文件**。省 inode，但增量烘焙要重写整包，且"只发布 DXIL"
  变成需要重新打包而不是删目录。
- **产物放在集中的 `build/shaders/` 下，按逻辑名索引**。需要一份名字→路径的映射，
  而那份映射就是第二个真相；运行时反推不出来，只能再传一个参数进来。
- **index.json 只存一份合并的源码哈希**。多源资产会坏：`Strict` 下永远判为过期，
  `Lenient` 下永远算错 key。这个 bug 曾真实存在。
- **`ShaderArtifactEntry::Keywords` 参与 key 或用于查找**。不行：运行时必须能纯函数地
  从 (pass, stage, category, 投影后 defines, SourceIdentity, ToolchainHash) 算出 key 再查表，
  keyword 名只是给人看的。让它参与 key 会引入名字顺序、大小写这类无关变量。

## 必须保持为真

- blob 文件名是 `ShaderHash::ToHex()` 的输出，32 个小写 hex 字符。
- `GetShaderArtifactDirectory` 去掉全部后缀（`.shader.json` 是两级），
  故 `forward_pass.shader.json` → `forward_pass`，不是 `forward_pass.shader`。
- `radray_shader_cook` 没有 `--output` 或等价选项。
- `ReadShaderArtifactBlob` 校验 magic、版本、内容哈希，任一不符返回 nullopt。
- `ShaderArtifactIndex::Sources` 是列表而不是单个哈希。
- 改动 blob 容器格式或 index schema 时递增 `kShaderArtifactFormatVersion`。

## 后续裁决：首次部署前重定义 v1（2026-08）

[ADR-0013](0013-vertex-stage-interface-projection.md) 要求 vertex-stage blob 保存最小输入接口投影。
作出该决定时，blob v1 从未实际部署或被发布运行时消费，因此本次作为**唯一的首次部署前例外**，
原地重定义 v1 grammar：header 使用覆盖 `key` / `stage` / `category` 与完整嵌套 payload 的
`ContentHash`，vertex payload 先写接口再写 bytecode，非 vertex payload 只写 bytecode；
`formatVersion` 本次仍为 1。

不提供旧 v1 兼容分支。已有本地旧产物必须重新 cook，新 reader 只需拒绝它们，不承诺具体拒绝
发生在 hash 还是结构校验阶段。本次例外不建立“已部署格式也可原地改写”的先例。

从这份 v1 基线开始，上文最后一条约束扩展为：改动 blob 容器格式、`index.json` schema，
**或任何写入 blob 的派生数据之提取逻辑**，都必须递增 `kShaderArtifactFormatVersion`。
版本同时进入 artifact key 与 toolchain hash，递增会强制重新 cook；否则增量 cook 可能命中旧 key，
直接复用按旧逻辑提取的 vertex interface，静默吞掉提取修复。
