# RadRay

C++20 real-time renderer. Five static libs under `modules/` (plus an optional shader-compiler
client), D3D12 + Vulkan backends.

## Docs first

Knowledge lives in `docs/`, not in code comments. Read on demand from the table below —
do **not** preload the whole tree.

| Task | Read |
|---|---|
| Don't know where to start; locating a subsystem | `docs/architecture/overview.md` |
| Configure, build, run tests, fetch deps | `docs/guide/build-test.md` |
| Naming, interface style, test conventions | `docs/guide/cpp-conventions.md` |
| IDE / clangd / debugger setup | `docs/guide/dev-env.md` |
| Writing HLSL or changing shader contract | `docs/guide/shader-authoring.md`, `docs/architecture/shaderlib.md`, `docs/architecture/shader-pipeline.md` |
| Adding a binding to a pass | `docs/guide/shader-authoring.md` and `docs/architecture/shader-pipeline.md` |
| Shader toolchain, variants, AOT artifacts | `docs/architecture/shader-pipeline.md`, `docs/todo/hlsl-radray-dxc-shader-pipeline.md`, and `docs/todo/filesystem-backed-shader-include-correction.md` |
| HLSL library layout and binding ABI | `docs/architecture/shaderlib.md` |
| Asset lifetime, refcounts, deferred destroy | `docs/architecture/asset-system.md` |
| Frame pacing, flights, uploads, shutdown | `docs/architecture/frame-and-gpu.md` |
| RHI, backends, barriers, synchronization | `docs/architecture/render-rhi.md` |
| Render pipeline, scene proxies, Application | `docs/architecture/render-framework.md` |
| What core provides (containers, coroutines, ...) | `docs/architecture/core-facilities.md` |
| Why is a design the way it is | Glob `docs/adr/*.md`, then read the match |
| Research an external API, engine, spec, or design question | Invoke `research`; keep one primary-source report under `docs/research/` |
| Resume a handed-off session | Read the handoff path supplied by the user/session, then the durable artifacts it names |

`docs/architecture/overview.md` always holds the current index.

**Handoff and grilling**: a handoff is session state, not repository authority. Read one only
when the user/session points to it, verify stale claims against its referenced artifacts and the
current worktree, and invoke its suggested skills by their OpenCode names. Use `grill-me` to
continue a grilling interview: resolve one decision at a time and do not implement until the user
confirms shared understanding.

When asked to transfer work to another session, invoke `handoff`; it writes a redacted summary to
the OS temporary directory. Do not create or refresh `docs/handoff/` for that purpose. Existing
files there are historical session snapshots, not durable knowledge, and therefore do not carry
the normal `适用` / `权威` / `锚点` header.

**Research**: reports under `docs/research/` are point-in-time evidence, not the current RadRay
contract. Keep the normal three-line header, record the source version/commit and investigation
scope, and cite primary sources for claims. Use repository-relative `锚点` paths for RadRay files;
absolute paths are allowed only for external source checkouts and are not portability-checked.
Use Markdown links for web sources.

**Maintenance duty**: when you change behavior a doc describes, update that doc in the same commit.
Code comments carry API contracts only; design rationale belongs in `docs/`.

**Link direction**: docs reference code, not the reverse. A doc names files in its `锚点:` header
and names types/functions/fields inline. Code may mention a doc in exactly two places:
one file-header banner naming the owning subsystem, and a guardrail on a deliberately
counter-intuitive line pointing at its ADR. Do not add `see docs/...` to individual members.

## Hard rules

Violating these is a defect, not a style preference.

### Types and naming
- STL containers must use the `radray` aliases from `radray/types.h` (`string`, `vector`, `unordered_map`, ...).
- Coroutines must use the `radray` aliases from `radray/coroutine.h` (e.g. `radray::task`),
  never `exec::task` / `stdexec::*` directly.
- Nullable interface pointers use `Nullable<T>`. A raw pointer means non-null.
- Debug builds are detected with `RADRAY_IS_DEBUG`, never `NDEBUG` / `_DEBUG`.
- String formatting goes through `fmt`. Check for an existing `format_to` before adding one.
- Flag-style enums use `enum_flags.h` (`EnumFlags<T>`, `is_flags<T>`, `format_as`).
- **Never rename an existing enum member.** Member names are public identifiers consumed by
  `magic_enum` and serialized data. Add a new member and migrate data explicitly instead.

### Exceptions
- Never add `try`/`catch` just to keep a `noexcept` declaration. If an exception cannot be
  meaningfully handled, let it propagate or let `std::terminate` fire at the `noexcept` boundary.
- Catch only specific, recoverable exceptions. Never use `catch (...)` to turn allocation
  failures, programming errors, or invariant violations into `false` / `nullopt` / a diagnostic.
- Avoid `throw` and exception-based control flow. Prefer validation, `std::error_code`,
  or the existing result types.
- **Ask the user before introducing any new `try`, `catch`, or `throw`** — even when it looks necessary.

### Layering
- Dependency chain: `core` ← `shader` ← `window`, `core` ← `shader` ← `shadercompiler`,
  `core` ← `shader` ← `render` ← `runtime`.
- `radrayshader` holds the compiler/render shared shader wire contract and artifact decoder;
  it must not depend on DXC headers/binaries or on `radrayrender`/`radrayruntime`.
- The `radrayshadercompiler` client is optional and sits beside render/runtime; it must
  not make `radrayrender` or `radrayruntime` depend on compiler SDK headers or binaries.
  Verify target edges with `link /MAP` or `ninja -C build_debug -t commands`, not
  `dumpbin /DEPENDENTS` (Vulkan loads via volk).
- `third_party/` and `SDKs/` are script-populated read-only trees. Do not edit them.

### Shaders
- `shaderlib/` is the HLSL include root. Every include is root-relative and angle-bracketed:
  `#include <core/math.hlsli>` — not `<shaderlib/core/math.hlsli>`, not `<math.hlsli>`.
  DXC accepts file-relative form but the source-identity scanner does not, so it breaks cook/JIT.
  Reserve `""` for genuinely path-relative includes, which currently do not exist.
- `.hlsl` = entry point (`VSMain`/`PSMain`/`CSMain`); `.hlsli` = include-guarded library header.
- `shaderlib/core/platform.hlsli` is the only target-gate shim. New passes must follow the
  compiler-owned contract defined by the TODO plan, use shaderlib-root-relative includes such as
  `#include <core/math.hlsli>`, and pass binding numbers directly through `VK_BINDING(binding, set)`;
  do not add a parallel metadata format or numbered binding wrapper.

### Tests
- Test sources go in `modules/<module>/tests/`, registered via `radray_add_test` or
  `radray_add_radray_gtest_case`.
- `ctest -R` matches the **gtest suite name** (the C++ class), not the cmake target name.
- Do not run build and test concurrently.

## Commands

```powershell
python tools/fetch_third_party.py restore
python tools/fetch_sdks.py restore
cmake --preset win-x64-debug
cmake --build build_debug --parallel 24
ctest --test-dir build_debug -C Debug -R <SuiteName> --output-on-failure
python tools/win_gen_compile_commands.py --build-dir build_debug --configuration Debug
```

Binaries land in `build_debug/_build/<Config>/`. Details and the target→suite table are in
`docs/guide/build-test.md`.
