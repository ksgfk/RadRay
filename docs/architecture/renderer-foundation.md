> - 适用: 查找重构期间 renderer foundation 的状态与旧设计入口
> - 权威: 当前分支的移除状态；历史契约仅见临时快照
> - 锚点: `modules/runtime/CMakeLists.txt`, `modules/runtime/tests/CMakeLists.txt`

# Renderer foundation 状态

旧 output/view/workload、RenderGraph、CPU draw/list、pool 与 history 实现及相关测试已移除。
删除前的设计和边界见[设计快照](../temp/render-framework-design.md)，完整旧契约见
[历史附件](../temp/renderer-foundation-legacy.md)。这些接口当前不可调用。

保留的宿主、组件与 shader 服务见 [Runtime 宿主](render-framework.md)，
GPU/flight、上传和提交完成协议见[帧与 GPU](frame-and-gpu.md)。当前公共 pipeline、材质与场景绘制
已在 runtime 的 `render_framework/` 中实现，契约见 [Runtime 宿主](render-framework.md)；未恢复旧 RenderGraph。
