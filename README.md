# RadRay

TODO

## TodoList
* [P2] SwapChain 支持 HDR

## compile

* cmake config
  * cmake --preset win-x64-debug
  * cmake --preset win-x64-release

* generate compile_commands.json on win
  * python .\tools\win_gen_compile_commands.py --build-dir .\build_debug --configuration Debug
  * python .\tools\win_gen_compile_commands.py --build-dir .\build_release --configuration Release

## dev env

### vscode 

install extensions: C/C++ Extension Pack, clangd

### settings.json

```json
{
    "xmake.additionalConfigArguments": [],
    "xmake.compileCommandsDirectory": ".vscode",
    "C_Cpp.codeAnalysis.runAutomatically": true,
    "C_Cpp.intelliSenseEngine": "disabled",
    "C_Cpp.formatting": "disabled",
    "C_Cpp.autoAddFileAssociations": false,
    "C_Cpp.autocompleteAddParentheses": false,
    "C_Cpp.autocomplete": "disabled",
    "C_Cpp.errorSquiggles": "disabled",
    "C_Cpp.codeFolding": "disabled",
    "C_Cpp.configurationWarnings": "disabled",
    "C_Cpp.default.enableConfigurationSquiggles": false,
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
    ],
    "files.readonlyInclude": {
        "**/build**/**": true,
        "**/third_party/**": true,
        "**/SDKs/**": true
    },
    "cmake.buildArgs": [
        "--parallel",
        "24"
    ]
}
```

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
            "args": [
                "--backend",
                // "vulkan",
                "d3d12",
                // "metal",
                "--multithread",
                "--valid-layer"
            ],
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
