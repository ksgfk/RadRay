# RadRay

## compile
* xmake f -m debug/release -v
* xmake -v

tips:
* make file `options.lua` use template `options.lua.template` in `scripts` folder. can set many settings in `options.lua`

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
    ]
}
```
