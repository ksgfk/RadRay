# Repository Guidelines

## Setup
- Populate dependencies before configuring cmake:
  - `python tools/fetch_third_party.py restore`  → `third_party/`
  - `python tools/fetch_sdks.py restore`         → `SDKs/`
- Generate `compile_commands.json` for IDE support:
  - `python tools/win_gen_compile_commands.py --build-dir build_debug --configuration Debug`
  - Output goes to `.vscode/compile_commands.json` (clangd reads from there per `opencode.json`)

## Project Structure & Module Organization
- `modules/` contains core code split into `core`, `window`, `shader`, `render`, and `runtime`.
- Dependency chain: `core` ← `window`, `core` ← `shader` ← `render` ← `runtime` (runtime requires both window + render).
- `shader` holds the shader toolchain (dxc/hlsl/spirv/spvc/msl compilers, manifest format layer) plus the manifest data vocabulary in `radray/shader/shader_types.h`. It never touches a GPU device, so shader CLIs link only `radraycore + radrayshader` and pull in zero graphics backend. All of it lives in namespace `radray::render`, same as `rhi.h`; `rhi.h` forward-includes `shader_types.h`, so existing `#include <radray/render/rhi.h>` users see those types unchanged.
- The rule for placing a type in `shader_types.h` is "is it manifest data?" — does it appear in `*.shader.json` or have a JSON codec? Do NOT use "does it hold a device pointer?"; that is necessary but not sufficient, and applying it once put 8 pure RHI-parameter types into the shader library that it never used. Types that only exist to be fed to the RHI (`PipelineLayoutDescriptor`, `VertexInputState`, `ShaderDescriptor`, ...) belong in `rhi.h`, and `radray/runtime/shader_program.h` packs manifest data into them.
- `RenderBackend` lives in `rhi.h`, not `shader_types.h`: shader-side APIs always take `ShaderBlobCategory` directly and the caller decides which bytecode it wants. The `RenderBackend → ShaderBlobCategory` mapping (`GetShaderBlobCategoryForBackend`) is in `rhi.h` too. When judging whether a lower layer needs a type, ask "if I delete this one convenience function, does the layer still need it?" — a type can end up too low simply because a helper was written there.
- Do not add a `radrayrender` dependency to `radrayshader`, and do not link the shader CLIs against `radrayruntime`/`radrayrender` — that reintroduces ~23 MB of backend objects. Verify with `link /MAP` or `ninja -C build_debug -t commands`, not `dumpbin /DEPENDENTS` (Vulkan loads via volk and leaves no dll import).
- `examples/` holds runnable demos; `benchmarks/` holds performance targets (enabled in release builds).
- `tools/` contains utility scripts (dependency fetching, compile_commands gen, shader codegen) and the `shader_gen` CLI target.
- `assets/` and `shaderlib/` store runtime assets and shader sources.
- `third_party/` and `SDKs/` are dependency trees populated by setup scripts (readonly — do not edit).

## Build
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
- Enum member names are stable public identifiers used by `magic_enum` and serialized data. Never rename an existing enum member; add a new member and migrate data explicitly when a protocol name must change.

## Exception Policy
- Never add `try`/`catch` merely to preserve a `noexcept` declaration. If an exception cannot be meaningfully handled, do not catch it; allow it to propagate, or allow `std::terminate` at a `noexcept` boundary.
- Catch only specific, recoverable exceptions and handle them explicitly. Do not use `catch (...)` to turn allocation failures, programming errors, or invariant violations into ordinary `false`, `nullopt`, or diagnostic results.
- Avoid explicit `throw` and exception-based control flow. Prefer validation, `std::error_code`, or existing result types for expected failures.
- Before introducing any new `try`, `catch`, or `throw`, explicitly ask the user and receive confirmation. This approval is required even when the exception construct appears necessary.

## Shader Conventions
- Treat `shaderlib/` as the HLSL include root. All includes must be **root-relative** and wrapped in angle brackets: `#include <core/math.hlsli>`, not `#include <shaderlib/core/math.hlsli>` and not file-relative `#include <math.hlsli>`. DXC accepts the file-relative form but the dependency scanner that computes shader source identity does not, so it would break cook/JIT.
- Use `<>` for anything resolved through the include root (i.e. everything in `shaderlib/`). Reserve `""` for genuinely path-relative includes, which currently do not exist — a quoted include is a signal that something is resolving relative to the including file and should be reviewed.
- File extensions carry meaning: `.hlsl` = entry point (has `VSMain`/`PSMain`/`CSMain`), `.hlsli` = library header (include-guarded, no entry points).
- Include guards are `RADRAY_<PATH>_HLSLI`, derived from the path (e.g. `shadow/cascade.hlsli` → `RADRAY_SHADOW_CASCADE_HLSLI`).
- Layer layout, lower layers must not include upper ones:
  - `core/` — backend shims, math, shading frame, color. No lighting or material semantics.
  - `bsdf/` — fresnel, microfacet distributions, principled BSDF. Evaluated in the local shading frame (n = +Z).
  - `lighting/` — light GPU layouts and irradiance evaluation.
  - `shadow/` — shared filtering primitives plus one file per technique.
  - `forward_pipeline/`, `imgui/` — pipeline-specific bindings and entry points.
- Naming: functions and locals `snake_case`; types and GPU struct fields `PascalCase`; macros `RADRAY_` prefixed uppercase.
- Cross-backend binding declarations go through the `VK_*` macros in `core/platform.hlsli` and the `RADRAY_FORWARD_*` macros in `forward_pipeline/bindings.hlsli`. Never write bare `register(...)` / `[[vk::binding]]` literals in a pass.
- Keyword groups are declared with `#pragma radray_keyword_group(Name, _KW...) stages(...)` in the file that owns the guarded bindings, outside any `#if`. Entry shaders inherit groups from their includes.
- A keyword must earn its variant dimension. Before adding one, check all three:
  - **Not expressible as fixed-function state.** Anything `MaterialRenderState` can express (blend, depth write, cull) must live there alone. A keyword must never coexist with, or imply, a fixed-function state that expresses the same authoring decision — that creates two sources of truth with nothing to keep them in sync (manifest validation and reflection validation both see only one side).
  - **Genuinely changes bytecode.** A branch that is dead whenever its own keyword is off does not qualify. Example: a back-face normal flip guarded by `SV_IsFrontFace` is already unreachable under `CullMode::Back`, so guarding it with a keyword buys nothing.
  - **Scope is explicit.** Either pipeline-level (one value for the whole frame, e.g. `_POINT_SHADOWS` guarding shadow-map bindings) or material-level (per-draw, e.g. `_BASECOLOR_MAP`). A per-draw decision cannot be a pipeline-level keyword; the pipeline has no single value to pick.
  Legitimate keywords typically guard descriptor bindings or code with no fixed-function equivalent (`_ALPHATEST_ON` needs `clip()`; `_POINT_SHADOWS` needs a `TextureCube` binding).
- Reuse existing implementations in `shaderlib/` before adding shader-local helper functions.

## Test
- Test sources go in `modules/<module>/tests/`.
- Tests are registered in CMake with `radray_add_test` (plain gtest) or `radray_add_radray_gtest_case` (for tests needing `RADRAY_PROJECT_DIR`, `RADRAY_TEST_ENV_DIR`, etc.).
- `radray_add_test` pins `DISCOVERY_MODE PRE_TEST`. Do not drop it: CMake 4.4 never passes `TEST_TARGET` to `gtest_discover_tests_impl`, so its per-target anti-race hash degenerates to the empty-string hash `e3b0c44298` and every target in a directory shares one discovery json. With the default POST_BUILD mode that races, producing random `JSON failed parsing` build errors or tests silently registered against the wrong executable.
- A `ctest -N` count is not evidence of coverage. Verify by comparing each exe's `--gtest_list_tests` output against the ctest registration list in both directions.
- CTest test preset uses ClangCL: `ctest --preset win-x64 -R {test name} --output-on-failure`
- **Critical**: `-R` matches the **gtest suite name** (the C++ class), NOT the cmake target name.
- Do NOT run build and test concurrently.
