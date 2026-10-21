> - 适用: 查阅 runtime 移除前的 renderer-foundation 设计与操作
> - 权威: 2026-10-21 归档快照；不是当前契约或本次验证结果
> - 锚点: `docs/history/2026-10-21-runtime-archive.md`

状态：已归档。日期为本次归档日期。基线：`751aca4e6a70203e6a62f9de67893b33463b94fc`，文件树等同原 HEAD `ed23aef93b779082d3efab221bbe52d9f30051f0`。
原文件（相对 docs 根）：`architecture/renderer-foundation.md`。正文中的“当前”、命令、测试与能力均限定于此基线；共享说明保留上下文，不覆盖当前长期文档。
原源码锚点（通过归档基线查阅）：`modules/runtime/CMakeLists.txt`, `modules/runtime/tests/CMakeLists.txt`

# Renderer foundation 状态

旧 output/view/workload、RenderGraph、CPU draw/list、pool 与 history 实现及相关测试已移除。
删除前的设计和边界见[设计快照](2026-09-14-render-framework-design.md)，完整旧契约见
[历史附件](2026-09-14-renderer-foundation-legacy.md)。这些接口当前不可调用。

保留的宿主、组件与 shader 服务见 [Runtime 宿主](2026-10-21-runtime-render-framework-snapshot.md)，
GPU/flight、上传和提交完成协议见[帧与 GPU](2026-10-21-runtime-frame-and-gpu-snapshot.md)。当前公共 pipeline、材质与场景绘制
已在 runtime 的 `render_framework/` 中实现，契约见 [Runtime 宿主](2026-10-21-runtime-render-framework-snapshot.md)；未恢复旧 RenderGraph。
