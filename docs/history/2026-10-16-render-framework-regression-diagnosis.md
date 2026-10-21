> - 适用: 2026-10-16 Render Framework 重构后的 CPU 时间回退定位
> - 权威: 经授权保存的测量与代码分析；未实施优化，不作为新的框架契约
> - 锚点: `docs/history/2026-10-21-runtime-archive.md`
归档维护注（2026-10-21）：本记录的原基线、结果及当时语义不变；下列源码锚点已随 runtime 移除，按原基线或归档说明查阅：`benchmarks/bench_material_scene/material_scene_workload.cpp`, `modules/runtime/src/render_framework/renderer.cpp`, `modules/runtime/src/render_framework/binding_registry.cpp`, `modules/runtime/src/render_framework/material_template.cpp`。
正文中指向 runtime 的“当前”说明现仅为历史快照，不代表主线能力。


# Render Framework CPU 回退定位

状态：定位完成，生产源码未修改。基线仍为 `f20c56fbf073154185ae792daac71982e5f6ab43` 加原 sampler 工作区补丁；
当前版本是[重构验收](2026-10-16-render-framework-refactor-validation.md)的最终源码。
核对该验收包全部源码 SHA-256，零差异。本文补充原因，不替换原始前后性能结果。

## 结论

当前实现复用了 GPU descriptor 存储，却没有充分复用 CPU 已编译的绑定结果。
`CompileDraws` 每帧按 draw 展开全部输入，创建 writes/dependencies/offsets 等临时容器，之后才合并完整组。
稳定数据的准备频率仍接近 draw 数，完整组去重发生在这些 CPU 工作之后。
Material/Sampler/View/Scene 的域信息目前主要用于模板合法性验证，没有用于按变化频率组织编译。

这解释了 descriptor 冗余为零而 CPU 时间仍增加：存储复用命中不会撤销命中前的输入解析、复制、分配和校验成本。
同时，帧保存完整请求与绘制对象图，GPU 完成后再逐项销毁这些容器，增加了 FullFrame 中的 CPU 回收成本。
显式所有权和真实完成协议可以保留；需要调整 CPU 编译和帧存储的粒度。

```mermaid
flowchart LR
    A[稳定材质版本和模板] --> B[每帧每 draw 展开输入]
    B --> C[构造临时 writes / dependencies]
    C --> D[整帧合并相同组请求]
    D --> E[布局和值规范化]
    E --> F[命中已有 descriptor]
```

## 测量方法与边界

使用同机 RTX 3060、Windows、MSVC Release、profiler OFF，与原报告相同的已构建 runtime/render 库。
只在 `build_descriptor_reuse/` 的 benchmark fixture 副本中用 Google Benchmark PauseTiming/ResumeTiming
选择计时阶段，没有给生产类添加字段、计数器或插桩，也没有修改生产实现。每帧其它阶段仍实际执行和提交。
LTCG 会重新链接诊断程序，因此这些数字用于定位，不替换原始未拆分 benchmark。

- 分段：N1000、M1/100、V3、dirty0、F3；before/after/after/before 四个独立进程，每 case 每轮 500 帧、3 次重复，最终每版本 6 个样本。
- 完成消息 CPU：在 `CompleteFlightIfReady` 返回后才恢复计时，覆盖 payload 释放、Renderer 完成清理和 Scene 完成通知，排除 GPU 等待。每次 1000 帧，同样四进程、6 样本。
- 绑定 key 微基准：真实已编译布局和 buffer，先暖 Resolve，再测同一个完整组的缓存命中；每次 100000 次，两个独立进程、每次 3 次重复。
- 规模控制：N1000、V3、dirty0、F3，M1/7/32/100/300，只计 Compile；两个独立进程，每 case 500 帧、3 次重复。实际 draws/frame 与 3×M 一致，稳态 host_bytes/frame 为零。

全部有效运行无 benchmark error。首轮 key 微基准错误地使用了空 source request，没有解析已有 ShaderProgramId；
修正诊断 fixture 后重跑，出错数据没有纳入有效样本。原始尝试保留在本机 `diagnosis-detail/runs/`，有效结果是 `runs-valid/`。
分段中位数来自独立 case，不能强行相加作为同一帧的严格分解。亚微秒项受计时开关和调度影响，只作辅助证据。

## 分段实测

下面是 M100 的 wall time 中位数，单位 µs。旧版 SceneDraw 同时完成绑定与 PSO 准备，新版该阶段主要收集请求，随后统一 Compile。

| 阶段 | D3D12 前 → 后 | Vulkan 前 → 后 |
|---|---:|---:|
| PrepareRecord 整段 | 705.55 → 912.74 | 726.33 → 894.86 |
| SceneDraw 准备/收集 | 639.47 → 507.86 | 625.80 → 523.84 |
| 新的统一 Compile | — → 348.43 | — → 339.58 |
| 原生录制及局部 prepared 数据销毁 | 72.76 → 50.29 | 85.71 → 43.90 |
| 完成消息 CPU，排除 fence 等待 | 1.01 → 68.64 | 0.96 → 62.69 |

录制时间下降；增长集中在新的 CPU 编译和完成清理。新的收集加编译超过旧版准备的成本，构成 PrepareRecord 的主要回退。
完成 CPU 的额外约 62–68 µs 进一步增加 FullFrame。没有把 GPU 等待误算为这项 CPU 清理。
M1 的完成 CPU 仅约 0.85→4.42 µs（D3D12）、0.80→3.90 µs（Vulkan），增长明显随请求量增加。

## 固定最终绑定值，扫描请求数量

本负载的 tint 使用 `i % 7`。M>=7 时，100 个或 300 个材质资产仍只有 **7 种 Material 最终值**。
从输入和去重代码推导，每帧需要的完整组是 7 个 Material、3 个 View 和 1 个 Scene，共 11 个；
这 11 是当前 workload 的逻辑推导，未冒充新增生产计数器的实测值。GPU 原生 descriptor 去重已有独立样例验收。

| 材质资产数 | 实际 draws/frame | Material 最终值种数 | D3D12 Compile µs | Vulkan Compile µs |
|---:|---:|---:|---:|---:|
| 1 | 3 | 1 | 18.96 | 14.14 |
| 7 | 21 | 7 | 52.90 | 38.20 |
| 32 | 96 | 7 | 131.82 | 118.51 |
| 100 | 300 | 7 | 351.70 | 343.02 |
| 300 | 900 | 7 | 1001.29 | 1005.18 |

在最终绑定值种数不增加时，编译耗时仍随 draw 请求数量近似线性增长。
M100 的 300 个 draw 各遍历 3 个组，即 900 次组展开；缓存最终只保留少量组，无法免掉前面的工作。
这比“材质太多导致 descriptor 分配”更准确地解释当前回退。

## 对应代码路径

1. **域信息没有转成准备频率。** `material_template.cpp` 校验并记录 Group.Domain；
   `renderer.cpp` 的 CompileDraws 对所有 draw、所有 group 逐项 dispatch 输入，调用 UseResource/Value，创建临时 vectors。
   groupRequests 的命中发生在 writes/dependencies 已构造之后。Material 记录保存常量/纹理/sampler ResourceId，
   没有保存可直接引用的已编译完整组结果。
2. **缓存查找重复处理稳定元数据。** `BindingRegistry::Resolve` 在查 `_lookup` 之前生成兼容身份、构造值数组、
   排序依赖、计算 hash。D3D12 的 GetParameterGroupCompatibility 每次构建并排序 table/entry vectors；
   Vulkan 也重新构造 entries。它们在布局存活期间本来是稳定事实。
3. **帧对象图包含大量独立分配。** 每个 Draw 自带几何布局、顶点描述、push bytes、Groups、Offsets、Vertices 等容器；
   Requests 另存 Writes/Dependencies。ProcessCompletions 清空 Draws/Requests/Views/Passes 与各 unordered_set，
   外层 vector 的容量保留不等于内层分配保留。之后 Collect 还遍历登记表。
   排除 fence 后的实测证明整个完成清理段明显增长；尚未单独量化各容器析构或 Collect 的时间占比。

单个 Material 组的暖缓存微基准：

| 操作 | D3D12 µs | Vulkan µs |
|---|---:|---:|
| 重新生成布局兼容身份 | 1.503 | 0.178 |
| 完整 Resolve 缓存命中 | 2.785 | 0.416 |

D3D12 Resolve 的 6 样本范围为 1.75–2.86 µs，CV 21.5%，不把中位数当作固定单次成本。
这项确有重复成本，但**不是整个回退的唯一主因**：M100 已在 Resolve 之前合并到少量组，不能按 100×或 900×Resolve 相乘。
Vulkan 的 key 微基准明显更便宜，整帧 Compile 仍约 340 µs，加上上面的规模控制实验，指向合并前的公共 CPU 请求处理。
没有做逐函数采样或分配事件追踪，因此未声称某个 hash、malloc 或 free 占了确定百分比。

## 后续修正方向，尚未实施

- 在 template/layout 注册时派生并保存兼容身份、输入槽位顺序等稳定事实，使用现有 resolved layout，不新增平行 schema。
- 在材质版本首次编译或变化时处理 Material/Sampler 完整组，持有非拥有 DescriptorBlockId；帧中验证 generation 并登记使用。
  闲置 LRU 淘汰后按需重新 Resolve，不能让这层缓存绕过预算或延长外部资源驻留。
- 每帧对唯一 Scene/View 数据版本准备一次绑定；draw 引用已准备的组 ID、几何 ID、offset 和 push 数据。
  将合并提前到参数展开之前，同时保留对兼容完整组及最终值的规范化判等。
- 将当帧请求、编译结果与 GPU 退休使用清单区分开，采用连续存储/范围引用和批量回收；保留真实完成与 command epoch 保护。
  使用清单按已验证的本 Renderer 槽位去重，避免在内部热循环重复建立通用 owner/generation hash 节点。

优先级是减少重复编译与临时对象周转，其后才是单个 hash/vector 的微优化。所有权方向和 GPU descriptor 复用规则不需要回退。

## 原始证据

- [全部原始 JSON、manifest、源码校验、诊断 fixture 与构建/运行脚本](2026-10-16-render-framework-regression-diagnosis.json)
- [分阶段 CSV](2026-10-16-render-framework-regression-phases.csv)
- [完成 CPU 与 key 微基准 CSV](2026-10-16-render-framework-regression-details.csv)
- [固定值种数的规模扫描 CSV](2026-10-16-render-framework-regression-scale.csv)

本机完整目录为 `build_descriptor_reuse/diagnosis/`、`diagnosis-detail/`、`diagnosis-scale/`。
exe、DLL 和原始日志未全部入库；附件保留校验与可重建 fixture，不承诺离开本机仍有这些二进制。
这些是性能诊断运行，没有将其冒充新的 GPU 颜色/深度正确性回归；生产源码与已验证版本一致。
