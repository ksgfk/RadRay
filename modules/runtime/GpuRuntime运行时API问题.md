先不管具体实现，从设计层面上，目前的Gpu System合理吗？

● 从设计层面来看，核心框架是合理的，但有几个值得讨论的点：

  ---
  合理的部分

  Frame/Async 二分法清晰。帧上下文绑定 surface/present 域，异步上下文脱离帧——这个分割线符合真实渲染管线的需求，不会退化成旧的 GpuSubmitContext 模糊语义。

  Surface Lease 所有权模型干净。Lease 由 FrameContext 内部持有，外部只拿借用指针，Present() 只标记不转移——这保证了 submit 时 runtime 能完整访问所有 lease
  状态。

  Helper path / Plan path 互斥是正确的约束。它逼迫使用者在需要多队列时显式切换到 plan 路径，不让 runtime 猜测物理依赖。

  GpuTask 作为值语义完成令牌也是对的。可拷贝、可等待、可依赖，内部 shared_ptr 保证 completion state 不会悬空。

  ---
  需要讨论的设计问题

  1. Runtime 自称 "GPU 边界层"，但没有资源入口

  设计文档说 runtime 是"渲染系统与其他系统交互的 GPU 边界层"。但当前 API 里：

  - 没有 CreateBuffer() / CreateTexture()
  - 没有 GetDevice() 访问器
  - 没有任何资源生命周期管理

  如果一个外部系统（ImGui、资源上传、工具窗口）要创建 GPU 资源，它只能绕过 runtime 直接用 render 层。这时 runtime 就不再是 "边界"，只是 "提交调度器"。

  需要决定：runtime 到底是不是资源的正式入口？如果是，需要补资源 API 或至少暴露 device 访问。如果不是，设计文档里 "GPU 边界层" 的定位需要收窄为 "GPU
  提交边界层"。

  2. 没有全局 flight frame 概念

  Flight frame count 目前只存在于 GpuPresentSurfaceDescriptor 里——它是 per-surface 的。

  但在真实渲染器里，per-frame 资源循环（双缓冲/三缓冲的 uniform buffer、descriptor set、upload ring buffer 等）需要一个全局的 flight frame slot
  概念。当前设计下：

  - 纯 compute 工作负载（没有 surface）没有 flight frame 概念
  - 多个 surface 的 flight frame count 可能不一致
  - 上层 render graph 做 transient 资源池化时，不知道用几个 slot

  需要决定：runtime 是否应该持有一个全局的 FlightFrameCount 和当前 frame slot index？还是说这完全留给上层？

  3. 没有 WaitIdle / Drain 语义

  ProcessTasks() 是非阻塞的。Destroy() 内部会等，但它同时也销毁设备。

  但有些场景需要 "等待所有 GPU 工作完成但不销毁"：

  - swap chain resize 前等待
  - 热重载 shader/pipeline 前等待
  - 编辑器切换场景时的安全点

  目前只能靠调用方自己追踪每个 GpuTask 逐个 Wait。

  需要决定：是否加一个 WaitIdle() 或 DrainAllTasks()？

  4. Submit 之后 context 的命令资源去向

  Submit() 把 unique_ptr<GpuFrameContext> 移走，整个 context 连同里面的 command buffer 一起进入 InFlightTask。GPU 工作完成后 ProcessTasks() 移除
  InFlightTask，所有 command buffer 随之析构。

  这意味着每帧都在分配/销毁 command buffer，没有池化。从设计层面看：

  - 如果 runtime 不负责 command buffer 池化——那谁负责？上层每帧调 Create*CommandBuffer() 拿到的都是新分配的。
  - 如果 runtime 负责——需要把已完成 task 的 command buffer 回收到池中，而不是直接丢弃。

  需要决定：command buffer 池化是 runtime 的职责还是 render graph 的职责？当前设计里这个边界是模糊的。

  ---
  小结

  当前设计作为 "GPU 提交调度 + surface 管理" 是成立的。核心对象的分工、所有权模型、同步语义都没有结构性问题。

  但如果要撑住设计文档里 "执行基础设施层 + GPU 边界层" 的定位，还需要回答：

  ┌─────────────────────┬────────────────────────────────────┬──────────────────────────────┐
  │        问题         │               选项 A               │            选项 B            │
  ├─────────────────────┼────────────────────────────────────┼──────────────────────────────┤
  │ 资源入口            │ runtime 暴露 device 或提供资源 API │ runtime 只管提交，资源归别处 │
  ├─────────────────────┼────────────────────────────────────┼──────────────────────────────┤
  │ 全局 flight frame   │ runtime 持有全局 slot              │ 完全留给上层                 │
  ├─────────────────────┼────────────────────────────────────┼──────────────────────────────┤
  │ WaitIdle            │ runtime 提供                       │ 调用方自行追踪               │
  ├─────────────────────┼────────────────────────────────────┼──────────────────────────────┤
  │ Command buffer 池化 │ runtime 内部池化                   │ 上层自己管                   │
  └─────────────────────┴────────────────────────────────────┴──────────────────────────────┘

  这四个问题的答案会决定 runtime 的实际边界在哪里。


  再深度思考一遍，提出所有可能存在的问题

  深度审查完毕。按严重程度排列：

  ---
  A. 正确性级别的设计缺陷

  A1. Flight slot index 和 back buffer index 混淆——会导致 retire 状态错乱

  当前实现中存在两个不同索引被混用：

  - 等待退休时用 surface._frameNumber % flightFrameCount （flight slot）
  - 记录退休时用 swapchain.GetCurrentBackBufferIndex() （back buffer index）

  BackBufferCount=3, FlightFrameCount=2, _slotRetireStates.size()=2

  Frame 0: wait slot 0 (empty ok), acquire bb=0, record retire to [0] ✓
  Frame 1: wait slot 1 (empty ok), acquire bb=1, record retire to [1] ✓
  Frame 2: wait slot 0 (has frame 0), acquire bb=2, record retire to [2] → 越界，静默跳过
  Frame 3: wait slot 1 (has frame 1), acquire bb=0, record retire to [0]
  Frame 4: wait slot 0 (还是 frame 0 的！frame 2 的 retire 丢了), acquire bb=1

  _slotRetireStates 大小是 FlightFrameCount，但写入索引是 backBufferIndex，这两个值域不同。当 backBufferIndex >= FlightFrameCount 时退休状态不记录，意味着
   runtime 丢失了对该 GPU 工作的完成追踪。

  根因：GetFrameSlotIndex() 到底代表什么——flight frame slot 还是 back buffer index——设计文档没有精确定义。这两个概念在 BackBufferCount != FlightFrameCount
   时必然分裂。

  A2. Fence::Wait() 没有 targetValue 参数——等待语义不精确

  设计文档 §14.1 明确要求 Fence::Wait(uint64_t targetValue)，但 render 层当前只有 void Wait()。当前实现：

  if (point.Fence->GetCompletedValue() < point.TargetValue) {
      point.Fence->Wait();  // 等到"最新"完成——不是等到 targetValue
  }

  共享 fence 模型下，queue 上先后提交了 A(value=3)、B(value=4)、C(value=5)。调用 Task A 的 Wait()，实际等到的是 C 完成。这是 over-wait，不违反正确性但违反
   §4.4 的 "精确完成点" 语义，且在高负载下可能造成不必要的 CPU 停顿。

  IsCompleted() 用 GetCompletedValue() 对比是精确的，但 Wait() 不精确——两个 API 的语义精度不一致。

  A3. 同一帧内对同一 surface 重复 AcquireSurface 无保护

  auto r1 = frame->AcquireSurface(surfaceA);
  auto r2 = frame->AcquireSurface(surfaceA); // 无拒绝

  会对同一个 swap chain 调两次 AcquireNext()，创建两个 lease，_frameNumber 递增两次。flight slot 追踪完全错乱。设计文档 §8.4 说 "GpuSurfaceLease
  不能跨帧保存"，但没说同帧内同一 surface 能不能被 acquire 两次。

  A4. 销毁顺序未定义——Surface 可能在 Runtime 之后析构

  GpuPresentSurface 的析构调 Destroy() → _swapchain->Destroy()。如果 GpuRuntime::Destroy() 先执行了 _device->Destroy()，之后 surface 析构时 swapchain 的
  backend 资源（D3D12 的 IDXGISwapChain、Vulkan 的 VkSwapchainKHR）在 device 已销毁后被释放。

  D3D12 可能只是泄漏，Vulkan 则是 UB（device 已 destroy 后再 destroy swapchain）。

  runtime 不追踪它创建的 surface，所以无法在 Destroy() 中主动销毁它们。这是一个结构性的生命周期缺陷。

  ---
  B. 同步模型缺陷

  B1. Swap chain sync object 在 plan 路径下绑定位置硬编码

  if (!waitToExecute.empty() && i == 0) {        // 第一个 step
      desc.WaitToExecute = waitToExecute;
  }
  if (!readyToPresent.empty() && i == steps.size() - 1) { // 最后一个 step
      desc.ReadyToPresent = readyToPresent;
  }

  如果 step 0 是 compute pass 而非渲染 back buffer 的 graphics pass，GPU 会在不需要时等待 swap chain 的 acquire 信号。如果最后一个 step 是 copy 而不是渲染
   back buffer 的 pass，ReadyToPresent 信号的时机也错了。

  Render graph compiler 知道哪个 step 实际使用了 back buffer，但当前 API 没有途径传达这个信息。GpuQueueSubmitStep 缺少 swap chain sync 绑定的字段。

  B2. 多 surface 的所有 sync object 被打包到同一批——不同 surface 的 pacing 互相干扰

  // CollectSwapChainSync: 所有 lease 的 WaitToDraw 汇聚到一个数组
  for (const auto& lease : surfaceLeases) {
      waitToExecute.push_back(lease->_waitToDraw);  // surface A + B + C 全部堆在一起
  }

  假设 surface A 的 acquire 立即完成但 surface B 的还没好——当前实现让所有 step 0 一起等所有 surface，导致 A 的渲染被 B 拖慢。设计文档 §13.4 明确禁止 "只
  signal 第一块 surface 的 fence，然后顺便更新所有 surface 状态"，但当前做的恰好是反向的——所有 surface 的等待被合并。

  B3. WaitFor 依赖在 plan 路径下只挂到 "无前置 step 的入口节点"——假设所有外部依赖通过入口节点传递

  if (step.WaitStepIndices.empty()) {
      CollectDependencyWaits(dependencies, waitFences, waitValues);
  }

  如果 plan 有两个独立入口 (step 0 和 step 2 都没有
  WaitStepIndices)，两个入口都会挂上全部外部依赖——即使某个外部依赖只跟其中一个入口相关。这是保守但不精确的。设计上没有途径让 render graph 指定
  "这个外部依赖只关联特定 step"。

  ---
  C. 语义/契约缺陷

  C1. 无效 GpuTask 的 IsCompleted() 返回 false——语义歧义

  GpuTask task{};             // 或者空帧 Submit 返回的
  task.IsCompleted();         // → false

  一个从未提交的 task / 空帧 task 返回 "未完成" 是反直觉的。调用方不能用 IsCompleted() 统一判断 "这个工作做完了没有"——需要先检查 IsValid()。设计文档 §13.6
   说可以返回无效 task，但没定义无效 task 的完成语义。

  C2. IsEmpty() 和 Submit "是否有工作" 的判定标准不一致

  // IsEmpty() 把 surfaceLeases 和 dependencies 算进去
  return _defaultGraphicsCmds.empty() && ... && _surfaceLeases.empty() && _dependencies.empty();

  // Submit 只看命令
  bool hasCommands = !frame->_defaultGraphicsCmds.empty() || ... || !frame->_submissionPlan.empty();

  一个 acquire 了 surface 但没录任何命令的 frame：IsEmpty() == false，但 Submit() 返回无效 task。调用方如果用 IsEmpty()
  来决定要不要提交，会提交一个什么都不做的帧。

  C3. BeginFrame() 不限制同时活跃的帧数——多 frame 并发无约束

  auto frame1 = runtime->BeginFrame();
  auto frame2 = runtime->BeginFrame();
  frame1->AcquireSurface(surfaceA);
  frame2->AcquireSurface(surfaceA); // 同一 surface 被两个活跃帧 acquire

  没有任何机制阻止。两个帧分别递增 _frameNumber，分别在 submit 时信号同一 fence 的不同 value。如果 submit 顺序和 acquire 顺序不一致，flight slot
  退休追踪会错乱。

  C4. Acquire 了 surface 但不 Submit 的帧——swap chain 状态泄漏

  Acquire 调了 swapchain.AcquireNext()，swap chain 内部状态已经推进。如果帧不 submit（比如渲染判定后决定跳过），image 被 acquire 了但没被 present 也没被
  release。在 Vulkan 下这是对 VkImage 的使用约束违规。

  设计文档 §13.6 说 "未提交直接析构是合法且推荐的快速路径"——但只针对没有 acquire 任何 surface 的空帧。一旦 acquire 了 surface，"直接丢弃" 的语义就不对了。

  ---
  D. 缺失的能力

  D1. 没有 WaitIdle / DrainAllTasks

  resize、热重载、场景切换前需要等待所有 GPU 工作完成但不销毁 runtime。目前只能：
  - 自己追踪每个 GpuTask 逐个 Wait（侵入业务代码）
  - 或调 Destroy() 再重建（代价太大）

  D2. 没有全局 flight frame 概念

  per-frame 资源循环（uniform ring buffer、descriptor set pool、upload staging buffer）需要全局的 "当前帧在第几个 flight slot" 概念。当前 flight frame
  只存在于 per-surface 级别。纯 compute 工作负载完全没有 flight frame 信息。

  D3. 没有 Device 暴露或资源创建入口

  上一轮讨论过的。外部系统（ImGui、资源上传）无法通过 runtime 创建 GPU 资源。

  D4. 没有 command buffer 池化 / 回收机制

  Create*CommandBuffer() 每次新分配。GPU 工作完成后，InFlightTask 被移除，command buffer 随之析构。每帧分配+销毁。设计上没有回收路径。

  D5. 没有 queue 能力查询

  runtime 不暴露 "compute queue 是否可用"、"copy queue 是否可用" 的查询。调用方在 CreateComputeCommandBuffer() 返回 null 时才发现，没法提前适配。

  ---
  E. 线程安全

  E1. 线程模型完全未定义

  - GpuRuntime 的所有方法能不能并发调用？
  - GpuFrameContext 能不能在 thread A 录命令、thread B 调 Submit？
  - GpuTask::IsCompleted() 能不能在任意线程调用？
  - ProcessTasks() 和 Submit() 能不能并发？

  对一个 "执行基础设施层"，线程安全契约必须是设计文档的一部分。

  ---
  F. 头文件级别的结构问题

  F1. GpuCompletionPoint / GpuCompletionState 暴露在公开头文件

  设计文档明确标注 "runtime 内部抽象"。暴露意味着上层代码可以直接 include 并依赖它们，未来重构的自由度被锁死。

  F2. 资源持有类型缺 non-copyable / non-movable

  GpuPresentSurface、GpuFrameContext、GpuAsyncContext、GpuRuntime 全部缺少显式 = delete。GpuPresentSurface 因为 unique_ptr
  成员隐式不可拷贝，但可移动——移动后源对象析构会 double-Destroy。

  F3. 实现细节的 7 个 private 方法不应在公开头文件

  上一轮提过的。

  ---
  总结：哪些必须在继续演进前解决

  ┌──────┬────────────────────────────────────────┬─────────────────────────────────┐
  │ 编号 │                  问题                  │              影响               │
  ├──────┼────────────────────────────────────────┼─────────────────────────────────┤
  │ A1   │ flight slot / back buffer index 混淆   │ 运行时正确性——retire 状态丢失   │
  ├──────┼────────────────────────────────────────┼─────────────────────────────────┤
  │ A3   │ 同 surface 重复 acquire 无保护         │ 运行时正确性——状态错乱          │
  ├──────┼────────────────────────────────────────┼─────────────────────────────────┤
  │ A4   │ surface / runtime 销毁顺序             │ 潜在 crash 或 UB                │
  ├──────┼────────────────────────────────────────┼─────────────────────────────────┤
  │ C4   │ acquire 后不 submit 的 swap chain 泄漏 │ Vulkan 下违反规范               │
  ├──────┼────────────────────────────────────────┼─────────────────────────────────┤
  │ B1   │ swap chain sync 绑定硬编码             │ 多队列 plan 下 present 时机错误 │
  ├──────┼────────────────────────────────────────┼─────────────────────────────────┤
  │ C2   │ IsEmpty / Submit 判定不一致            │ 调用方误判空帧                  │
  └──────┴────────────────────────────────────────┴─────────────────────────────────┘

  其余问题（A2、B2、B3、C1、C3、D1-D5、E1、F1-F3）属于 "应该尽快解决但不阻塞当前阶段" 的范畴。