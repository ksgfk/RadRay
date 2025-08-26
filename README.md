# RadRay

TODO

## compile

* cmake config
  * cmake --preset win-x64-debug
  * cmake --preset win-x64-release

* generate compile_commands.json on win
  * .\win_gen_compile_commands.ps1 -BuildDir .\build_debug -Configuration Debug
  * .\win_gen_compile_commands.ps1 -BuildDir .\build_release -Configuration Release

## dev env

### vscode 

install extensions: C/C++ Extension Pack, clangd

### settings.json

```json
{
    "C_Cpp.codeAnalysis.runAutomatically": false,
    "C_Cpp.intelliSenseEngine": "disabled",
    "C_Cpp.formatting": "disabled",
    "C_Cpp.autoAddFileAssociations": false,
    "C_Cpp.autocompleteAddParentheses": false,
    "C_Cpp.autocomplete": "disabled",
    "C_Cpp.errorSquiggles": "disabled",
    "clangd.arguments": [
        "--compile-commands-dir=${workspaceFolder}/.vscode",
        "--log=error",
        "--completion-style=bundled",
        "--background-index",
        "--background-index-priority=normal",
        "--header-insertion=never",
        "--pch-storage=memory"
    ]
}
```

### launch.json

example

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "(msvc) Launch",
            "type": "cppvsdbg",
            "request": "launch",
            "program": "${command:cmake.launchTargetPath}",
            "args": [],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [
                {
                    "name": "PATH",
                    "value": "${env:PATH}:${command:cmake.getLaunchTargetDirectory}"
                }
            ],
            "console": "integratedTerminal"
        }
    ]
}
```

#### special launch settings

##### bench_read_obj

* "cwd": "${workspaceFolder}"
