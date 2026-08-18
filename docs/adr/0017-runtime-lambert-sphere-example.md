# ADR-0017 runtime Lambert sphere example

状态: 部分被 ADR-0043、ADR-0047、ADR-0048 取代
日期: 2026-08
影响: `examples/example_lambert_sphere`、`modules/runtime` 的 pipeline 注入入口、shader JIT、
`docs/guide/build-test.md`

## 背景

需要一个可运行的最小样例，验证当前 runtime 在真实窗口帧循环中驱动 runtime shader JIT、
target-native layout、PSO、资源上传、深度附件和自定义 RenderPipeline 的完整路径。旧的 sphere
与 glTF 示例依赖已经删除的 material/primitive API，不能直接恢复。

## 当前结论

- 样例是普通 executable，目录和目标名均为 `example_lambert_sphere`；`.cpp` 与 `.hlsl` 同目录，
  不注册 CTest。
- CMake 默认构建样例。JIT 关闭时仍构建；应用通过 `RADRAY_ENABLE_SHADER_JIT` 宏记录错误并
  返回非零码，不进入运行时窗口循环。
- runtime 只增加最小的 `RenderSystem::SetPipeline(unique_ptr<RenderPipeline>)` owning 注入入口。
  example 在 `OnInit` 中注入自己的 `LambertPipeline`；不扩展尚未完成的 scene/primitive proxy 路径。
- `LambertPipeline` 使用 `CameraComponent`，并通过 `LambertPass : RenderPipelinePass` 录制绘制。
  example 在 `OnInit` 通过 `World::SpawnActor` 创建 camera actor 并添加组件；pipeline 只借用
  framework 的 `Scene`/`CameraComponent` 指针。Camera 固定在 `(0, 0, -3)`，FOVY 为 60 度，
  near/far 为 `0.1/100`。
- 球体由 example 调用 `TriangleMesh::InitAsUVSphere` 在 CPU 侧生成，复用 core 的顶点绕序和
  法线数据，再通过 `TriangleMesh::ToSimpleMeshResource` 转为 framework mesh resource；首次
  帧录制时由 `ResourceUploader::UploadMeshResource` 上传 device-local vertex/index buffer，
  之后复用 framework 返回的 `GpuMesh`。
- HLSL 是同目录的 Lambert pass，include 根是工程根目录下的 `shaderlib`。调用侧按 shader-root
  递归收集 `<...>` 依赖并以逻辑 root-relative 名称提交给 JIT；JIT request 的 root
  `SourceName` 固定为 `example_lambert_sphere.hlsl`，不复制 shaderlib，不维护第二套 include
  根。shader 使用 `lighting/lights.hlsli` 的 `DirectionalLight` ABI。
- shader artifact 的 decoder 与 layout builder 按实际 backend 选择 DXIL/SPIR-V view；example
  仅在 layout 创建处使用现有 backend-specific `DeviceD3D12` / `DeviceVulkan` 入口，PSO、
  parameter set 和命令录制继续使用公共 RHI。
- PSO 的 vertex input location 从 decoded artifact 的 `VertexInputs` 查找 `POSITION` 与
  `NORMAL`；C++ 只提供样例 vertex struct 的 stride/offset，不跨 target 硬编码 location。
- shader 只做单方向光直接 Lambert：`max(dot(N, L), 0) * Albedo * Irradiance / PI`；不加
  环境光、IBL、纹理、specular、normal map 或 tone mapping。FrameData 通过
  `VK_BINDING(0, 0)`、`DirectionalLight` 通过 `VK_BINDING(1, 0)` 分别绑定为两个
  ConstantBuffer，每个 flight 有独立 upload buffer/parameter set。分开绑定用于同时覆盖
  多个 constant-buffer binding 的 JIT、layout 和参数更新路径。
- `DirectionalLight` 的 CPU mirror 只在 example 内定义为两个连续 `Eigen::Vector4f`，并以
  `sizeof/offsetof` 静态断言校验 `lighting/lights.hlsli` 的 ABI；不向 shaderlib 增加 CPU 头。
- swapchain 为线性 `BGRA8_UNORM` 时，Lambert shader 末端只做 linear-to-sRGB 转换，不加入
  tone mapping。
- example 使用固定 `Albedo = (0.72, 0.36, 0.12)` 与 `Irradiance = (3.0, 3.0, 3.0)`，
  只为稳定观察动态 Lambert 明暗，不引入材质系统。
- 两个 constant buffer 的上传复用 `DynamicCBufferArena`；每个 flight 懒创建自己的 arena
  （绑定该 flight 的 `HostWriteBatch`）和 parameter set，每帧 reset 当前 arena 后更新
  `Frame`/`Light` 的 constant-buffer slice。
- 默认 D3D12，`--vulkan` 选择 Vulkan；JIT target、artifact decoder 和 native layout 按实际
  device 选择；JIT 先对同一 root source 做 contract discovery，再只编译实际后端需要的
  单一 target lane。`--multithread` 启用 threaded runner，`--valid-layer` 启用 validation。
- 窗口标题为 `example_lambert_sphere`，默认尺寸为 `1280x720`；resize 后按当前 backbuffer
  尺寸重建 viewport、D24S8 depth attachment 和 framebuffer。
- swapchain 显式使用 `BGRA8_UNORM` 与 `FIFO`，不依赖 `ApplicationRuntimeDescriptor` 的未设定
  format，也不启用 sRGB 或 tearing 变体。
- 方向光相位由 pipeline 在渲染线程按 `ctx.Frame.DeltaTime()` 累积，固定为 6 秒周期；以
  `theta = 2*pi*phase/6s` 驱动 `DirectionalLight.Direction =
  float3(0.8*cos(theta), -0.6, 0.8*sin(theta))`，不修改并发中的 `Scene`/`LightComponent`，
  也不引入额外共享状态。
- 使用 `D24_UNORM_S8_UINT` depth attachment；尺寸变化时重建 depth texture/view，并清理旧
  framebuffer 缓存。颜色、深度和 stencil 每帧清除，资源状态转换保持显式。
- CPU/GPU 矩阵保持 Eigen 列主序与 HLSL `mul(Matrix, vector)` 约定一致，不做矩阵 transpose。
  用户要求两后端矩阵一致，Vulkan Y 反转由负 viewport height 完成。

## viewport 与 front-face 调查

- 旧 SRP 时代的 `examples/gltf_viewer/gltf_viewer.cpp`（`818b152`）在应用 helper 中对 Vulkan
  使用 `Y = height`、`Height = -height`；`2e02990` 将同一逻辑移入
  `modules/runtime/src/render_framework/forward_pipeline.cpp`，之后的 `sphere_demo` 由该管线继承。
- 旧不透明 pass 使用 `PrimitiveState::Default()`；该默认值为 `FrontFace::CW + CullMode::Back`。
  `bcf56b8` 只为透明材质改成 `CullMode::None`，没有为 Vulkan 单独翻转 `FrontFace`。
- 因此本样例不增加后端专用 FrontFace 修正，也不把负 viewport 与投影矩阵 Y 翻转叠加；不透明
  Lambert 球沿用 `PrimitiveState::Default()`。只有改变 Y 反转策略或改变网格绕序时，才需要重新
  评估 FrontFace/CullMode。
- 外部规范核查与此结论一致：Microsoft 的 D3D viewport 说明以左上角为屏幕原点，并由 viewport
  变换翻转 Y；Khronos 的 `VkViewport` 说明负 `height` 会在 viewport 变换前取反 clip-space Y，
  且 `y` 应指向 viewport 的 lower-left。取 `VkViewport{0, height, width, -height, 0, 1}` 后，
  `x_ndc`、`y_ndc` 和深度映射与 D3D12 的 `VkViewport{0, 0, width, height, 0, 1}` 对齐：不会
  额外左右翻转，也不会上下翻转。Vulkan front-face 面积按 framebuffer coordinates 计算；负
  `height` 会改变面积符号，这是对比 Vulkan 正高度时的 winding 变化，但它同时复现了 D3D 的
  framebuffer Y 方向，因此不需要额外翻转 FrontFace。后一句是由两套规范的 viewport/front-face
  定义推导出的结论。
- Vulkan 规范还规定：物理设备 API 版本低于 1.1 时，负 `height` 需要启用
  `VK_KHR_maintenance1` 或 `VK_AMD_negative_viewport_height`。runtime 仅在物理设备 API
  版本低于 1.1 时把 `VK_KHR_maintenance1` 加入现有 device-extension 集合；Vulkan 1.1
  及以上直接使用核心能力，不启用已提升进核心的扩展，也不加入 AMD fallback。runtime 和
  example 不新增能力检测、分支或提示，扩展缺失时沿用现有 device-extension 初始化结果。

## 放弃的方案及代价

- 直接依赖 `Application::OnRenderView`：当前无 pipeline 时 runtime 不调用它，无法形成可运行绘制链。
- 在 example 中恢复 scene/primitive/material 体系：对应 API 已被 TODO 迁移删除，且会把样例绑定到
  尚未完成的 primitive draw path。
- 复制 shaderlib 到 executable 输出目录：破坏“工程根目录是 shader include root”的测试条件，
  并掩盖 source/include identity 问题。
- 固定单一 backend 或 JIT target：只能验证一条 target lane，无法覆盖 DXIL/SPIR-V layout 边界。
- 用 push constants 承载完整 FrameData：矩阵、光照和材质数据不适合依赖 Vulkan 最低 push-constant
  大小保证。

## 必须保持为真

- shader source identity 使用逻辑 root-relative 名称；物理路径不进入 source identity。
- shader binding 只使用 HLSL declaration name、`VK_BINDING` 和当前 artifact 的 `BindingHandle`；
  不新增 register、sidecar metadata 或 runtime reflection。
- runtime 负责 frame lifecycle、acquire、submit、present；example pipeline 只录制内容和自有资源。
- pipeline 复用 `RenderPipeline` 默认的目标准备与相机阶段；`LambertPass` 在
  `OnAddRenderPasses` 入队，并在成功录制后标记对应 `RenderPipelineTarget::ContentDrawn`，
  防止 framework 的兜底 clear 覆盖绘制。
- `RADRAY_ENABLE_SHADER_JIT=OFF` 的样例启动必须在创建窗口前失败并返回非零码。
- viewport/front-face 的约定必须保持为：矩阵后端一致，Vulkan 通过负 viewport height 做 Y 反转，
  不透明 pass 使用 `PrimitiveState::Default()` 的 `FrontFace::CW + CullMode::Back`。
