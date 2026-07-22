# Repository Guidelines

## Setup
- Populate dependencies before configuring cmake:
  - `python tools/fetch_third_party.py restore`  → `third_party/`
  - `python tools/fetch_sdks.py restore`         → `SDKs/`
- Generate `compile_commands.json` for IDE support:
  - `python tools/win_gen_compile_commands.py --build-dir build_debug --configuration Debug`
  - Output goes to `.vscode/compile_commands.json` (clangd reads from there per `opencode.json`)

## Project Structure & Module Organization
- `modules/` contains core code split into `core`, `window`, `render`, and `runtime`.
- Dependency chain: `core` ← `window`, `render` ← `runtime` (requires both window + render).
- `examples/` holds runnable demos; `benchmarks/` holds performance targets (enabled in release builds).
- `tools/` contains utility scripts (dependency fetching, compile_commands gen, shader codegen).
- `assets/` and `shaderlib/` store runtime assets and shader sources.
- `third_party/` and `SDKs/` are dependency trees populated by setup scripts (readonly — do not edit).

## Build
- The MSVC preset requires a Visual Studio Developer environment to be initialized before configuring or building. If the MSVC environment is unavailable, locate `VsDevCmd.bat` with `rtk find VsDevCmd.bat 'C:\Program Files\Microsoft Visual Studio'`.
- Config: `cmake --preset win-x64-debug` (MSVC) or `cmake --preset win-x64-debug-clangcl` (ClangCL)
- Build: `cmake --build build_debug --parallel 24`
- Built binaries land in `build_debug/_build/<Config>/`

## Coding Style & Naming Conventions
- Language baseline: C++20 (`CMAKE_CXX_STANDARD 20`), C11 for C sources.
- STL containers must use `radray` namespace aliases (e.g. `string`, `vector`, `unordered_map`) from `radray/types.h`.
- Coroutines must use `radray` namespace aliases from `radray/coroutine.h` (e.g. `radray::task`), never `exec::task` / `stdexec::*` directly. The aliases wrap stdexec so the underlying library can be swapped without touching call sites.
- Interface nullable pointers use `Nullable<T>`; raw pointers mean non-null.
- DEBUG mode uses macro `RADRAY_IS_DEBUG` (NOT `NDEBUG` or `_DEBUG`).
- String formatting must use `fmt` library; check whether a type has `format_to` before using it.
- Flag-style enums use `enum_flags.h` (`EnumFlags<T>`, `is_flags<T>`, `format_as`).

## Exception Policy
- Never add `try`/`catch` merely to preserve a `noexcept` declaration. If an exception cannot be meaningfully handled, do not catch it; allow it to propagate, or allow `std::terminate` at a `noexcept` boundary.
- Catch only specific, recoverable exceptions and handle them explicitly. Do not use `catch (...)` to turn allocation failures, programming errors, or invariant violations into ordinary `false`, `nullopt`, or diagnostic results.
- Avoid explicit `throw` and exception-based control flow. Prefer validation, `std::error_code`, or existing result types for expected failures.
- Before introducing any new `try`, `catch`, or `throw`, explicitly ask the user and receive confirmation. This approval is required even when the exception construct appears necessary.

## Shader Conventions
- Treat `shaderlib/` as the HLSL include root. Include shared shader files as `#include "common.hlsl"` or `#include "bsdf.hlsl"`, not `#include "shaderlib/common.hlsl"`.
- Reuse existing implementations in `shaderlib/` before adding shader-local helper functions.

## Test
- Test sources go in `modules/<module>/tests/`.
- Tests are registered in CMake with `radray_add_test` (plain gtest) or `radray_add_radray_gtest_case` (for tests needing `RADRAY_PROJECT_DIR`, `RADRAY_TEST_ENV_DIR`, etc.).
- CTest test preset uses ClangCL: `ctest --preset win-x64 -R {test name} --output-on-failure`
- **Critical**: `-R` matches the **gtest suite name** (the C++ class), NOT the cmake target name.
- Do NOT run build and test concurrently.
