"""Generate local DXIL, SPIR-V and metadata fixtures; never download dependencies."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from compile_attachment_fixtures import compile_fixtures


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hash(root):
    files = [root / "tools/compile_test_shaders.py",
             root / "tools/compile_attachment_fixtures.py",
             root / "modules/shader_compiler/tests/shader_fixture_generator.cpp",
             root / "project_manifest.json"]
    for directory in ("shaderlib", "modules/shader/include", "modules/shader/src",
                      "modules/shader_compiler/include", "modules/shader_compiler/src",
                      "modules/shader/tests/data/shader_sources",
                      "modules/shader_compiler/tests/data", "modules/render/tests/data"):
        files.extend(path for path in (root / directory).rglob("*")
                     if path.suffix in (".h", ".cpp", ".hlsl", ".hlsli"))
    digest = hashlib.sha256()
    for path in sorted(files):
        digest.update(path.relative_to(root).as_posix().encode())
        # Git checkouts may use CRLF or LF; the recipe identity is portable.
        digest.update(hashlib.sha256(path.read_text(encoding="utf-8").encode()).digest())
    return digest.hexdigest()


def check_outputs(output, sources, producer=None):
    stamp = output / "fixtures.sha256"
    if not stamp.is_file():
        return "missing fixtures.sha256"
    lines = stamp.read_text(encoding="utf-8").splitlines()
    if len(lines) < 3 or lines[0] != sources:
        return "fixture sources or generation recipe changed"
    if producer is not None and lines[1] != producer:
        return "fixture generator or DXC changed"
    recorded = set()
    for line in lines[2:]:
        digest, separator, name = line.partition(" ")
        relative = Path(name)
        if (not separator or len(digest) != 64 or len(relative.parts) != 2
                or relative.parts[0] not in ("shader_artifacts", "shader_bytecode")
                or relative.suffix != ".bin" or name in recorded):
            return "invalid fixture inventory"
        recorded.add(name)
        path = output / relative
        if not path.is_file() or path.stat().st_size == 0:
            return f"missing or empty {name}"
        if file_hash(path) != digest:
            return f"modified {name}"
    actual = {path.relative_to(output).as_posix()
              for directory in ("shader_artifacts", "shader_bytecode")
              for path in (output / directory).glob("*.bin")}
    if recorded != actual:
        return "fixture inventory changed"
    return None


def read_cache(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and ":" in key and not key.startswith(("//", "#")):
            values[key.split(":", 1)[0]] = value
    return values


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="Configured compiler-enabled RadRay build directory")
    parser.add_argument("--config", default="Debug", choices=("Debug", "Release"))
    parser.add_argument("--output-dir", type=Path, default=root / "build/test_shaders")
    parser.add_argument("--dxc", type=Path, default=root / "SDKs/radray_dxc/extracted/bin/dxc.exe")
    parser.add_argument("--check", action="store_true", help="Check fixtures without building or loading DXC")
    parser.add_argument("--force", action="store_true", help="Recompile even when all inputs and outputs match")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    sources = source_hash(root)
    if args.check:
        problem = check_outputs(output, sources)
        if problem:
            parser.error(f"{problem} in {output}. Generate fixtures first: python tools/compile_test_shaders.py "
                         f'--build-dir <compiler-enabled-build> --config {args.config} --output-dir "{output}". '
                         "Compiler-disabled builds consume these pre-generated files; they do not require DXC.")
        print(f"Test shader fixtures verified: {output}")
        return

    if os.name != "nt":
        parser.error("The RadRay compiler client currently generates fixtures on Windows only. "
                     "Copy a generated fixture directory from Windows and use --check on this host.")
    if args.build_dir is None or not (args.build_dir / "CMakeCache.txt").is_file():
        parser.error("--build-dir must be a configured RadRay directory with "
                     "RADRAY_BUILD_SHADER_COMPILER=ON and RADRAY_BUILD_TESTS=ON. Configure first; no dependencies are downloaded.")
    build = args.build_dir.resolve()
    cache = read_cache(build / "CMakeCache.txt")
    if Path(cache.get("RadRay_SOURCE_DIR", "")).resolve() != root:
        parser.error("--build-dir must belong to this RadRay checkout")
    for option in ("RADRAY_BUILD_SHADER_COMPILER", "RADRAY_BUILD_TESTS"):
        if cache.get(option, "OFF").upper() not in ("ON", "1", "TRUE", "YES"):
            parser.error(f"{option}=ON is required to generate fixtures. Use a separate compiler-enabled build; "
                         "keep the consumer build compiler-disabled.")
    configurations = cache.get("CMAKE_CONFIGURATION_TYPES", cache.get("CMAKE_BUILD_TYPE", "")).split(";")
    if args.config not in configurations:
        parser.error(f"{args.config} is not configured in {build}: {configurations}")
    dxc = args.dxc.resolve()
    compiler_files = [dxc, dxc.parent / "dxcompiler.dll", dxc.parent / "dxil.dll"]
    for path in compiler_files:
        if not path.is_file():
            parser.error(f"Missing DXC package file: {path}. Run python tools/fetch_sdks.py restore manually "
                         "or pass --dxc <RadRay-fork-package>/bin/dxc.exe; stock DXC cannot generate metadata fixtures.")
    cmake = shutil.which("cmake")
    if cmake is None:
        parser.error("cmake is required on PATH to build radray_shader_fixture_generator")
    environment = os.environ.copy()
    environment["PATH"] = str(dxc.parent) + os.pathsep + environment.get("PATH", "")
    result = subprocess.run([cmake, "--build", str(build), "--config", args.config,
                             "--target", "radray_shader_fixture_generator"], env=environment)
    if result.returncode:
        parser.error(f"fixture generator build failed ({result.returncode}); see the build diagnostics above")
    cache = read_cache(build / "CMakeCache.txt")
    generator = Path(cache["RADRAY_BUILD_PATH"]) / args.config / "radray_shader_fixture_generator.exe"
    if not generator.is_file():
        parser.error(f"Missing generator: {generator}")
    for dll in compiler_files[1:]:
        local = generator.parent / dll.name
        if local.is_file() and file_hash(local) != file_hash(dll):
            parser.error(f"Stale executable-local DXC DLL shadows the selected SDK: {local}. Remove it explicitly.")
    producer = hashlib.sha256("".join(file_hash(path) for path in [generator, *compiler_files]).encode()).hexdigest()
    if not args.force and check_outputs(output, sources, producer) is None:
        print(f"Test shaders are up to date: {output}")
        return

    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".generate-", dir=output) as staging:
        staged = Path(staging)
        result = subprocess.run([str(generator), str(staged)], env=environment)
        if result.returncode:
            parser.error(f"fixture generator failed ({result.returncode}); check the RadRay DXC fork ABI/package and diagnostics above")
        if not compile_fixtures(root, dxc, staged / "shader_bytecode", environment):
            parser.error("GPU shader compilation failed; existing fixture bundle was not replaced")
        files = sorted(staged.glob("*/*.bin"))
        if not files or any(path.stat().st_size == 0 for path in files):
            parser.error("generator produced missing or empty fixtures")
        lines = [sources, producer]
        names = set()
        # Publish only after both generators succeed. A partial publish has no valid stamp.
        (output / "fixtures.sha256").unlink(missing_ok=True)
        for path in files:
            relative = path.relative_to(staged)
            names.add(relative.as_posix())
            lines.append(f"{file_hash(path)} {relative.as_posix()}")
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.is_file() or file_hash(destination) != file_hash(path):
                path.replace(destination)
        for directory in ("shader_artifacts", "shader_bytecode"):
            for path in (output / directory).glob("*.bin"):
                if path.relative_to(output).as_posix() not in names:
                    path.unlink()
        (output / "fixtures.sha256").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated {len(files)} test shader fixtures: {output}")


if __name__ == "__main__":
    main()
