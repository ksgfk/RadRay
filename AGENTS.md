# Repository Guidelines
## Project Structure & Module Organization
- `modules/` contains core code split into `core`, `window`, `render`, and `imgui`.
- `examples/` holds runnable demos; `benchmarks/` holds performance targets (enabled in release builds).
- `tools/` contains utility binaries and helper tooling.
- `assets/` and `shaderlib/` store runtime assets and shader sources.
- `third_party/` and `SDKs/` are dependency trees populated by setup scripts.
## Coding Style & Naming Conventions
- Language baseline: C++20 (`CMAKE_CXX_STANDARD 20`), C11 for C sources.
- 接口的可空指针使用 `Nullable<T>`, 裸指针代表不可空
