> - 适用: 配置 IDE、clangd、调试器
> - 权威: 本文是 IDE 侧配置的唯一来源。构建命令本身见 `guide/build-test.md`
> - 锚点: `opencode.json`, `.clang-format`, `tools/win_gen_compile_commands.py`

# 开发环境

## VSCode

装扩展：`C/C++ Extension Pack`、`clangd`。

两个 IntelliSense 引擎不能共存，方案是**全程用 clangd，把 cpptools 的 IntelliSense 全关**，
只留它的 clang-tidy 代码分析。

`.vscode/` 不在版本控制内，下面两份配置需要手动落地。

### settings.json

```json
{
    "C_Cpp.intelliSenseEngine": "disabled",
    "C_Cpp.formatting": "disabled",
    "C_Cpp.autocomplete": "disabled",
    "C_Cpp.errorSquiggles": "disabled",
    "C_Cpp.codeFolding": "disabled",
    "C_Cpp.autoAddFileAssociations": false,
    "C_Cpp.autocompleteAddParentheses": false,
    "C_Cpp.configurationWarnings": "disabled",
    "C_Cpp.default.enableConfigurationSquiggles": false,
    "C_Cpp.codeAnalysis.runAutomatically": true,
    "C_Cpp.codeAnalysis.clangTidy.enabled": true,
    "C_Cpp.codeAnalysis.clangTidy.args": [
        "--config-file=${workspaceFolder}/.clang-tidy",
        "-p",
        "${workspaceFolder}/.vscode"
    ],
    "clangd.enable": true,
    "clangd.arguments": [
        "--compile-commands-dir=${workspaceFolder}/.vscode",
        "--log=error",
        "--completion-style=bundled",
        "--background-index",
        "--background-index-priority=normal",
        "--header-insertion=never",
        "--pch-storage=memory"
    ],
    "cmake.buildArgs": ["--parallel", "24"],
    "files.readonlyInclude": {
        "**/build**/**": true,
        "**/third_party/**": true,
        "**/SDKs/**": true
    },
    "VSCodeCounter.exclude": [
        "**/.github/**",
        "**/.vscode/**",
        "**/build**/**",
        "**/assets/**",
        "**/third_party/**",
        "**/SDKs/**",
        "**/dear_imgui_shader_spirv.cpp",
        "**/dear_imgui_shader_dxil.cpp",
        "**/dear_imgui_shader_metallib.cpp",
        "**/imgui.ini"
    ]
}
```

`--compile-commands-dir` 指向 `.vscode`，所以改完构建配置要重跑
`tools/win_gen_compile_commands.py`（见 `guide/build-test.md`）。

### launch.json

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "(msvc) Launch",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "${command:cmake.launchTargetPath}",
            "args": ["--backend", "d3d12", "--multithread", "--valid-layer"],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [
                {
                    "name": "PATH",
                    "value": "${command:cmake.getLaunchTargetDirectory}:${env:PATH}"
                }
            ],
            "console": "integratedTerminal"
        }
    ]
}
```

`--backend` 可选 `d3d12` / `vulkan` / `metal`（metal 无实现）。
`PATH` 必须带上目标输出目录，否则 DXC 运行库加载不到。

## 格式化

`.clang-format` 在仓库根。clangd 直接用它，不需要额外配置。

## opencode

`opencode.json` 配置了 clangd LSP。文件本身在 `.gitignore` 里，属于个人配置。
