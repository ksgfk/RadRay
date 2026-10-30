> - 适用: 使用或修改场景、Mesh Drawing Pipeline、可编程管线、RDG 和帧资源
> - 权威: 本文维护 runtime 的接口边界与所有权；底层资源契约见 RHI 文档
> - 锚点: `modules/runtime/include/radray/runtime/`, `modules/runtime/src/`, `modules/runtime/tests/`, `modules/runtime/CMakeLists.txt`

# Rendering runtime

`RadRay::runtime` 是 C++20 静态库，实际 target 为 `radrayruntime`，依赖 `RadRay::render`。
公共头使用 `<radray/runtime/...>`，所有 runtime 类型直接位于 `namespace radray`；RHI 类型仍位于 `radray::render`。
容器使用 `radray/types.h` 的无前缀别名。模块不引用宿主 bridge、Minecraft program 或顶点语义。
启用开关、安装消费与测试命令见[构建与测试](../guide/build-test.md#runtime-测试)。

runtime 接口不传递错误字符串。失败处直接使用 `RADRAY_ERR_LOG` 输出诊断，调用方通过 `bool` 或 `Nullable` 判断结果；
应用实现的 `MeshPassProcessor::Validate`、`IRenderPipeline::BuildGraph` 与 pass 回调也遵循此约定。

分层来自 Raynium 中参考 UE 5.8.4 实现的最小渲染框架：Scene → MeshBatch/MeshPassProcessor → PreparedMeshPass，
应用的 IRenderPipeline → GraphBuilder → GraphExecution → RHI。它不是 UE 源码或完整 Renderer 的移植。
runtime 不包含应用 shader；shader 字节码、布局、固定状态、纹理绑定与参数内容由应用提供。
没有 GPUScene、GPU 剔除、并行录制、异步计算、多队列调度、图内内存别名或运行时 shader 编译。

## 环境与设备

`Environment::Create` 建立 DXGI factory 或 Vulkan instance；应用可传入日志使用的 appName。
`DeviceContext::Create` 借用环境并建立 Device 与一条 Direct 队列，支持外部资源扩展及宿主设备身份匹配。
可失败工厂返回 `Nullable<unique_ptr<T>>`；准备完资源后才交付有效对象。
Environment 必须晚于 DeviceContext 和所有资源销毁，设备析构等待队列。生命周期调用由环境的创建线程串行执行。
Vulkan instance 与 D3D12 factory 的底层单例约束仍由 RHI 管理。
`SupportsIndirect` 使用 RHI `DeviceFeatures::IndirectDraw` 和 `IndirectFirstInstance`，不依赖后端内部头。
后端可以通过 `RADRAY_ENABLE_D3D12` / `RADRAY_ENABLE_VULKAN` 编译开关禁用。

## 场景与 Mesh Drawing Pipeline

| 接口 | 当前职责 |
| --- | --- |
| `SceneUpdate` | epoch、普通索引、网格布局/范围、材质描述、primitive 引用及移除项 |
| `ViewSubmission` | 当帧可见 submesh、排序键、实例区间及不透明实例字节流/stride |
| `MeshPassProcessor` | 验证 mesh/material 兼容性，产生值类型稳定命令；身份与 Version 参与缓存失效 |
| `MeshDrawCommand` | 网格、材质、布局、program key、版本、绘制范围与逻辑资源/参数；不含图句柄或帧指针 |
| `PreparedMeshPass` | 保活可见缓存、解析几何位置、统一 packets、实例/indirect 数据和当前绑定 |
| `IRenderPipeline` | 注入 Processor，并用 BuildGraph 声明本帧 pass 与资源 |

`Scene::Prepare` 检查索引/顶点界限、引用、epoch、实例区间，在 Update 中暂存变化；`Commit` 使用预备节点发布。
Scene 对象由调用方持有，与 Renderer 的帧对象分开；当前驻留几何属于对应 Renderer，不支持在不同 renderer 间无上传迁移场景。
epoch 非零且不得倒退。切换 epoch 清空旧场景；设备保持。更新失败、图编译失败或录制失败都不发布增量。

按 primitive 缓存命令。mesh/material/index 版本、相关绑定资源版本、processor 身份或版本改变时失效。
材质参数是不透明字节，资源为逻辑索引；纹理内容变化不等于对象身份变化。可见范围、排序键与实例内容只更新帧数据。
应用的 processor 持有 shader/布局/固定状态和绑定设施，只提供命令及绑定描述，不录制 GPU 命令。

绘制按 depth key、program/material、页、mesh、实例源和范围稳定排序。
相同几何、版本、布局、绑定和状态的连续索引范围可合并；几何/状态/范围一致且实例流连续才合并实例。
direct 与 CPU indirect 消费同一组 DrawPacket，能力不足时使用 direct；multi-draw 数量分批由 RadRay 完成。

`GraphicsPipelineCache` 深拷贝 key 中的入口名、顶点语义和描述值，比较 shader 对象、pipeline layout、顶点布局、固定状态、颜色/深度/采样 attachment signature。
尺寸不在 key 中。shader/layout owner 必须比缓存活得久。

## 可编程管线与 RDG

`Renderer::Create` 接受一个 `IRenderPipeline` owner。每帧先准备场景/几何和 SceneUpload，再调用 BuildGraph，编译、准备资源、录制并提交。
BuildGraph 接收 FrameContext、PreparedMeshPass 和本次调用借用的参数；回调捕获值由图持有，GPU 对象由帧和资源 owner 保活。
替换测试管线添加中间纹理、采样 pass 或 compute 不需要修改执行器，实例见 `modules/runtime/tests/test_runtime_gpu.cpp`。

GraphBuilder 提供 CreateTexture/CreateBuffer、ImportTexture/ImportBuffer、AddRasterPass/AddComputePass/AddCopyPass、AddMeshPass、MarkOutput、Extract 与 Compile。
句柄含共享身份 token 和资源索引；token 由图和句柄共同持有，不依赖全局计数器，解析时检查所属图、类型和 pass 声明；同一底层对象导入去重并保留各次传入的 owner，矛盾的初始/最终状态和交接声明拒绝编译。
构建时发现的空导入、冲突导入和无效输出句柄立即记录错误，GraphBuilder 保留失败状态，后续 Compile 返回 false。
FrameContext 的 Upload 管理当槽的命名上传缓冲，内容按调用方声明布局写入。

绑定描述 `MeshBinding.resources` 与顶点、索引、实例、indirect 一起由 PreparedMeshPass 汇集。
AddMeshPass 自动导入/引用这些资源、合并访问并创建 raster 回调，调用方不用重复在 raster 访问列表里抄写每条 draw 的依赖。
PassContext 通过 `Nullable` 返回已声明 Buffer/Texture、创建兼容 TextureView，并取得本 pass 的 raster/compute encoder 或 copy 命令接口；pass 开闭、屏障和提交由 runtime 管理。
未声明资源或错误 encoder 的解析返回空值并使本次 Record 失败；回调返回 true 不能掩盖该错误。
回调不得自行改变资源状态或 pass 边界。可借用 import 指针，但其外部 owner 必须保持到 GPU 完成；传入 ImportOptions.owner 时由图自动保活。

图按整资源追踪，读取依赖此前最近有效写入；写访问建立 WAW 和 WAR，读访问建立 RAW。
写声明表示初始化整资源；部分写且需保留旧内容时使用读写访问或 attachment Load。未初始化读、跨图句柄、不兼容 usage/stage/pass、冲突访问、attachment 尺寸/格式/采样数与 load/store 都在 Compile 校验。
Discard 使内容失效，后续读取和输出会被拒绝。当前 attachment 是完整单层 2D，不提供子资源 attachment、resolve 或 stencil 操作配置。

最终输出、extraction、导入资源写入和显式 sideEffect 是保留根，反向保留依赖。
执行保持声明顺序，不做覆盖写消除、激进重排或 pass 合并。无关 pass 和未使用临时资源不会分配/执行。
CompiledGraph 包含保留 pass、依赖、资源使用区间、前置屏障、epilogue 和最终状态；Dump 与 Renderer.LastGraph 提供诊断文本。进程环境变量 `RADRAY_DUMP_GRAPH` 非空时，每帧也将图诊断写入日志。

用途映射到 RadRay buffer/texture 状态及 shader stage。连续 UAV 访问只要有写依赖就生成 UAV 屏障，即使前后状态相同。
导入的 acquire/release 在首用和图末处理；即使没有存活 pass，也保留必须的边界交接。
timeline waits/signals 由 Renderer 提交层处理，不放入 pass 回调。

ResourceStore 只分配存活临时资源，不做同图内存别名。按完整 buffer/texture 描述符匹配池中对象，记录最终状态与 completion，所有 owner 释放且 GPU 完成后才复用。
ExtractedResource 持有分配/导入 owner、最终状态及共享 completion fence/value；CPU 使用先等完成，跨帧导入使用返回的最终状态。
设备和环境仍须活到所有 extracted owner 释放之后。

## 上传、帧槽与资源退役

顶点驻留约 4 MiB 的 device-local 页，超大 mesh 扩大单页；索引按普通 ID 存储。
GeometryStore 的 SceneUpload 声明 staging 读取和几何写入，复制屏障由 RDG 生成；回调只复制内容。
索引内容由应用生成，runtime 不解释应用顶点或面组语义。

两个 FrameContext 各自持有 allocator、command buffer、参数/实例/indirect/staging、图执行对象与保活引用。
复用前等待本槽 completion，默认 5 秒超时；超时保持 owner。场景替换只移除当前引用，旧分配由未完成帧保活，完成后返还预备的空闲节点，析构不分配内存。
无引用空页在后续准备时释放。资源池与几何驻留分别管理，不做 GPUScene 或 GPU 剔除。

所有可能分配的提交账本在 queue.Submit 前准备；成功提交后才更新 completion、场景、几何和外部序号。
编译/资源准备/录制失败不提交；录制失败关闭并 reset 未提交命令。RHI 自身不可恢复的终止不属于可恢复失败。
Finish 等待 renderer completion 再释放帧引用；等待超时返回失败并保留 owner。C++ 直接析构 renderer 也会在仍有未完成提交时等待队列。

profiling 开启且设备支持 timestamp 时，RDG 的 TimestampResolve 声明计时 readback 写入，记录整图 GPU 区间。
每 120 个样本输出 p50/p95/p99、几何页字节与命令构建累计数；不是像素读回，也不是固定场景性能基准。

