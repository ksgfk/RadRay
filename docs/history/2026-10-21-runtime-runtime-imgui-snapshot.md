> - 适用: 查阅 runtime 移除前的 runtime-imgui 设计与操作
> - 权威: 2026-10-21 归档快照；不是当前契约或本次验证结果
> - 锚点: `docs/history/2026-10-21-runtime-archive.md`

状态：已归档。日期为本次归档日期。基线：`751aca4e6a70203e6a62f9de67893b33463b94fc`，文件树等同原 HEAD `ed23aef93b779082d3efab221bbe52d9f30051f0`。
原文件（相对 docs 根）：`architecture/runtime-imgui.md`。正文中的“当前”、命令、测试与能力均限定于此基线；共享说明保留上下文，不覆盖当前长期文档。
原源码锚点（通过归档基线查阅）：`CMakeLists.txt`, `modules/CMakeLists.txt`, `modules/runtime/include/radray/runtime/application.h`

# ImGui 适配状态

旧 ImGui runtime/RenderGraph 适配、专用 shader、样例、测试与 CMake 选项已随旧渲染框架移除。
第三方源码和依赖恢复清单保留。ApplicationExtension 与 runtime 输入路由也已移除；NativeWindow 原始事件与 GPU 完成通知保留。
旧集成设计见[历史附件](2026-09-14-runtime-imgui-legacy.md)，重构范围见[设计快照](2026-09-14-render-framework-design.md)。
