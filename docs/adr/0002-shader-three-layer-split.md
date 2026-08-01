# ADR-0002 shader 系统拆成格式层 / 对象层 / 资产层

状态: 生效
日期: 2026-07
影响: `modules/shader` 整体；`modules/runtime/include/radray/runtime/shader_program.h`、`shader_asset.h`；`tools/shader_gen`、`tools/shader_cook` 的链接边界

## 背景

shader 系统最初是一整块，manifest 解析、PipelineLayout 构建、字节码缓存、`Asset` 接入全在
一起，且整体位于 `modules/render`。两个后果：

1. `tools/shader_cook` 与 `tools/shader_gen` 只需要"文本 manifest → 字节码"，却因为链了
   `radrayrender` 而吃进整个图形后端。实测约 23 MB 的 d3d12/vulkan obj，`d3d12.dll` 被硬引用，
   起因只是 `rhi.cpp.obj` 里 5 个工厂函数的 UNDEF 符号。
2. 材质层若只想拿"一个 pass 的 layout + 字节码"，会被迫拖进 `AssetManager`，
   而后者传递带来 stdexec 的编译开销。

## 决策

三层，边界按"依赖了什么"而非"看起来像什么"划：

| 层 | 位置 | 内容 | 不含 |
|---|---|---|---|
| 格式层 | `modules/shader` (`shader_manifest.h`) | manifest desc、变体域、产物索引、`ShaderResolver`、cook、哈希 | `Asset`、GPU 设备 |
| 对象层 | `modules/runtime` (`shader_program.h`) | `ShaderPassProgram`：共享 PipelineLayout 引用 + 字节码缓存 | `Asset`、`AssetManager` |
| 资产层 | `modules/runtime` (`shader_asset.h`) | `ShaderAsset`：一份 manifest 一个 Asset | — |

格式层单独成为静态库 `radrayshader`，位于依赖链的 `core ← shader ← render` 位置。
两个 CLI 只链 `radrayshader + radraycore`。

这条分界照 `image_data.h`（数据格式）与 `image_asset.h`（Asset）的既有先例，不是新发明。

## 放弃的方案及代价

- **保持单块，靠链接器裁剪**。行不通：静态库的粒度是 obj，`rhi.cpp.obj` 里的工厂函数
  引用了两个后端的 device 类，整个 obj 被拉进来就带走全部后端实现。
- **在 CLI 里 stub 掉 `Device::Create`**。能减小体积，但引入一份必须与真实实现保持同步的
  假实现，且掩盖了"CLI 根本不该知道 device 存在"这个事实。
- **`shader_program.h` 也放进 `radrayshader`**。不行，它持有活的 `PipelineLayout`（GPU 对象），
  按定义属 render 层之上。
- **把 `Asset` 接入也做进对象层**（即两层而非三层）。那会让材质层被迫依赖 `AssetManager`，
  正是要避免的第 2 个后果。

## 必须保持为真

- `radrayshader` 的 `target_link_libraries` 里没有 `radrayrender`。
- `tools/shader_gen` 与 `tools/shader_cook` 的 `target_link_libraries` 里只有
  `radrayshader`（+ 可选 `mimalloc-override`）。
- 验证链接边界用 `link /MAP` 或 `ninja -C build_debug -t commands`，**不要用
  `dumpbin /DEPENDENTS`**：Vulkan 经 volk 动态加载，不留 dll import，看不出来。
- `modules/shader/tests/*` 只链 `radrayshader`。这本身就是边界的回归测试。
- `shader_manifest.h` 不 include 任何声明 `render::Device` / `render::PipelineLayout` 的头。
