> - 适用: 配置 IDE、clangd、调试器
> - 权威: 本文是 IDE 侧配置的唯一来源。构建命令本身见 [build-test](build-test.md)
> - 锚点: `.clang-format`, `CMakePresets.json`, `cmake/Utility.cmake`, `cmake/UpdateCompileCommands.cmake`, `tools/win_gen_compile_commands.py`, `tools/expand_unity_compile_commands.py`

# 开发环境

## VSCode

装扩展：`C/C++ Extension Pack`、`clangd`。

两个 IntelliSense 引擎不能共存，方案是**全程用 clangd，把 cpptools 的 IntelliSense 全关**，
只留它的 clang-tidy 代码分析。

`.vscode/` 不在版本控制内，下面的配置需要手动落地。

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
    "cmake.copyCompileCommands": null,
    "cmake.mergedCompileCommands": null,
    "cmake.postConfigureTask": "RadRay: Update compile commands",
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

`--compile-commands-dir` 指向 `.vscode`。Ninja/Makefile 的编译数据库由下方配置完成 task 更新；
clang-cl 的 Visual Studio/MSBuild 预设继续使用已有脚本，见[构建与测试](build-test.md#编译数据库与文档检查)。
仓库没有 `.clang-tidy`；示例不传不存在的配置文件。需要自定义检查时先建立自己的配置再指定它。

### 编译数据库后处理

在 `.vscode/tasks.json` 中加入以下 task，标签与 `cmake.postConfigureTask` 一致。
每次 CMake Tools 成功 configure/generate 后，它会先检查当前构建目录的 generator，
仅在 Ninja/Makefile 下调用 CMake 目标展开数据库；Visual Studio/MSBuild 会直接跳过。
`copyCompileCommands` 和 `mergedCompileCommands` 显式设为 `null`，覆盖用户级复制设置，
Ninja/Makefile 由这个 task 写入 clangd 的数据库路径。

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "RadRay: Update compile commands",
            "type": "process",
            "command": "cmake",
            "args": [
                "-DRADRAY_BUILD_DIR=${command:cmake.buildDirectory}",
                "-DRADRAY_BUILD_CONFIG=${command:cmake.buildType}",
                "-P", "${workspaceFolder}/cmake/UpdateCompileCommands.cmake"
            ],
            "problemMatcher": []
        }
    ]
}
```

该目标仅支持 Ninja/Makefile，使用当前构建目录与配置，不编译 C++；
Visual Studio/MSBuild 不创建该目标，也不自动调用或串联 `win_gen_compile_commands.py`。
命令行只执行 configure 时不会触发 VSCode task，可显式执行
`cmake --build <build-dir> --config <Config> --target radray_compile_commands`。

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
