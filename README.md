# RadRay

C++20 实时渲染器。D3D12 + Vulkan 后端，Windows 为主平台，macOS 走 Vulkan-on-Metal。

## 文档

| 我要做什么 | 读哪里 |
|---|---|
| 了解仓库结构、定位子系统 | [docs/architecture/overview.md](docs/architecture/overview.md) |
| 配置、构建、跑测试、拉依赖 | [docs/guide/build-test.md](docs/guide/build-test.md) |
| 配置 IDE / clangd / 调试器 | [docs/guide/dev-env.md](docs/guide/dev-env.md) |
| 当前设计、术语与取舍 | 从[架构地图](docs/architecture/overview.md)进入所属子系统文档 |
| 整理文档、使用项目 doc / grill 技能 | [文档维护](docs/guide/documentation.md) |

[AGENTS.md](AGENTS.md) 也是长期文档，记录仓库级约束与阅读入口。
长期知识集中在它与 `docs/architecture/`、`docs/guide/`，历史版本由 Git 保存。

## 快速开始

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --parallel 24
```

二进制在 `build_debug/_build/<Config>/`。

## TODO

- [P2] SwapChain 支持 HDR

## License

见 [LICENSE](LICENSE)。
