# Repository Guidelines

## The most important Scope Discipline
- Do not silently expand a user's requested change into adjacent architecture, examples, tests, or backend API work.
- If a fix appears to require work outside the explicit instruction, first explain the dependency and ask whether to include it.
- If the user gives an architectural constraint, follow it even when a broader implementation seems convenient.
- Prefer a narrow, incomplete-looking change plus a question over completing unrequested design gaps.
- When unsure whether the user considered an edge case, ask before implementing extra behavior.

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
- Config: `cmake --preset win-x64-debug` (MSVC) or `cmake --preset win-x64-debug-clangcl` (ClangCL)
- Build: `cmake --build build_debug --parallel 24`
- Built binaries land in `build_debug/_build/<Config>/`

## Coding Style & Naming Conventions
- Language baseline: C++20 (`CMAKE_CXX_STANDARD 20`), C11 for C sources.
- STL containers must use `radray` namespace aliases (e.g. `string`, `vector`, `unordered_map`) from `radray/types.h`.
- Interface nullable pointers use `Nullable<T>`; raw pointers mean non-null.
- DEBUG mode uses macro `RADRAY_IS_DEBUG` (NOT `NDEBUG` or `_DEBUG`).
- String formatting must use `fmt` library; check whether a type has `format_to` before using it.
- Flag-style enums use `enum_flags.h` (`EnumFlags<T>`, `is_flags<T>`, `format_as`).

## Test
- Test sources go in `modules/<module>/tests/`.
- Tests are registered in CMake with `radray_add_test` (plain gtest) or `radray_add_radray_gtest_case` (for tests needing `RADRAY_PROJECT_DIR`, `RADRAY_TEST_ENV_DIR`, etc.).
- CTest test preset uses ClangCL: `ctest --preset win-x64 -R BuddyAllocatorTest --output-on-failure`
- **Critical**: `-R` matches the **gtest suite name** (the C++ class), NOT the cmake target name.
- Do NOT run build and test concurrently.

## Non-obvious CMake Functions
All defined in `cmake/Utility.cmake`:
- `radray_add_test` / `radray_add_gtest_case` / `radray_add_radray_gtest_case`
- `radray_add_example` / `radray_example_files`
- `radray_default_compile_flags` / `radray_optimize_flags_binary` / `radray_optimize_flags_library`

## Toolchain Quirks
- `RADRAY_IS_DEBUG` is defined for all configs except Release (see `cmake/Utility.cmake:164`).
- On Windows, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `UNICODE`, `_UNICODE` are always defined.
