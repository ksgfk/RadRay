> - 适用: 配置 IDE、clangd、调试器
> - 权威: 本文是 IDE 侧配置的唯一来源。构建命令本身见 [build-test](build-test.md)
> - 锚点: `.clang-format`, `CMakePresets.json`, `tools/win_gen_compile_commands.py`

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
        "**/SDKs/**"
    ]
}
```

`--compile-commands-dir` 指向 `.vscode`，所以改完构建配置要重新生成或复制编译数据库；
Ninja 和 MSBuild 的处理不同，见[构建与测试](build-test.md)。
仓库没有 `.clang-tidy`；示例不传不存在的配置文件。需要自定义检查时先建立自己的配置再指定它。

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
            "args": ["--d3d12", "--multithread", "--valid-layer"],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [
                {
                    "name": "PATH",
                    "value": "${command:cmake.getLaunchTargetDirectory};${env:PATH}"
                }
            ],
            "console": "integratedTerminal"
        }
    ]
}
```

以上参数用于 `example_scene_sync`，用 `--vulkan` 切换后端；`example_framework_stress` 的验证开关是 `--validation`。
先选择对应 CMake launch target。Windows `PATH` 用分号分隔，并包含已部署 DXC 运行库的目标输出目录。

## 格式化

`.clang-format` 在仓库根。clangd 直接用它，不需要额外配置。

## opencode

`opencode.json` 配置了 clangd LSP。文件本身在 `.gitignore` 里，属于个人配置。
