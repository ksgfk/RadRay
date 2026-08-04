# RadRay

C++20 实时渲染器，D3D12 + Vulkan 双后端，纯光栅。

本文件是**领域词汇表**：定义术语「是什么」。不描述实现、不记录决策、不当规格书用。
实现现状见 `docs/architecture/`，决策见 `docs/adr/`。

## Shader 管线

**Pass**:
共享同一份绑定 ABI 的一组 stage。一次绘制所需的完整 shader 程序。
_Avoid_: program, effect, technique

**Stage**:
Pass 中的一个可编程管线阶段（vertex / pixel / compute）。
_Avoid_: 单独使用的 "shader"（过于笼统，可指 stage、pass 或字节码）

**Variant**:
同一个 Pass 在一组具体特性开关取值下的编译产物。
_Avoid_: permutation, configuration, 变种

**Trace**:
执行一份 C++ shader 定义、把其中的运算记录成 AST 的过程。Shader 源真相是 C++ 代码，
HLSL 是 trace 之后由 codegen 产出的中间文本。
_Avoid_: 录制, capture, JIT（JIT 指编译时机，trace 指记录行为）

**Binding layout**:
一个 Pass 需要哪些资源、各自落在哪个 group 与 slot 上的完整描述。由 trace 产出，
是绑定 ABI 的唯一权威。
_Avoid_: pipeline layout, root signature, descriptor set layout（这三个是后端说法）；
Property（这是 codegen 内部的表示）；binding ABI（指同一事物的契约面，不指数据本身）

**Binding group**:
一组资源绑定的集合。同时是 D3D12 的 register space 与 Vulkan 的 descriptor set index ——
这是后端已硬化的不变量，任何一层都不做重映射。
_Avoid_: descriptor set, register space, space, table

**Residency**:
一个绑定是经 descriptor table 访问，还是作为 root descriptor 直接绑定。属于性能决策，
不是 shader 的属性。
_Avoid_: binding mode, access path

**Artifact**:
离线编译产出的、可在无编译器环境下加载的 shader 产物。内容寻址。
_Avoid_: blob（指承载 artifact 的单个文件）, cache（cache 可弃，artifact 是交付物）

### 离线编译（术语待定）

第一期不做离线编译，"cook" 与 "bake" 两个词一并搁置 —— 旧代码里它们的边界要靠注释解释
（`shader_manifest.h:150`），属于需要重新命名的遗留。等第一期跑通、真要做离线产物时再定名。
在此之前不要在新代码里引入这两个词。

## 资产

**Asset**:
由路径标识、引用计数管理生命周期的可加载资源。
_Avoid_: resource（resource 指 GPU 侧对象）

**AssetId**:
由归一化路径派生的资产标识。
_Avoid_: asset key, path hash
