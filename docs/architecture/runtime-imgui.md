> - 适用: 查找 ImGui 适配的移除状态与旧设计
> - 权威: 当前分支的移除状态；不定义新的 UI 集成方案
> - 锚点: `CMakeLists.txt`, `modules/CMakeLists.txt`, `modules/runtime/include/radray/runtime/application.h`

# ImGui 适配状态

旧 ImGui runtime/RenderGraph 适配、专用 shader、样例、测试与 CMake 选项已随旧渲染框架移除。
第三方源码和依赖恢复清单保留。ApplicationExtension 与 runtime 输入路由也已移除；NativeWindow 原始事件与 GPU 完成通知保留。
旧集成设计见[历史附件](../temp/runtime-imgui-legacy.md)，重构范围见[设计快照](../temp/render-framework-design.md)。
