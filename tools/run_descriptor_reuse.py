"""Freeze and run the descriptor workload in sequential, isolated GPU processes."""

import argparse
import csv
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import sys
import zipfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cases(quick):
    if quick:
        return [(scene, 32, 3, 3) for scene in ("shared", "color-depth")]
    result = [(scene, 32, 3, 3) for scene in (
        "shared", "independent", "color-depth", "compatible-color", "equal-override",
        "changed-override", "visible-order", "peak-drop", "mixed")]
    for material in (1, 300):
        result.extend((scene, material, 3, 3) for scene in ("shared", "independent"))
    for views in (1, 6):
        result.extend((scene, 32, views, 3) for scene in ("shared", "independent"))
    for flights in (1, 2):
        result.extend((scene, 32, 3, flights) for scene in ("shared", "independent"))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build_debug"))
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--backends", nargs="+", choices=("d3d12", "vulkan"), default=["d3d12", "vulkan"])
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.exists():
        parser.error("Use a new output directory: existing evidence is never overwritten")
    output.mkdir(parents=True)
    executable = args.exe.resolve()
    binary_dir = output / "bin"
    binary_dir.mkdir()
    shutil.copy2(executable, binary_dir / executable.name)
    for path in executable.parent.glob("*.dll"):
        shutil.copy2(path, binary_dir / path.name)
    compiler_dir = root / "SDKs/radray_dxc/extracted/bin"
    for name in ("dxcompiler.dll", "dxil.dll"):
        shutil.copy2(compiler_dir / name, binary_dir / name)
    shaders = output / "shaders"
    shaders.mkdir()
    for path in (root / "examples/descriptor_reuse").glob("*.hlsl"):
        shutil.copy2(path, shaders / path.name)
    shutil.copytree(root / "shaderlib/core", shaders / "shaderlib/core")
    build = args.build_dir.resolve()
    shutil.copy2(build / "CMakeCache.txt", output / "CMakeCache.txt")
    for path in (build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"):
        shutil.copy2(path, output / "CMakeCXXCompiler.cmake")
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    tracked += subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=root).decode().split("\0")
    source = {root / name for name in tracked if name and
              (name.startswith(("modules/", "examples/", "benchmarks/", "shaderlib/", "cmake/")) or name in
               ("CMakeLists.txt", "CMakePresets.json", "project_manifest.json", "project_manifest.lock.json", "AGENTS.md"))}
    source.update((root / "examples/descriptor_reuse").glob("*"))
    source.add(pathlib.Path(__file__).resolve())
    source = sorted(path for path in source if path.is_file())
    with zipfile.ZipFile(output / "source.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for path in source:
            archive.write(path, path.relative_to(root))
    patch = subprocess.check_output(["git", "diff", "--binary", "--", "modules", "examples", "benchmarks", "shaderlib", "cmake", "CMakeLists.txt"], cwd=root)
    (output / "workspace.patch").write_bytes(patch)
    manifest = {
        "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root).decode().strip(),
        "git_status": subprocess.check_output(["git", "status", "--short"], cwd=root).decode().splitlines(),
        "invocation": sys.argv,
        "platform": platform.platform(),
        "python": sys.version,
        "cmake": subprocess.check_output(["cmake", "--version"]).decode(),
        "build_command": 'VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build build_debug --config Debug --target example_descriptor_reuse --parallel 4',
        "source_sha256": {str(path.relative_to(root)).replace("\\", "/"): sha256(path) for path in source},
        "package_sha256": {str(path.relative_to(output)).replace("\\", "/"): sha256(path) for path in output.rglob("*") if path.is_file()},
        "cases": [],
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    rows = []
    failed = 0
    for backend in args.backends:
        for scene, materials, views, flights in cases(args.quick):
            name = f"{backend}-{scene}-m{materials}-v{views}-f{flights}"
            path = output / f"{name}.json"
            command = [str(binary_dir / executable.name), f"--backend={backend}", f"--scene={scene}",
                       f"--materials={materials}", f"--views={views}", f"--flights={flights}",
                       f"--shader-root={shaders}", f"--compiler={binary_dir / 'dxcompiler.dll'}",
                       f"--artifacts={output / 'artifacts' / name}", f"--output={path}"]
            with (output / f"{name}.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=180)
            entry = {"name": name, "command": command, "returncode": result.returncode}
            manifest["cases"].append(entry)
            passed = result.returncode == 0 and path.is_file()
            if path.is_file():
                data = json.loads(path.read_text(encoding="utf-8"))
                passed = passed and data["passed"]
                for snapshot in data.get("snapshots", []):
                    rows.append({"backend": backend, "scene": scene, "materials": materials, "views": views,
                                 "flights": flights, "passed": passed, **{k: v for k, v in snapshot.items() if k != "vulkan_types_live"}})
            failed += not passed
            print(f"{'PASS' if passed else 'FAIL'} {name}", flush=True)
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    manifest["result_sha256"] = {str(path.relative_to(output)).replace("\\", "/"): sha256(path)
                                 for path in output.rglob("*") if path.is_file() and path.name != "manifest.json"}
    manifest["failed"] = failed
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if rows:
        with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
