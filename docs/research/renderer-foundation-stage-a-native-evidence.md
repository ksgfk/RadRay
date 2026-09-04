> - 适用: 核验 Stage A native capabilities、UAV ordering 与 debug labels 的实现前提
> - 权威: 2026-09-04 的调查证据，不是当前 RadRay API 契约
> - 锚点: `modules/render/include/radray/render/rhi.h`, `modules/render/src/d3d12/d3d12_impl.cpp`, `modules/render/src/vk/vulkan_impl.cpp`

# Stage A native evidence

RadRay 审阅基线为 `dfe62a8a606bb019865324b76330f546db93e507`。范围仅为当前 SDK 可表达的
texture support、同步与标注；不引入 PIX runtime、新的 Vulkan feature 或更高版本的 API。
仓库提到的 `research` skill 在当前可用技能及本机 Codex/OpenCode 技能目录中未找到，故直接
查阅以下官方参考与项目源码，并在此保存一次性报告。

- Microsoft `D3D12_RESOURCE_DESC` 在线参考（访问 2026-09-04）描述 dimension、mips、MSAA、
  format 与 resource flags 的约束；支持查询不替代 allocation 成功检查。
  [D3D12 resource descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc)。
- Khronos Vulkan 1.4 在线 reference（访问 2026-09-04）的 `VkMemoryBarrier` 说明全局 memory
  dependency 不绑定 image subresource/layout。因此 resource-only UAV ordering 可以保守扩大
  memory scope，同时保留每 mip 的原 layout；这是本次 RadRay 映射的推论。
  [VkMemoryBarrier](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryBarrier.html)。
- Microsoft BeginEvent 参考推荐 PIXBeginEvent，但 RadRay 当前不依赖 PIX runtime。
  RenderDoc `v1.x` 注释文档（访问 2026-09-04）明确支持 D3D12 BeginEvent/EndEvent，metadata=0
  的 payload 为 wchar 字符串；本实现使用这个最小标注路径，不宣称具有 PIX 的 CPU/ETW profiler。
  [Microsoft BeginEvent](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-beginevent)，
  [RenderDoc capture annotations](https://github.com/baldurk/renderdoc/blob/v1.x/docs/how/how_annotate_capture.rst)。
- 编译时以仓库 lock 管理的 DirectX-Headers/Vulkan-Headers/volk headers 为符号依据，不修改 SDK
  或 third_party 内容；实际查询与创建一致性由双 backend 的 `DeviceCapabilitiesTest` 验证。
