# ADR-0048 Vulkan Y 翻转由 runtime 公共 helper 统一，RHI 仍原样透传

状态: 生效
日期: 2026-08
影响: 新增 runtime viewport helper、内置 `ForwardPipeline` 与其执行器、
`modules/runtime/include/radray/runtime/components/camera_component.h` 的注释、
`examples/example_lambert_sphere`
修订 [ADR-0017](0017-runtime-lambert-sphere-example.md) 的 viewport 结论

## 背景

ADR-0017 确定：矩阵在两个后端保持一致，Vulkan 的 Y 反转由负 viewport height 完成，并附了一份
对 D3D viewport 规范与 `VkViewport` 规范的核查 —— 取
`VkViewport{0, height, width, -height, 0, 1}` 后，NDC 的 x/y 与深度映射与 D3D12 的
`VkViewport{0, 0, width, height, 0, 1}` 对齐，且不需要额外翻转 FrontFace。

结论是对的，落地形式不是。`camera_component.h:13` 写着"**刻意不处理后端视口差异**，RHI 的
`SetViewport` 在两个后端都原样透传，不做 Y 翻转"，于是每个录制点都要自己写一遍分支。
`example_lambert_sphere.cpp:666-670` 就是这个分支，而 `test_runtime_shader_jit.cpp` 与
`test_radray_render_pso_smoke.cpp` 各自又写了一份（它们只测单一朝向，所以没写分支，
但这意味着"正确的 viewport 怎么算"在仓库里没有唯一答案）。

历史上这段逻辑搬过家：旧 `gltf_viewer` 放在应用 helper 里，`2e02990` 把它移进
`forward_pipeline.cpp`，`sphere_demo` 由该管线继承。也就是说"由管线统一提供"曾经是既有形态，
M-1 删除旧管线时连带删掉了它。

## 决策

**Y 翻转的计算收进 runtime 的一个公共 helper，内置执行器与相机路径统一使用它。
公共 RHI 的 `SetViewport` 保持原样透传。**

- runtime 提供 `MakeViewport(backend, width, height)`（以及需要时的 offset/depth range 重载），
  返回按 ADR-0017 结论构造的 `Viewport`：D3D12 用左上原点正高度，Vulkan 用
  `{0, height, width, -height, 0, 1}`。
- 内置 `ForwardPipeline`、它的执行器与样例只调用这个 helper，不再写 backend 分支。
- `render::GraphicsCommandEncoder::SetViewport` **不在后端内部翻转**。RHI 保持薄透传，
  传入什么就下发什么。
- ADR-0017 的 viewport/front-face 不变量本身不变：矩阵后端一致、Vulkan 靠负 height 做 Y 反转、
  不透明 pass 用 `PrimitiveState::Default()` 的 `FrontFace::CW + CullMode::Back`。改的只是
  "这段计算写在哪"。
- `camera_component.h` 的"刻意不处理后端视口差异"注释随之更新：相机不产出 viewport，
  但引擎存在唯一的 viewport 构造入口。

## 放弃的方案及代价

- **维持现状，每个录制点自己判断 backend**。RHI 最薄，且调用方对最终下发值有完全控制。代价是
  同一段规范推导要在样例、两个测试和未来每条 pass 里各写一遍；写错的表现是画面上下颠倒或
  被剔除，而不是编译或创建失败。ADR-0017 那份规范核查也就没有唯一的落脚代码。
- **在 Vulkan 后端的 `SetViewport` 内部翻转，调用方永远只写左上原点坐标**。调用方最省心，
  连 helper 都不需要。但这让 RHI 不再是薄透传层：`Viewport` 的语义变成"依后端解释"，
  而任何需要真实 Vulkan 语义的场合（off-screen pass、与外部 Vulkan 代码互操作、
  测试想验证负 height 本身）都得再开一个后门。ADR-0012 让 RHI 不跟踪资源状态、
  ADR-0043 让公共 layout 构造面收窄，都是同一个方向上的选择。
- **把 Y 翻转做进投影矩阵，viewport 两个后端一致**。也能得到正确画面，但会与负 viewport height
  叠加（ADR-0017 明确不叠加），且投影矩阵会因后端而异 —— 违反"矩阵后端一致"这条已确立的
  不变量，CPU 侧的视锥、拾取与 CSM 计算都要跟着分叉。
- **让 `CameraComponent` 产出 viewport**。相机确实知道 aspect，但不知道 backend，也不知道目标
  尺寸（那是 pass 的 attachment 事实）。把 backend 塞进相机会让 game framework 依赖后端枚举。

## 必须保持为真

- runtime 与 examples 中不出现第二处 `backend == Vulkan` 的 viewport 分支；
  Y 翻转只在一个 helper 内实现。
- `render::GraphicsCommandEncoder::SetViewport` 在两个后端仍原样下发传入值，不做符号或原点变换。
- 投影矩阵在两个后端相同；负 viewport height 与矩阵 Y 翻转不叠加。
- 不透明 pass 继续使用 `FrontFace::CW + CullMode::Back`，不为 Vulkan 单独翻转 FrontFace。
- `CameraComponent` 不产出 `Viewport`，也不引用 `RenderBackend`。
