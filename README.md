# RadRay

C++20 实时渲染器。D3D12 + Vulkan 后端，Windows 为主平台，macOS 走 Vulkan-on-Metal。

## 文档

| 我要做什么 | 读哪里 |
|---|---|
| 了解仓库结构、定位子系统 | [docs/architecture/overview.md](docs/architecture/overview.md) |
| 配置、构建、跑测试、拉依赖 | [docs/guide/build-test.md](docs/guide/build-test.md) |
| 配置 IDE / clangd / 调试器 | [docs/guide/dev-env.md](docs/guide/dev-env.md) |
| 某个设计为什么这样 | [docs/adr/](docs/adr/README.md) |

`AGENTS.md` 是给编码 agent 的入口约束，人类读者可以跳过。

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
