# Repository Guidelines

## Project Structure & Module Organization
- `modules/` contains core code split into `core`, `window`, `render`, and `imgui`.
- `tests/` contains GoogleTest executables, usually one folder per target (for example `tests/test_buddy_alloc/`).
- `examples/` holds runnable demos; `benchmarks/` holds performance targets (enabled in release builds).
- `tools/` contains utility binaries and helper tooling.
- `assets/` and `shaderlib/` store runtime assets and shader sources.
- `third_party/` and `SDKs/` are dependency trees populated by setup scripts.

## Build, Test, and Development Commands
- Configure (Windows debug): `cmake --preset win-x64-debug`
- Configure (Windows release): `cmake --preset win-x64-release`
- Build preset: `cmake --build --preset win-x64-debug` (or `win-x64-release`)
- Run tests from build dir: `ctest --test-dir build_debug --output-on-failure`
- Fetch dependencies when missing:
  - `./fetch_third_party.ps1`
  - `./fetch_sdks.ps1`
- Generate compile commands for editors:
  - `./win_gen_compile_commands.ps1 -BuildDir ./build_debug -Configuration Debug`

## Coding Style & Naming Conventions
- Language baseline: C++20 (`CMAKE_CXX_STANDARD 20`), C11 for C sources.
- Formatting is enforced by `.clang-format`: 4-space indentation, no tabs, pointer alignment left, no include sorting.
- Use lowercase, descriptive target/test names (for example `test_structured_buffer`).
- Keep module names and folder names consistent with existing layout (`modules/render`, `tests/test_*`).

## Testing Guidelines
- Framework: GoogleTest via `gtest_discover_tests(...)` in each test target.
- Add new tests under `tests/test_<feature>/` with a local `CMakeLists.txt` and source file `test_<feature>.cpp`.
- Prefer focused unit tests for core logic and backend-specific tests only where platform APIs are required.
- Run `ctest` before submitting; use `--output-on-failure` to capture failing assertions.

## Commit & Pull Request Guidelines
- Commit style in history is short, imperative, and lowercase (for example `fix build and render`, `improve bindless example`).
- Keep commits scoped to one change set; avoid mixing refactors with behavior changes.
- PRs should include:
  - clear summary of behavior change,
  - affected modules/tests,
  - platform notes (Windows/macOS, D3D12/Vulkan/Metal),
  - screenshots or logs for rendering/UI changes.
