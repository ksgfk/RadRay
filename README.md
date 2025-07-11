# RadRay

## compile
* xmake f -m debug/release -v
* xmake -v

tips:
* make file `options.lua` use template `options.lua.template` in `scripts` folder. can set many settings in `options.lua`
* cmd command `xmake i -o bin` can export binaries to bin dir
* [optional] cmd `xmake lua scripts/after_build_test.lua [install]` to copy test assets to build dir

## dev env
* vscode
  * install extensions: C/C++, XMake, clangd
* suggest settings.json
```json
{
    "xmake.additionalConfigArguments": [],
    "xmake.compileCommandsDirectory": ".vscode",
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
    ],
    "VSCodeCounter.exclude": [
        "**/.gitignore",
        "**/.vscode/**",
        "**/build/**",
        "**/.xmake/**",
        "**/ext/**",
        "**/volk.h",
        "**/volk.c",
        "**/vk_mem_alloc.h",
        "**/vk_mem_alloc.cpp",
    ]
}
```
