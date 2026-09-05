> - 适用: 运行、漫游、修改或验证 Tidal Atrium 渲染展示场景
> - 权威: 本文说明样例操作与展示范围；通用渲染契约见 renderer foundation
> - 锚点: `examples/example_tidal_atrium/`, `shaderlib/pipelines/atrium/`, `tools/generate_tidal_atrium.py`, `tools/verify_tidal_atrium.py`

# 潮汐光庭 / Tidal Atrium

开放式夜间光艺术馆。中央天球仪、彩色玻璃长廊、材质雕塑、计算艺术屏与实时俯视屏组成可自由
飞行的场景。应用使用 runtime 的公开 scene、snapshot、culling、renderer list、RenderGraph、
external output 与 view history 接口；渲染管线和输入控制位于样例目录。

## 生成与运行

需要已恢复的仓库依赖、启用 JIT 的 runtime、可用的 D3D12 或 Vulkan 设备。当前资产生成脚本使用
Python、Pillow 和本机 Windows 的 Consolas / Segoe UI 字体。构建配置见[构建与测试](build-test.md)。

```powershell
python tools/generate_tidal_atrium.py
cmake -S . -B build_debug
cmake --build build_debug --config Debug --target example_tidal_atrium --parallel 24
.\build_debug\_build\Debug\example_tidal_atrium.exe --backend d3d12
```

使用 Vulkan 和独立渲染线程：

```powershell
.\build_debug\_build\Debug\example_tidal_atrium.exe --backend vulkan --multithread
```

程序默认 1280 × 800，窗口可以缩放。`--width N --height N` 指定初始尺寸，`--valid-layer` 开启后端
校验层。默认在 150% 内部分辨率绘制场景，再缩小到窗口尺寸；界面保持输出分辨率。

## 漫游

这是可穿过建筑的自由飞行观察器，没有重力和碰撞；高度限制为 0.65–60 米。

| 操作 | 按键 |
|---|---|
| 前后左右移动 | W / S / A / D |
| 转动视角 | 按住鼠标右键移动，或方向键 |
| 下降 / 上升 | Q / E |
| 加速 | Shift |
| 跳到五个展区 | 1–5 |
| 暂停 / 继续雕塑、光源和历史图案 | Space |
| 深度预通道开关 | F2 |
| 线框模式 | F3 |
| 同一目标上的双视图 | F4 |
| 悬浮青色信标图层 | F5 |
| 计算图案的历史反馈 | F6 |
| 内部分辨率 100% / 150% | F7 |
| 详细操作提示 | H 或 F1 |
| 隐藏 / 显示全部界面 | Tab |
| 释放鼠标；未捕获鼠标时退出 | Esc |

失去窗口焦点会释放鼠标、解除光标限制并清空按键状态。

## 五个展区

| 观景点 | 可观察内容 |
|---|---|
| 1 · Light Court | 动态天球仪、一个方向光和三个绕轨点光；中心球的材质颜色逐帧变化；F2 对比深度预通道 |
| 2 · Chromatic Walk | 青、琥珀、玫瑰色透明板；移动相机观察从后向前排序与不透明物体的遮挡 |
| 3 · Material Library | 石材与铜色雕塑；两颗同形球分别使用 nearest / 无 mip 与 trilinear / mip chain；双色花瓶使用 binding 3、7 两个顶点流和两个材质 section |
| 4 · Signal Garden | 左屏是计算着色器生成并读取上一帧的动态图案，F6 对比反馈；右屏是正交相机的实际离屏输出；背景为原创壁画 |
| 5 · Observatory | F4 以两个不重叠视图共享颜色和深度 attachment；左侧按 F5 决定是否显示信标，右侧只显示建筑图层 |

右侧小窗持续显示同一份俯视输出和计算图案。统计面板显示主视图可见 primitive、depth / lit /
transparent draw 数，以及上一帧 graph 的 pass、barrier、资源池命中与分配数。摄像机转向时，
每个视图独立进行 CPU 视锥和 layer mask 剔除。

## 展示范围

材质以 `ForwardLit` 为 primary pass；通常同时具有 `DepthOnly`，标牌、壁画与接触贴花则省略
`DepthOnly`。通用 renderer list 按队列选择 pass，透明材质不进入预通道。每个 flight 拥有独立的
场景值、draw lists、上传内存和不可变 descriptor sets；资产在 GPU 完成前由宿主引用保活。

场景 surface shader 在 Lambert 光照之上加入解析高光、自发光和距离雾，以统一视觉风格。
程序天空按每个视图的投影和相机旋转重建世界空间视线，以世界 +Y 确定高度角和地平线。
星空也按世界方向采样；相机平移不改变天空，转动或修改视场角时天空与场景使用一致的投影。
分屏分别设置各视图的参数、viewport 和 scissor；正交俯视相机使用平行视线。
地面的柔和暗部是预先绘制的透明接触贴花。当前样例没有实时 shadow map、PBR 管线、TAA、
SSR、GI 或 bloom；shaderlib 中已有的 BSDF / shadow 数学原语不等同于已接通的 runtime 管线。
已有框架的能力和限制以 [Renderer foundation](../architecture/renderer-foundation.md) 为准。

计算屏使用与 `RWTexture2D<float4>` 对应的 `RGBA32_FLOAT` 存储纹理。view history 在初次进入、
观景点切换、分屏或尺寸变化时按框架规则失效，再从黑色背景建立有效历史。它展示图像反馈，
不进行运动矢量重投影。截图通过 graph 的 texture-to-buffer copy 和 host-read barrier，等待 GPU
完成后逐行读回。它保存输出尺寸的 RGBA 图像，后端 swapchain 可使用 BGRA。

## 素材

生成器将 OBJ、PNG、`scene.json` 和素材来源说明写入被忽略的 `assets/tidal_atrium/`，并向
`assets/assets.json` 增补固定 UUID 的条目。已有其他资产条目保持原值。指定其他资产根时，生成和
运行使用相同目录：`python tools/generate_tidal_atrium.py --assets PATH`，再设置 `RADRAY_ASSETS_DIR`。

`scene.json` 保存位置、正数缩放、角度、材质名、动画名和图层。网格与程序纹理由固定参数重建。
`tidal_mural.png` 的现有原图不会被覆盖；本次场景使用生成的原创壁画，原始提示保存于同目录的
`mural_prompt.txt`。全新资产目录没有壁画时，脚本会生成原创曲线图案作为默认壁画，使场景可直接
运行。字体栅格在本机生成；对外分发时应随资产保留 `provenance.json` 并核对所用字体的分发条件。

## 自动巡游与验证

直接巡游 360 帧并导出 14 组截图、graph JSON 与 draw metrics：

```powershell
.\build_debug\_build\Debug\example_tidal_atrium.exe --backend d3d12 --valid-layer --tour --capture-dir build_debug/atrium-tour
```

`--frames N` 限定就绪后的帧数，`--tour` 未指定帧数时使用 360；没有巡游时，设置 `--frames N`
和 `--capture-dir` 会在结束前保存当前视角。巡游还会通过窗口事件检查键盘移动，并在可取得焦点的
Windows 窗口上检查鼠标捕获、转向和释放；窗口自动缩为 1000 × 700 后恢复。

以下验证器执行真正的 GPU 巡游，检查后端校验输出、14 张 PNG、无丢弃 draw、multi-section 数量、
各 pass 执行、history、layer culling、资源池复用、尺寸与分辨率恢复。它对暂停场景的 depth 开关做
像素对照，并确认 wireframe / history 的可见差异。每次运行指定一个新的输出目录，避免使用旧截图。

```powershell
python tools/verify_tidal_atrium.py --exe build_debug/_build/Debug/example_tidal_atrium.exe --backend d3d12 --output build_debug/atrium-d3d12-check
python tools/verify_tidal_atrium.py --exe build_debug/_build/Debug/example_tidal_atrium.exe --backend vulkan --multithread --output build_debug/atrium-vulkan-check --compare build_debug/atrium-d3d12-check
```

省略 `--exe` 可以重新检查已有输出。`--compare` 对比两个后端相同场景区域的像素，避开随运行时长
变化的界面统计。日志和 `verification.json` 留在指定构建目录，不作为长期文档。

天空专项验证使用 `--sky-test`，默认 82 帧，导出平视、抬头、低头、水平转向、平移、滚转、
分屏和窄视场角的 8 组截图。相机远离场景且关闭界面，使读回像素只包含天空。验证器将亮带位置
与世界水平面的投影对照，并检查平移前后像素相同、转向时星空变化，以及分屏与单视图的方向一致。

```powershell
python tools/verify_tidal_atrium.py --exe build_debug/_build/Debug/example_tidal_atrium.exe --backend d3d12 --sky-test --output build_debug/atrium-sky-d3d12
python tools/verify_tidal_atrium.py --exe build_debug/_build/Debug/example_tidal_atrium.exe --backend vulkan --multithread --sky-test --output build_debug/atrium-sky-vulkan --compare build_debug/atrium-sky-d3d12
```
