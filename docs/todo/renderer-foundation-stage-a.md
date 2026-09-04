> - 适用: 实施 Stage A 的 workload/output/view、capabilities、RenderGraph、资源池和 history
> - 权威: 本文记录实施范围与验收状态；已落地行为以 architecture 文档为准
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/runtime/include/radray/runtime/render_framework/render_pipeline.h`, `modules/runtime/src/render_system.cpp`, `modules/runtime/src/forward_pipeline/forward_pipeline.cpp`

# Renderer Foundation — Stage A

输入为用户提供的 `RadRay_Stage_A_Renderer_Foundation_Detailed_Design_source.md` 与
`RadRay_Stage_A_claim_source_ledger.md`（2026-09-04，基线
`dfe62a8a606bb019865324b76330f546db93e507`）。两份输入作为设计与证据，不覆盖仓库硬规则。

## 范围与不变量

- 保留 game-thread `PrepareFrame` / render-thread `Render` 与 per-flight 值快照和资产保活。
- 所有新 runtime 系统仍在 `radrayruntime`，RHI 仅增加 backend 事实、同步与标签。
- Output 使用不复用的单调 ID；host 负责 acquire、fallback clear、真实状态收口与 present。
- 一次 Render 内构建、编译、realize、串行执行一张 graph，使用既有 Direct command buffer。
- graph generation 检查句柄；按 mip × array layer 验证内容与状态；内容依赖决定 culling，
  顺序 hazard 决定执行依赖；discard overwrite 不保留旧 producer。
- 所有 allocation/view/pass/framebuffer 在录制前实现；失败保留实际状态供 host 恢复。
- 每 flight 精确 descriptor 对象池；不复用同 graph 的对象，不做 heap aliasing。
- pool/history 销毁 view 前摘除 framebuffer；trim 只在 flight 安全点，history generation
  替换进入当前 flight retire bin，到同 flight 下次安全复用再释放。
- history 只有成功写入后提交，skip/失败不旋转；camera cut/resize/格式/sample 改变使内容失效。
- 最终 pipeline context 不公开 AppFrameContext、CommandBuffer、AppWindow 或 swapchain token。
- 不做 async compute、并行录制、aliasing、pass merge、culling/renderer list 或 material multipass。

## 检查站

| 阶段 | 实施与验收 | 状态 |
|---|---|---|
| A0 | capabilities、完整 texture support/validation、UAV barrier、debug labels；双 backend 创建/读回 | 完成 |
| A1 | output registry、per-flight plan、view resolve、显式 acquire、zero/offscreen/multi-output | 完成 |
| A2 | per-flight texture/buffer/view pool、physical states、统计、safe trim、framebuffer 摘除 | 完成 |
| A3 | graph handle/pass/resource、CPU validation、版本依赖/culling、subresource/hazard、确定性报告 | 完成 |
| A4 | graph realization/executor、raster/compute/copy、external state roundtrip、失败恢复 | 完成 |
| A5 | Forward 迁移、删 per-window depth/manual barrier、按 view 保存 draws、兼容 PSO key | 完成 |
| A6 | relative size、previous matrices、history tokens/retirement、失效与双 backend 跨帧读回 | 完成 |
| A7 | 删除旧 context/direct API、文档/ADR/静态边界、回归与性能基线 | 完成 |

## 验收范围

CPU 覆盖错误 descriptor/handle/access/load/store、subresource 初始化、覆盖裁剪、稳定顺序与 dump、
pool identity/flight isolation/trim、view rect/projection 与 history 状态机。GPU 分别覆盖 D3D12 与
Vulkan 的 capability 创建一致性、raster → sample、compute → raster、same-state UAV、copy、mip、
fallback、外部 output、history；延续 Forward 像素与 200+ 帧多线程 mutation/resize 测试。
固定 graph 在各 flight warmup 后不增加 native resource/view/framebuffer；记录 100/1000 pass
编译基线。build 与 test 顺序执行；无法运行的验证明确记录，不能记为通过。

## 核验差异与结果

- 起始 checkout 与设计基线一致，工作区干净。基线 D3D12 range transition 只处理第一个
  mip/layer，现已展开全部 mip/layer/plane；新增显式 resource UAV memory barrier。
- 最终命名为 `RenderDeviceCapabilities`，避免 Windows 的 `DeviceCapabilities` 宏冲突。
- graph 的声明顺序已经是稳定拓扑序：内容/hazard edge 只指向后声明 pass，因此无需再次排序。
- execute context 使用窄 commands facade。设计草案中的原生 encoder 有 `GetCommandBuffer`，
  会恢复旁路 barrier/submit，已用编译期边界测试封住。
- 编译测试使用纯 CPU device stub，真实 capability/创建/执行一致性另在双 backend 验证。
- Forward 像素测试发现旧 fixture quad 绕序与默认背面剔除不符，已修正索引，保留实际剔除状态。
- 当前契约见 [renderer foundation](../architecture/renderer-foundation.md)，决策见
  [ADR-0054](../adr/0054-explicit-workloads-and-single-queue-render-graph.md)。

## 2026-09-04 验证记录

环境：Windows、MSVC 19.51 / Visual Studio 18.9.1、Intel i7-11700F、NVIDIA RTX 3060，
Vulkan 1.4.325 / NVIDIA 591.44，Vulkan validation layer 1.4.357.0。
测试启用 D3D12 debug layer / Vulkan validation layer；GPU 测试都实际运行，不以 SKIP 计通过。

- 最终 Debug 全量构建和 316/316 CTest 通过（57.82 秒），包含 token index guard、纯 CPU
  capability/subresource 用例、零输出 graph 执行和同帧部分 output fallback；无跳过项。
- 双 backend 102 帧固定 mip clear/readback：每个 pool 累计创建 1 texture、1 view，101 次命中，
  registry 保持 1 framebuffer；物理 Before state 从首帧 Undefined 变为后续 CopySource。
- 双 backend raster→sample、compute UAV→raster、连续 UAV、copy、history 跨帧读回及 resize retire
  通过；Clear/Load 共享 PSO；模拟 raster/compute encoder 失败后从真实状态恢复，读回正确。
- 双 backend Forward 220 帧 threaded actor/material/texture/light mutation 与 resize 通过，
  每 flight pool texture 数量有界；独立 camera/aspect 的多个 offscreen family 像素通过。
- compiler/JIT 关闭的独立 Release 构建通过，15/15 CPU/层级测试通过。实际
  `ninja -C build_stage_a_release -t commands test_render_graph_compile` 的 link command 包含
  runtime/render/shader/core，不含 shadercompiler/DXC，符合可选 client 依赖方向。
- AddressSanitizer Debug（关闭 mimalloc，`/fsanitize=address`，`/INCREMENTAL:NO`，
  `ASAN_OPTIONS=alloc_dealloc_mismatch=1`）29/29 通过，覆盖纯 CPU compiler、双 backend graph、
  history、output、独立多视图像素和 220 帧线程/resize；未报告内存错误。
- 本次 Windows/MSVC 配置没有 ThreadSanitizer；不把 ASAN 通过表述为 data-race 检查通过。
- `python tools/check_docs.py` 和 `git diff --check` 通过。

### CPU Release 基线

`test_render_graph_compile`，100 次重复，每个 pass 为独立 side-effect compute 声明；无 GPU，
无原生资源。总时间包含 graph setup、Compile、JSON/DOT dump 与确定性比较；Compile 单独计时。
数值是本机基线，不是跨机器阈值；统计的 allocation 为 native resource creation，不是 CPU malloc。

| Pass 数 | 平均 Compile | 平均 setup/compile/dump | native create |
|---|---|---|---|
| 100 | 20 µs | 245 µs | 0 |
| 1000 | 138 µs | 1952 µs | 0 |

复现命令见 [build-test](../guide/build-test.md)。完整实施未引入 async compute、pass merge、heap
aliasing 或 temporal effect；Stage A Forward 仍限制每 family 一个 view、RenderScale=1。
