> - 适用: 2026-10-16 确认的 Render Framework 所有权、帧编译和 descriptor 复用重构
> - 权威: 经用户授权保存的实施输入与删除前快照；不是当前接口契约
> - 锚点: `modules/render/`, `docs/history/2026-10-21-runtime-render-framework-snapshot.md`, `docs/history/2026-10-21-runtime-archive.md`
归档维护注（2026-10-21）：本记录的原基线、结果及当时语义不变；下列源码锚点已随 runtime 移除，按原基线或归档说明查阅：`modules/runtime/`, `examples/`。
正文中指向 runtime 的“当前”说明现仅为历史快照，不代表主线能力。


# Render Framework 重构输入与基线

状态：已执行。重构前后各 42 个双后端场景通过读回；最终生命周期、回归、性能结果与限制见[实施验收](2026-10-16-render-framework-refactor-validation.md)。下文保留重构前语义。
基线提交：`f20c56fbf073154185ae792daac71982e5f6ab43`，包含执行开始时的未提交 sampler 修改。
当前契约见 [Runtime 宿主](2026-10-21-runtime-render-framework-snapshot.md) 和 [帧与 GPU](2026-10-21-runtime-frame-and-gpu-snapshot.md)。
本记录由用户明确授权保存，后续不把未运行项目写成通过。

## 执行起点

开始实施前的六个修改文件：

- `docs/history/2026-10-21-runtime-render-framework-snapshot.md`
- `modules/runtime/include/radray/runtime/render_framework/material_gpu_state.h`
- `modules/runtime/include/radray/runtime/render_system.h`
- `modules/runtime/src/render_framework/raster_draw.cpp`
- `modules/runtime/src/render_system.cpp`
- `modules/runtime/tests/test_raster_draw.cpp`

原始补丁保存在 [baseline.patch](2026-10-16-render-framework-refactor-baseline.patch)，
执行起点的源码 SHA-256 与补丁校验值保存在 [source.json](2026-10-16-render-framework-refactor-source.json)。
这份基线不能用纯净 HEAD 替代。查看旧源码使用 `git show <基线>:<旧路径>`，再应用保存的补丁。

## 重构前职责、所有权与限制

`RenderSystem` 拥有 shader program、PSO 缓存和持久常量 arena；共享 service generation 令牌用于判断服务是否仍有效。
`MaterialAsset` 拥有 CPU 材质数据、纹理资产依赖以及可变 `MaterialGpuState`。
每个 `MaterialPass` 通过共享 definition 指针保留定义，在 definition 上惰性缓存共享的 native plan。
`MaterialGpuState` 按 pass 指针拥有持久参数集，并通过共享 page/block 保持常量分配存活。
sampler 修改通过 RenderSystem 的弱引用缓存共享完整 sampler-only 参数组，material 保留强引用。

`RasterDraw::PrepareMaterial` 同时进行值解析、常量上传、参数集查找、创建和写入。
静态材质参数集按材质和 pass definition 分配；等价但独立构造的 pass 不共享持久存储。
动态参数集归 `GpuFrameResources` 的 flight/layout/group 池拥有。局部 `RasterFrameBindingCache`
以 native plan 指针区分布局，并在 GPU 切片分配后比较 buffer binding。
`SceneDraw::Prepare` 为每次 view/pass 调用分别准备 View 常量和可见列表，局部缓存不跨整个 frame。
因此业务身份、局部准备边界和物理 flight 可以放大本来相同的完整参数组；仅修 sampler 不改变这些边界。

`SceneManager` 拥有 RT scene 和各 flight 的 scene GPU 镜像，GT `SceneWriter` 发布更新。
材质列表、实例 schema 与实例参数 storage 通过共享指针跨交付包保留。
mesh/material 描述中的原生指针由 GT 资产 use/retirement 协议保活。CPU scene 同步可独立于 GPU 使用。

GT 封存并发布，RT 接纳、准备和录制，GpuSystem 提交 queue 工作；FrameTimeline 只在真实完成后发布 completion。
GT 消费完成消息，释放 scene 资产退休项及 frame payload。FrameSerial 表示身份，不能替代 queue fence。
command epoch 结束和 GPU completion 分属不同阶段。独立宿主可以共享 Device，但完成证明不共享。

旧源码入口：`render_framework/material.{h,cpp}`、`material_gpu_state.{h,cpp}`、`raster_draw.{h,cpp}`、
`scene_draw.{h,cpp}`、`scene_writer.{h,cpp}`、`instance_parameters.{h,cpp}`、`scene_manager.{h,cpp}`、
`render_system.{h,cpp}`、`gpu_frame_resources.{h,cpp}`、`gpu_resource.{h,cpp}`、`application.{h,cpp}`。
头文件位于 `modules/runtime/include/radray/runtime/`，实现位于 `modules/runtime/src/`。

## 确认后的边界

- 整个 render_framework 不再使用 shared_ptr、weak_ptr、enable_shared_from_this 或侵入式所有权替代品。
- 直接迁移仓库调用方与 shader，最终删除旧接口和兼容执行路径。
- 保留资产系统、共享 Device 边界、GpuSystem/FrameTimeline 的真实提交完成协议。
- 严格分离绑定域，复用兼容的完整参数组；不承诺不同完整组之间局部 D3D12 table 共享。
- 先保存历史并运行基线，验证 GPU 读回成功之后才能删除旧框架。

## 已取得的重构前基线

在 NVIDIA GeForce RTX 3060、Windows、MSVC 14.51.36231、Debug 下串行运行 42 个独立进程，
D3D12/Vulkan 各 21 个，全部完成预热、30 帧稳态、真实完成后读回和最终零占用检查。
原始结果及完整命令、设备、源码/二进制校验保存在 [baseline.json](2026-10-16-render-framework-refactor-baseline.json)，
逐检查点统计见 [baseline.csv](2026-10-16-render-framework-refactor-baseline.csv)。
本地可重跑包为 `build_descriptor_reuse/baseline-validated/`，含二进制、编译器 DLL、shader、artifact、源码 ZIP、配置和日志；
该目录不入库，校验清单与原始 JSON/CSV 已入库。包缺失时不能宣称仍可直接重跑旧二进制。

```powershell
python tools/run_descriptor_reuse.py --exe build_debug/_build/Debug/example_descriptor_reuse.exe --output build_descriptor_reuse/baseline-validated
```

默认 128 对象、32 材质、3 视图、3 flight 的 D3D12 resource/sampler 实测：
共享 pass 为 100/2，独立等价定义为 1216/2，Color+Depth 为 136/2，兼容双 Color 为 200/4，
同值 override 为 612/2。共享定义仍有 6 份完整组重复，独立定义有 564 份；双 Color 有 49 份。
Color+Depth 和双 Color 每帧各有 6 个额外语义数据切片（view 常量和可见列表），
可见顺序不同的控制场景只有 3 个额外 view 常量切片，未把必要的可见列表分离算作重复。
这些是原生分配/调用统计，不是帧率结论，Vulkan 的 descriptor 类型统计单独保存在 JSON。

正式包修正了样例自身的 artifact bytes 保活，再次运行全部 42 项；各检查点分配统计与初次扫描完全一致。
早期 `smoke-*` 和 `baseline/` 包只留作本地诊断，不作为验收输入。
正式采样时 RHI 已增加尚未由旧框架调用的兼容性查询，Vulkan sampler 保存了可查询的原生状态，
BindingRegistry 新文件尚未接入执行；实际路径仍是原始 MaterialGpuState/Prepare，heap 容量未变。
实际采样源码以正式包 manifest 校验为准，起点补丁另行保留，二者不混同。

冻结口径：`examples/descriptor_reuse/workload.{h,cpp}`、`descriptor_probe.{h,cpp}`、shader 和场景矩阵保持语义，
只迁移 `framework_bridge.{h,cpp}`。图元由程序生成的三角形顶点序列构成，各对象具有独立 scissor、矩阵和实例 tint。
每个 frame 读回最终 view、最终 Color pass 的各对象中心；Color+Depth 同时读取深度。
材质数超过请求对象数时提高实际对象数，JSON 同时保留请求值和实际值，保证全部材质实际绘制。
`mixed` 独立记录 ABI，不与 shader 不变的场景混算。probe 只计自己包装的原生参数集，
未初始化/已 Reset 的池条目计入存活占用但不宣称它们具有完整等价绑定值。

## 目标架构

### 实施中检查点：新 Renderer 垂直路径

首个垂直路径已在两个后端运行全部 42 项，原始记录见 [vertical.json](2026-10-16-render-framework-refactor-vertical.json)
和 [vertical.csv](2026-10-16-render-framework-refactor-vertical.csv)，本地冻结包为 `build_descriptor_reuse/renderer-vertical-01/`。
这次检查仅覆盖新 Renderer 的材质登记、整帧绑定编译和离屏绘制；Application、Scene GPU 镜像及旧调用方尚未全部迁移，不能据此宣布重构完成。

默认共享定义、独立等价定义、Color+Depth、兼容双 Color、同值 override 均为 72 resource / 2 sampler；
F=1/2/3 结果相同，真实变化的 override 为 74/2。300 材质的共享/独立定义均为 608/2。
全部检查点的完整组冗余为零，同语义数据切片重复为零，最后的清理占用为零。
所有场景预热后的参数集 Create/Set/Flush 增量为零；这不代表没有 CPU 编译成本，也不能换算成帧率。

样例只替换 framework bridge；workload 的唯一调整是 `mixed` 的 ABI 标签明确改成
`grouped-v1-migrated-from-mixed-v1`。该场景编译已有 shader 的分组分支，单独配对比较；其余 shader、
工作负载、参数序列、probe、读回和统计字段保持原有语义。正式终验仍须在全部迁移完成后的源码上重新运行。

Application 独占新的 Renderer；独立宿主也可装配。Renderer 借用 GpuSystem 和 shader/PSO 服务，
独占模板、材质版本、导入资源、BindingRegistry、scene GPU 镜像及框架帧存储。
TemplateId、MaterialId、ResourceId、DescriptorBlockId 都是类型化非拥有句柄，验证 renderer、slot 和 generation。
复制句柄不增加引用计数；替换创建新版本，已有使用结束后回收旧版本。
Material 资产仅保存 CPU 描述及资产依赖，不保存 GPU owner、pass native plan 或 sampler 专用缓存。
持久常量由 Renderer 独占 arena 分配，以 allocation ID 显式归还。

场景包独占数据：材质列表和实例 schema 使用登记 ID，必要定义与更新随包交付；实例 bytes 是包拥有的连续存储，
更新记录 offset/size。GT 封存后不修改，RT 保存自己的表示，不跨帧借用旧包。CPU-only 场景仍然可用。

```text
应用有序更新 → 收集全部 view/pass/draw/output → 准备唯一数据与可见性
           → 编译绑定和资源依赖 → 录制 → 提交 → 按真实完成票据回收
```

同一 frame 的多个 pipeline/output 先汇集到一个 FrameBuilder。录制只消费编译结果，不分配参数集、
不上传常量、不执行内容去重。框架使用的 buffer 由 Renderer 保持到所有缓存和 GPU 使用结束。

模板根据输入来源验证 Scene/Frame、View/Pass、Material、Sampler 四域。
实际 group 和布局来自 shader artifact，拒绝跨域混合组并提供诊断；不自行重建平行 binding schema。
BindingRegistry 用后端兼容组布局与规范化最终值查找，hash 定位后完整判等。
业务 Material/Pass/View/Flight 身份不进入 descriptor key。
buffer 资源版本、base offset、range、stride、数组元素、view 和 sampler 状态参与身份；dynamic offset 留在绑定命令。
先对 CPU 常量内容去重再分配 GPU 切片，同值 override 复用默认值。
原生参数集写完封存，外部只获得 ID，BindingHandle 保持 layout 专属。

闲置 LRU 默认预算为 4096 resource descriptor 和 128 sampler descriptor；Vulkan 按类型数量计费。
活跃项不淘汰，分配压力先清闲置再重试，提供显式生产清理入口。资源退出时清理相关闲置绑定，缓存不延长外部资产驻留。

Renderer 登记表在 owner 安全点修改，GT 更新和完成消息有序交接。
每个已接纳帧保存去重使用清单，回收同时满足：没有持久登记依赖、没有接纳未提交使用、所有已提交票据真实完成。
录制完成、Present 和 FrameSerial 都不是完成证明。取消必须结束 command epoch 后撤销保护，部分失败保留必要工作。
导入资源退出遵循请求退出、Renderer 安全清理、确认、GT 释放 owner。
关停停止生产、排空 GPU 和完成消息，然后清 Renderer、资产、shader 服务和 Device。

## 基线样例与冻结契约

新增 example_descriptor_reuse，程序生成 mesh/纹理/材质，256×256 离屏目标，无外部资产依赖。
默认 128 对象、32 材质、3 视图、3 flight；每 case 预热 2F 帧，再运行 30 帧。
命令行选择 D3D12/Vulkan、场景、材质/视图/flight 数和 JSON 输出；脚本串行启动独立进程并汇总 CSV。
冻结场景、参数变化序列、JSON 字段和 GPU 读回规则，重构后只替换框架接入。
基线包必须保留 shader/artifact、源码和二进制校验、命令、构建配置及设备信息，防止旧程序读新 shader。

样例自己的 Device/ParameterSet probe 记录统计，不在生产代码中添加测试字段。

| 记录 | 口径 |
|---|---|
| D3D12 | 实际 table 分配 resource/sampler 槽位，包含空洞；当前/峰值/累计分配/释放 |
| Vulkan | 实际 set 数及各 descriptor 类型数量；不声称驱动内部字节数 |
| 参数准备 | 创建、Set、Flush 次数，分别记录初始化和稳态 |
| 原生等价冗余 | 兼容完整组且最终绑定值相同的多份存储与槽位成本 |
| 语义数据重复 | 同一视图数据的不同 buffer 切片，与原生 descriptor 等价分开报告 |
| 正确性和回收 | 读回、分配失败、负载下降、drain、注销、最终清理 |

场景覆盖共享/独立等价 pass；分别扫描 M=1/32/300、V=1/3/6、F=1/2/3；
Color、Color+Depth、绑定兼容而 PSO 不同的双 Color；默认/同值/变化 override；
同一视图跨 pass 复用和不同可见排列控制；视图负载升高再下降，第1/F/61F帧及 drain 检查；
旧 mixed-group 与新分组 shader 单独配对，明确 ABI 迁移，不混入 shader 不变的比较。

## 实施顺序与验收门槛

1. 保存本记录、起点补丁和源码校验；实现样例、双后端运行、确认读回并保存完整基线包。
2. 实现登记表、独占帧存储、退出/提交/完成闭环，再接入多视图 Color/Depth 垂直路径。
3. 迁移场景包、自动/手动实例、custom producer、examples、benchmarks 和所有测试。
4. 删除 MaterialGpuState、逐 pass Prepare、共享 service generation 及仅服务旧路径的通用参数池。
5. 相同负载复测并执行受影响 runtime/render 回归、生命周期和无共享所有权扫描。
6. 更新当前架构、帧生命周期、shader、构建/测量文档，保存 validation 历史和原始数据。

硬验收：等价 pass 占用相同；view/flight 不复制静态 Material/Sampler；兼容 pass 共享相同完整组；
同视图相同参数跨 pass 共享切片，不同可见列表分离；同值 override 不增存储，真实变化仅影响对应域；
完整组等价冗余为零；负载回落满足闲置预算；注销、真实完成、显式清理后样例 descriptor 归零。
维持原 heap 容量，不用扩容掩盖问题。

双后端必须实际验证颜色、深度、纹理、sampler、实例参数及 override；缺失或 skip 不是通过。
生命周期包括接纳未提交即注销、延迟 fence、替换、slot 复用、Scene 销毁、输出丢弃、分配失败、
单/双线程和 F=1/2/3；非法 renderer/过期句柄/混合组被拒绝，CPU-only、无 Scene、服务重建及独立宿主继续可用。
构建与测试串行。计时由现有 Google Benchmark 提供，分配样例不推导 FPS。
最终执行 `python tools/check_docs.py` 和 `git diff --check`。只有实测基线、复测和生命周期验收通过才标完成。
